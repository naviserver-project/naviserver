/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */


/*
 * tclcache.c --
 *
 *      Tcl cache commands.
 */

#include "nsd.h"

/*
 * The following defines a Tcl cache.
 */

typedef struct TclCache {
    Ns_Cache   *cache;
    Ns_Time     timeout;  /* Default timeout for concurrent updates. */
    Ns_Time     expires;  /* Default time-to-live for cache entries. */
    size_t      maxEntry; /* Maximum size of a single entry in the cache. */
} TclCache;


/*
 * Local functions defined in this file
 */

static int CacheAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, int append);

static Ns_Entry *CreateEntry(const NsInterp *itPtr, TclCache *cPtr, const char *key,
                             int *newPtr, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static void SetEntry(TclCache *cPtr, Ns_Entry *entry, Tcl_Obj *valObj, Ns_Time *expPtr, int cost)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static bool noGlobChars(const char *pattern) 
    NS_GNUC_NONNULL(1);

static Ns_ObjvProc ObjvCache;



/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheCreateObjCmd --
 *
 *      Create a new Tcl cache.
 *
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheCreateObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char     *name = NULL;
    int             result = TCL_OK;
    int             iMaxSize = 0, iMaxEntry = 0;
    const Ns_Time  *timeoutPtr = NULL, *expPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-timeout",  Ns_ObjvTime,  &timeoutPtr, NULL},
        {"-expires",  Ns_ObjvTime,  &expPtr,     NULL},
        {"-maxentry", Ns_ObjvInt,   &iMaxEntry,   NULL},
        {"--",        Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",   Ns_ObjvString, &name,    NULL},
        {"size",    Ns_ObjvInt,    &iMaxSize, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else if (iMaxSize < 0 || iMaxEntry < 0) {
      Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			     "maxsize and maxentry must be positive numbers", NULL);
      result = TCL_ERROR;

    } else {
        const NsInterp *itPtr = arg;
        NsServer       *servPtr = itPtr->servPtr;
        Tcl_HashEntry  *hPtr;
        int             isNew;        

        Ns_MutexLock(&servPtr->tcl.cachelock);
        hPtr = Tcl_CreateHashEntry(&servPtr->tcl.caches, name, &isNew);
        if (isNew != 0) {
            TclCache      *cPtr = ns_calloc(1u, sizeof(TclCache));

            cPtr->cache = Ns_CacheCreateSz(name, TCL_STRING_KEYS, (size_t)iMaxSize, ns_free);
            cPtr->maxEntry = (size_t)iMaxEntry;
            if (timeoutPtr != NULL) {
                cPtr->timeout = *timeoutPtr;
            }
            if (expPtr != NULL) {
                cPtr->expires = *expPtr;
            }
            Tcl_SetHashValue(hPtr, cPtr);
        }
        Ns_MutexUnlock(&servPtr->tcl.cachelock);
        
        if (isNew == 0) {
            Tcl_AppendResult(interp, "duplicate cache name: ", name, NULL);
            result = TCL_ERROR;
        }
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheEvalObjCmd --
 *
 *      Get data from a cache by key.  If key is not present or data
 *      is stale, script is evaluated (with args appended, if present),
 *      and the result is stored in the cache and returned. Script
 *      errors are propagated.
 *
 *      The -force switch causes an existing valid entry to replaced.
 *
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      Other threads may block waiting for this update to complete.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheEvalObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp   *itPtr = arg;
    TclCache   *cPtr;
    const char *key;
    Ns_Time    *timeoutPtr = NULL, *expPtr = NULL;
    int         nargs, isNew, force = (int)NS_FALSE, status;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvTime,  &timeoutPtr, NULL},
        {"-expires", Ns_ObjvTime,  &expPtr,     NULL},
        {"-force",   Ns_ObjvBool,  &force,      INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr,   NULL},
        {"key",      Ns_ObjvString, &key,    NULL},
        {"args",     Ns_ObjvArgs,   &nargs,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    args[0].arg = itPtr; /* pass non-constant itPtr for "cache" */

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        status = TCL_ERROR;

    } else {
        Ns_Entry *entry = CreateEntry(itPtr, cPtr, key, &isNew, timeoutPtr);
        
        if (entry == NULL) {
            status = TCL_ERROR;
            
        } else if (isNew == 0 && force == 0) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_CacheGetValue(entry),
                                                      (int)Ns_CacheGetSize(entry)));
            status = TCL_OK;
            Ns_CacheUnlock(cPtr->cache);

        } else {
            Ns_Time start, end, diff;
            
            Ns_CacheUnlock(cPtr->cache);
            Ns_GetTime(&start);
            
            if (nargs == 1) {
                status = Tcl_EvalObjEx(interp, objv[objc-1], 0);
            } else {
                status = Tcl_EvalObjv(interp, nargs, objv + (objc-nargs), 0);
            }
            Ns_GetTime(&end);
            (void)Ns_DiffTime(&end, &start, &diff);
            
            Ns_CacheLock(cPtr->cache);
            if (status != TCL_OK && status != TCL_RETURN) {

                /* 
                 * Don't cache anything, if the status code is not TCL_OK
                 * or TCL_RETURN.
                 * 
                 * The remaining status codes are TCL_BREAK, TCL_CONTINUE
                 * and TCL_ERROR. Regarding TCL_BREAK and TCL_CONTINUE as
                 * signals for not cacheing is used e.g. in
                 * OpenACS. Therefore we want to return TCL_BREAK or
                 * TCL_CONTINUE as well.
                 *
                 * Certainly, we could map unknown error codes to TCL_ERROR
                 * as it was done in earlier versions of NaviServer.
                 *
                 * if (status != TCL_BREAK && status != TCL_CONTINUE) {
                 *     status = TCL_ERROR;
                 * }
                 */
                
                Ns_CacheDeleteEntry(entry);
            } else {
                status = TCL_OK;
                SetEntry(cPtr, entry, Tcl_GetObjResult(interp), expPtr, 
                         (int)(diff.sec * 1000000 + diff.usec));
            }
            Ns_CacheBroadcast(cPtr->cache);
            Ns_CacheUnlock(cPtr->cache);
        }
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheIncrObjCmd --
 *
 *      Treat the value of the cached object as in integer and
 *      increment it.  New values start at zero.
 *
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheIncrObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp   *itPtr = arg;
    TclCache   *cPtr;
    const char *key;
    int         isNew, incr = 1, result;
    Ns_Time    *timeoutPtr = NULL, *expPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvTime,  &timeoutPtr, NULL},
        {"-expires", Ns_ObjvTime,  &expPtr,     NULL},
        {"--",       Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr,  NULL},
        {"key",      Ns_ObjvString, &key,   NULL},
        {"?incr",    Ns_ObjvInt,    &incr,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    args[0].arg = itPtr; /* pass non-constant itPtr for "cache" */

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_Entry   *entry = CreateEntry(itPtr, cPtr, key, &isNew, timeoutPtr);
        int         cur = 0;
        
        if (entry == NULL) {
            result = TCL_ERROR;
        } else if ((isNew == 0)
                   && (Tcl_GetInt(interp, Ns_CacheGetValue(entry), &cur) != TCL_OK)) {
            Ns_CacheUnlock(cPtr->cache);
            result = TCL_ERROR;
        } else {
            Tcl_Obj    *valObj = Tcl_NewIntObj(cur + incr);
            
            SetEntry(cPtr, entry, valObj, expPtr, 0);
            Tcl_SetObjResult(interp, valObj);
            Ns_CacheUnlock(cPtr->cache);
            result = TCL_OK;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheAppendObjCmd, NsTclCacheLappendObjCmd --
 *
 *      Append one or more elements to cached value.
 *
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return CacheAppendObjCmd(arg, interp, objc, objv, 1);
}

int
NsTclCacheLappendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return CacheAppendObjCmd(arg, interp, objc, objv, 0);
}

static int
CacheAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, int append)
{
    NsInterp   *itPtr = arg;
    TclCache   *cPtr = NULL;
    const char *key = NULL;
    int         result = TCL_OK, nelements = 0;
    Ns_Time    *timeoutPtr = NULL, *expPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvTime, &timeoutPtr, NULL},
        {"-expires", Ns_ObjvTime, &expPtr,     NULL},
        {"--",       Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache", ObjvCache,     &cPtr,      NULL},
        {"key",   Ns_ObjvString, &key,       NULL},
        {"args",  Ns_ObjvArgs,   &nelements, NULL},
        {NULL, NULL, NULL, NULL}
    };
    args[0].arg = itPtr; /* pass non-constant itPtr for "cache" */

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        int         isNew;
        Ns_Entry   *entry = CreateEntry(itPtr, cPtr, key, &isNew, timeoutPtr);
            
        if (entry == NULL) {
            result = TCL_ERROR;
        } else {
            Tcl_Obj  *valObj = Tcl_NewObj();
            int       i;
            
            if (isNew == 0) {
                Tcl_SetStringObj(valObj, Ns_CacheGetValue(entry), 
                                 (int)Ns_CacheGetSize(entry));
            }
            for (i = objc - nelements; i < objc; i++) {
                if (append != 0) {
                    Tcl_AppendObjToObj(valObj, objv[i]);
                } else if (Tcl_ListObjAppendElement(interp, valObj, objv[i]) != TCL_OK) {
                    result = TCL_ERROR;
                    break;
                }
            }
            if (result == TCL_OK) {
                SetEntry(cPtr, entry, valObj, expPtr, 0);
                Tcl_SetObjResult(interp, valObj);
            }
            Ns_CacheUnlock(cPtr->cache);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheNamesObjCmd --
 *
 *      Return a list of Tcl cache names for the current server.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheNamesObjCmd(ClientData arg, Tcl_Interp *interp, int UNUSED(objc), Tcl_Obj *CONST* UNUSED(objv))
{
    const NsInterp      *itPtr = arg;
    NsServer            *servPtr = itPtr->servPtr;
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;

    Ns_MutexLock(&servPtr->tcl.cachelock);
    hPtr = Tcl_FirstHashEntry(&servPtr->tcl.caches, &search);
    while (hPtr != NULL) {
        Tcl_AppendElement(interp, Tcl_GetHashKey(&servPtr->tcl.caches, hPtr));
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&servPtr->tcl.cachelock);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheKeysObjCmd --
 *
 *      Get a list of all valid keys in a cache, or only those matching
 *      pattern, if given.
 *
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
noGlobChars(const char *pattern) 
{
    register char c;
    const char   *p = pattern;
    bool          result = NS_TRUE;

    NS_NONNULL_ASSERT(pattern != NULL);

    for (c = *p; likely(c != '\0'); c = *++p) {
	if (unlikely(c == '*') || unlikely(c == '?') || unlikely(c == '[')) {
            result = NS_FALSE;
            break;
        }
    }
    return result;
}


int
NsTclCacheKeysObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const TclCache *cPtr;
    const Ns_Entry *entry;
    const char     *pattern = NULL;
    int             exact = (int)NS_FALSE, result = TCL_OK;

    Ns_ObjvSpec opts[] = {
        {"-exact",   Ns_ObjvBool,  &exact,     INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr,    NULL},
        {"?pattern", Ns_ObjvString, &pattern, NULL},
        {NULL, NULL, NULL, NULL}
    };
    args[0].arg = arg; /* pass non-constant arg for "cache" */

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (pattern != NULL && (exact != 0 || noGlobChars(pattern))) {

        /*
         * If the provided pattern (key) contains no glob characters,
         * there will be zero or one entry for the given key. In such
         * cases, or when the option "-exact" is specified, a single hash
         * lookup is sufficient.
         */

        Ns_CacheLock(cPtr->cache);
        entry = Ns_CacheFindEntry(cPtr->cache, pattern);
        if (entry != NULL && Ns_CacheGetValue(entry) != NULL) {
            Tcl_AppendElement(interp, pattern);
        }
        Ns_CacheUnlock(cPtr->cache);
        
    } else {
        Ns_DString      ds;
        Ns_CacheSearch  search;
        
        /*
         * We have either no pattern or the pattern contains meta
         * characters. We need to iterate over all entries, which can
         * take a while for large caches.
         */

        Ns_DStringInit(&ds);
        Ns_CacheLock(cPtr->cache);
        entry = Ns_CacheFirstEntry(cPtr->cache, &search);
        while (entry != NULL) {
            const char *key = Ns_CacheKey(entry);

            if (pattern == NULL || Tcl_StringMatch(key, pattern) == 1) {
                Tcl_AppendElement(interp, key);
            }
            entry = Ns_CacheNextEntry(&search);
        }
        Ns_CacheUnlock(cPtr->cache);
        Ns_DStringFree(&ds);
    }
    
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheFlushObjCmd --
 *
 *      Flush all entries from a cache, or the entries identified
 *      by the given keys.  Return the number of entries flushed.
 *      NB: Concurrent updates are skipped.
 *
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheFlushObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const TclCache *cPtr = NULL;
    int             glob = (int)NS_FALSE, npatterns = 0, result = TCL_OK;
    Ns_ObjvSpec     opts[] = {
        {"-glob",    Ns_ObjvBool,  &glob, INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak, NULL,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     args[] = {
        {"cache",    ObjvCache,    &cPtr,      NULL},
        {"?args",    Ns_ObjvArgs,  &npatterns, NULL},
        {NULL, NULL, NULL, NULL}
    };
    args[0].arg = arg; /* pass non-constant arg for "cache" */

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_Entry  *entry;
        int        nflushed = 0, i;
        Ns_Cache  *cache;

        assert(cPtr != NULL);
        cache = cPtr->cache;

        Ns_CacheLock(cache);
        if (npatterns == 0) {
            nflushed = Ns_CacheFlush(cache);

        } else if (glob == 0) {
            for (i = npatterns; i > 0; i--) {
                entry = Ns_CacheFindEntry(cache, Tcl_GetString(objv[objc-i]));
                if (entry != NULL && Ns_CacheGetValue(entry) != NULL) {
                    Ns_CacheFlushEntry(entry);
                    nflushed++;
                }
            }
            
        } else {
            Ns_CacheSearch  search;
                
            entry = Ns_CacheFirstEntry(cache, &search);
            while (entry != NULL) {
                const char *key = Ns_CacheKey(entry);

                for (i = npatterns; i > 0; i--) {
                    const char *pattern = Tcl_GetString(objv[objc-i]);

                    if (Tcl_StringMatch(key, pattern) == 1) {
                        Ns_CacheFlushEntry(entry);
                        nflushed++;
                        break;
                    }
                }
                entry = Ns_CacheNextEntry(&search);
            }
        }
        Ns_CacheUnlock(cache);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(nflushed));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheGetObjCmd --
 *
 *      Return an entry from the cache. This function behaves
 *      similar to nsv_get; if the optional varname is passed,
 *      it returns 0 or 1 depending on succes and bind the variable
 *      on success. If no varName is provided, it returns the value
 *      or an error.
 *
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCacheGetObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const TclCache *cPtr = NULL;
    const char     *key;
    int             result = TCL_OK;
    Tcl_Obj        *varNameObj = NULL;
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr,        NULL},
        {"key",      Ns_ObjvString, &key,         NULL},
        {"?varName", Ns_ObjvObj,    &varNameObj,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    args[0].arg = arg; /* pass non-constant arg for "cache" */

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Ns_Entry *entry;
        Tcl_Obj        *resultObj;
        
        assert(cPtr != NULL);

        Ns_CacheLock(cPtr->cache);
        entry = Ns_CacheFindEntry(cPtr->cache, key);
        resultObj = (entry != NULL) ? Tcl_NewStringObj(Ns_CacheGetValue(entry), -1) : NULL;
        Ns_CacheUnlock(cPtr->cache);

        if (unlikely(varNameObj != NULL)) {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(resultObj != NULL));
            if (likely(resultObj != NULL)) {
                if (unlikely(Tcl_ObjSetVar2(interp, varNameObj, NULL, resultObj, TCL_LEAVE_ERR_MSG) == NULL)) {
                    result = TCL_ERROR;
                }
            }
        } else {
            if (likely(resultObj != NULL)) {
                Tcl_SetObjResult(interp, resultObj);
            } else {
                Tcl_AppendResult(interp, "no such key: ", key, NULL);
                result = TCL_ERROR;
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheStatsObjCmd --
 *
 *      Returns stats on a cache. The size and expirey time of each
 *      entry in the cache is also appended if the -contents switch
 *      is given.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheStatsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const TclCache *cPtr = NULL;
    int             contents = (int)NS_FALSE, reset = (int)NS_FALSE, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-contents", Ns_ObjvBool,  &contents, INT2PTR(NS_TRUE)},
        {"-reset",    Ns_ObjvBool,  &reset,    INT2PTR(NS_TRUE)},
        {"--",        Ns_ObjvBreak, NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache", ObjvCache, &cPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    args[0].arg = arg; /* pass non-constant arg for "cache" */

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        Ns_DString      ds;
        Ns_Cache       *cache;

        assert(cPtr != NULL);
        
        cache = cPtr->cache;
        Ns_DStringInit(&ds);

        Ns_CacheLock(cache);
        if (contents != 0) {
            Ns_CacheSearch  search;
            const Ns_Entry *entry;

            Tcl_DStringStartSublist(&ds);
            entry = Ns_CacheFirstEntry(cache, &search);
            while (entry != NULL) {
                size_t         size    = Ns_CacheGetSize(entry);
                const Ns_Time *timePtr = Ns_CacheGetExpirey(entry);

                if (timePtr->usec == 0) {
                    Ns_DStringPrintf(&ds, "%" PRIdz " %" PRIu64 " ",
                                     size, (int64_t) timePtr->sec);
                } else {
                    Ns_DStringPrintf(&ds, "%" PRIdz " %" PRIu64 ":%ld ",
                                     size, (int64_t) timePtr->sec, timePtr->usec);
                }
                entry = Ns_CacheNextEntry(&search);
            }
            Tcl_DStringEndSublist(&ds);
        } else {
            (void) Ns_CacheStats(cache, &ds);
        }
        if (reset != 0) {
            Ns_CacheResetStats(cache);
        }
        Ns_CacheUnlock(cache);

        Tcl_DStringResult(interp, &ds);
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CreateEntry --
 *
 *      Lock the cache and create a new entry or return existing entry,
 *      waiting up to timeout seconds for another thread to complete
 *      an update.
 *
 * Results:
 *      Pointer to entry, or NULL on timeout.
 *
 * Side effects:
 *      Cache will be left locked if function returns non-NULL entry.
 *
 *----------------------------------------------------------------------
 */

static Ns_Entry *
CreateEntry(const NsInterp *itPtr, TclCache *cPtr, const char *key, int *newPtr,
            Ns_Time *timeoutPtr)
{
    Ns_Cache *cache;
    Ns_Entry *entry;
    Ns_Time   t;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(cPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(newPtr != NULL);

    cache = cPtr->cache;

    if (timeoutPtr == NULL
        && (cPtr->timeout.sec > 0 || cPtr->timeout.usec > 0)) {
        timeoutPtr = Ns_AbsoluteTime(&t, &cPtr->timeout);
    } else {
        timeoutPtr = Ns_AbsoluteTime(&t, timeoutPtr);
    }
    Ns_CacheLock(cache);
    entry = Ns_CacheWaitCreateEntry(cache, key, newPtr, timeoutPtr);
    if (entry == NULL) {
        Ns_CacheUnlock(cache);
        Tcl_SetErrorCode(itPtr->interp, "NS_TIMEOUT", NULL);
        Tcl_AppendResult(itPtr->interp, "timeout waiting for concurrent update: ",
                         key, NULL);
    }
    return entry;
}


/*
 *----------------------------------------------------------------------
 *
 * SetEntry --
 *
 *      Set the value of the cache entry if not above max entry size.
 *
 * Results:
 *      1 if entry set, 0 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
SetEntry(TclCache *cPtr, Ns_Entry *entry, Tcl_Obj *valObj, Ns_Time *expPtr, int cost)
{
    const char *bytes;
    int         len;
    size_t      length;
    Ns_Time     t;

    NS_NONNULL_ASSERT(cPtr != NULL);
    NS_NONNULL_ASSERT(entry != NULL);
    NS_NONNULL_ASSERT(valObj != NULL);

    bytes = Tcl_GetStringFromObj(valObj, &len);
    assert(len >= 0);
    length = (size_t)len;

    if (cPtr->maxEntry > 0u && length > cPtr->maxEntry) {
        Ns_CacheDeleteEntry(entry);
    } else {
        char *value = ns_malloc(length + 1u);

        memcpy(value, bytes, length);
        value[length] = '\0';
        if (expPtr == NULL
            && (cPtr->expires.sec > 0 || cPtr->expires.usec > 0)) {
            expPtr = Ns_AbsoluteTime(&t, &cPtr->expires);
        } else {
            expPtr = Ns_AbsoluteTime(&t, expPtr);
        }
        Ns_CacheSetValueExpires(entry, value, length, expPtr, cost);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ObjvCache --
 *
 *      Get a cache by name.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Tcl obj will be converted to ns:cache type.
 *
 *----------------------------------------------------------------------
 */

static int
ObjvCache(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
          Tcl_Obj *CONST* objv)
{
    int                 result = TCL_OK;
    TclCache          **cPtrPtr = spec->dest;

    if (unlikely(*objcPtr < 1)) {
        result = TCL_ERROR;

    } else {
        static const char  *const cacheType = "ns:cache";

        if (unlikely(Ns_TclGetOpaqueFromObj(objv[0], cacheType, (void **)cPtrPtr) != TCL_OK)) {
            const NsInterp      *itPtr   = spec->arg;
            NsServer            *servPtr = itPtr->servPtr;
            const Tcl_HashEntry *hPtr;

            Ns_MutexLock(&servPtr->tcl.cachelock);
            hPtr = Tcl_FindHashEntry(&servPtr->tcl.caches, Tcl_GetString(objv[0]));
            Ns_MutexUnlock(&servPtr->tcl.cachelock);
            if (hPtr == NULL) {
                Tcl_AppendResult(interp, "no such cache: ", Tcl_GetString(objv[0]), NULL);
                result = TCL_ERROR;
            } else {
                *cPtrPtr = Tcl_GetHashValue(hPtr);
                Ns_TclSetOpaqueObj(objv[0], cacheType, *cPtrPtr);
            }
        }
        if (result == TCL_OK) {
            *objcPtr -= 1;
        }
    }

    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
