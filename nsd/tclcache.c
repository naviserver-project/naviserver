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

static int CacheAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                             Tcl_Obj *CONST objv[], int append);
static Ns_Entry *CreateEntry(NsInterp *itPtr, TclCache *cPtr, char *key,
                             int *newPtr, Ns_Time *timeoutPtr);
static void SetEntry(TclCache *cPtr, Ns_Entry *entry, Tcl_Obj *valObj, Ns_Time *expPtr, int cost);
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
NsTclCacheCreateObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp      *itPtr = arg;
    NsServer      *servPtr = itPtr->servPtr;
    Tcl_HashEntry *hPtr;
    char          *name = NULL;
    int            isNew, maxSize = 0, maxEntry = 0;
    Ns_Time       *timeoutPtr = NULL, *expPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-timeout",  Ns_ObjvTime,  &timeoutPtr, NULL},
        {"-expires",  Ns_ObjvTime,  &expPtr,     NULL},
        {"-maxentry", Ns_ObjvInt,   &maxEntry,   NULL},
        {"--",        Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",   Ns_ObjvString, &name,    NULL},
        {"size",    Ns_ObjvInt,    &maxSize, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_MutexLock(&servPtr->tcl.cachelock);
    hPtr = Tcl_CreateHashEntry(&servPtr->tcl.caches, name, &isNew);
    if (isNew) {
        TclCache *cPtr = ns_calloc(1, sizeof(TclCache));

        cPtr->cache = Ns_CacheCreateSz(name, TCL_STRING_KEYS, maxSize, ns_free);
        cPtr->maxEntry = maxEntry;
        if (timeoutPtr != NULL) {
            cPtr->timeout = *timeoutPtr;
        }
        if (expPtr != NULL) {
            cPtr->expires = *expPtr;
        }
        Tcl_SetHashValue(hPtr, cPtr);
    }
    Ns_MutexUnlock(&servPtr->tcl.cachelock);

    if (!isNew) {
        Tcl_AppendResult(interp, "duplicate cache name: ", name, NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
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
NsTclCacheEvalObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    TclCache *cPtr;
    Ns_Entry *entry;
    char     *key;
    Ns_Time  *timeoutPtr = NULL, *expPtr = NULL;
    int       nargs, isNew, force = NS_FALSE, status = TCL_OK;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvTime,  &timeoutPtr, NULL},
        {"-expires", Ns_ObjvTime,  &expPtr,     NULL},
        {"-force",   Ns_ObjvBool,  &force,      (void *) NS_TRUE},
        {"--",       Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr,   itPtr},
        {"key",      Ns_ObjvString, &key,    NULL},
        {"args",     Ns_ObjvArgs,   &nargs,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if ((entry = CreateEntry(itPtr, cPtr, key, &isNew, timeoutPtr)) == NULL) {
        return TCL_ERROR;
    }
    if (!isNew && !force) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_CacheGetValue(entry),
                                                  (int)Ns_CacheGetSize(entry)));
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
        Ns_DiffTime(&end, &start, &diff);

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
    }
    Ns_CacheUnlock(cPtr->cache);

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
NsTclCacheIncrObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    TclCache *cPtr;
    Ns_Entry *entry;
    Tcl_Obj  *valObj;
    char     *key;
    int       isNew, cur, incr = 1;
    Ns_Time  *timeoutPtr = NULL, *expPtr = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvTime,  &timeoutPtr, NULL},
        {"-expires", Ns_ObjvTime,  &expPtr,     NULL},
        {"--",       Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr, itPtr},
        {"key",      Ns_ObjvString, &key,   NULL},
        {"?incr",    Ns_ObjvInt,    &incr,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if ((entry = CreateEntry(itPtr, cPtr, key, &isNew, timeoutPtr)) == NULL) {
        return TCL_ERROR;
    }
    if (isNew) {
        cur = 0;
    } else if (Tcl_GetInt(interp, Ns_CacheGetValue(entry), &cur) != TCL_OK) {
        Ns_CacheUnlock(cPtr->cache);
        return TCL_ERROR;
    }
    valObj = Tcl_NewIntObj(cur += incr);
    SetEntry(cPtr, entry, valObj, expPtr, 0);
    Tcl_SetObjResult(interp, valObj);
    Ns_CacheUnlock(cPtr->cache);

    return TCL_OK;
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
NsTclCacheAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return CacheAppendObjCmd(arg, interp, objc, objv, 1);
}

int
NsTclCacheLappendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return CacheAppendObjCmd(arg, interp, objc, objv, 0);
}

static int
CacheAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                  Tcl_Obj *CONST objv[], int append)
{
    NsInterp *itPtr = arg;
    TclCache *cPtr;
    Ns_Entry *entry;
    Tcl_Obj  *valObj;
    char     *key;
    int       i, isNew, nelements;
    Ns_Time  *timeoutPtr = NULL, *expPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvTime, &timeoutPtr, NULL},
        {"-expires", Ns_ObjvTime, &expPtr,     NULL},
        {"--",       Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache", ObjvCache,     &cPtr,      itPtr},
        {"key",   Ns_ObjvString, &key,       NULL},
        {"args",  Ns_ObjvArgs,   &nelements, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if ((entry = CreateEntry(itPtr, cPtr, key, &isNew, timeoutPtr)) == NULL) {
        return TCL_ERROR;
    }
    valObj = Tcl_NewObj();
    if (!isNew) {
        Tcl_SetStringObj(valObj, Ns_CacheGetValue(entry), 
			 (int)Ns_CacheGetSize(entry));
    }
    for (i = objc - nelements; i < objc; i++) {
        if (append) {
            Tcl_AppendObjToObj(valObj, objv[i]);
        } else if (Tcl_ListObjAppendElement(interp, valObj, objv[i])
                   != TCL_OK) {
            Ns_CacheUnlock(cPtr->cache);
            return TCL_ERROR;
        }
    }
    SetEntry(cPtr, entry, valObj, expPtr, 0);
    Tcl_SetObjResult(interp, valObj);
    Ns_CacheUnlock(cPtr->cache);

    return TCL_OK;
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
NsTclCacheNamesObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp       *itPtr = arg;
    NsServer       *servPtr = itPtr->servPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

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
static int
noGlobChars(CONST char *pattern) 
{
    register char c;
    CONST char *p = pattern;

    for (c = *p; likely(c); c = *++p) {
	if (unlikely(c == '*') || unlikely(c == '?') || unlikely(c == '[')) {
            return 0;
        }
    }
    return 1;
}


int
NsTclCacheKeysObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    TclCache       *cPtr;
    Ns_Entry       *entry;
    Ns_CacheSearch  search;
    char           *pattern = NULL;
    Ns_DString      ds;
    int             exact = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-exact",   Ns_ObjvBool,  &exact,     (void *) NS_TRUE},
        {"--",       Ns_ObjvBreak, NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr,    arg},
        {"?pattern", Ns_ObjvString, &pattern, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    /*
     * If the provided pattern (key) contains no glob characters,
     * there will be zero or one entry for the given key. In such
     * cases, or when the option "-exact" is specified, a single hash
     * lookup is sufficient.
     */
    if (pattern && (exact || noGlobChars(pattern))) {
        Ns_CacheLock(cPtr->cache);
        entry = Ns_CacheFindEntry(cPtr->cache, pattern);
        if (entry != NULL && Ns_CacheGetValue(entry) != NULL) {
            Tcl_AppendElement(interp, pattern);
        }
        Ns_CacheUnlock(cPtr->cache);
    } else {
        /*
         * We have either no pattern or the pattern contains meta
         * characters. We need to iterate over all entries, which can
         * take a while for large caches.
         */

        Ns_DStringInit(&ds);
        Ns_CacheLock(cPtr->cache);
        entry = Ns_CacheFirstEntry(cPtr->cache, &search);
        while (entry != NULL) {
            char *key = Ns_CacheKey(entry);

            if (pattern == NULL || Tcl_StringMatch(key, pattern)) {
                Tcl_AppendElement(interp, key);
            }
            entry = Ns_CacheNextEntry(&search);
        }
        Ns_CacheUnlock(cPtr->cache);
        Ns_DStringFree(&ds);
    }
    return TCL_OK;
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
NsTclCacheFlushObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    TclCache       *cPtr = NULL;
    Ns_Cache       *cache;
    Ns_Entry       *entry;
    Ns_CacheSearch  search;
    int             i, nflushed = 0, glob = NS_FALSE, npatterns = 0;

    Ns_ObjvSpec opts[] = {
        {"-glob",    Ns_ObjvBool,  &glob, (void *) NS_TRUE},
        {"--",       Ns_ObjvBreak, NULL,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,    &cPtr,      arg},
        {"?args",    Ns_ObjvArgs,  &npatterns, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    assert(cPtr);
    cache = cPtr->cache;

    Ns_CacheLock(cache);
    if (npatterns == 0) {
        nflushed = Ns_CacheFlush(cache);
    } else if (!glob) {
        for (i = npatterns; i > 0; i--) {
            entry = Ns_CacheFindEntry(cache, Tcl_GetString(objv[objc-i]));
            if (entry != NULL && Ns_CacheGetValue(entry) != NULL) {
                Ns_CacheFlushEntry(entry);
                nflushed++;
            }
        }
    } else {
        entry = Ns_CacheFirstEntry(cache, &search);
        while (entry != NULL) {
            char *key = Ns_CacheKey(entry);

            for (i = npatterns; i > 0; i--) {
	        char *pattern = Tcl_GetString(objv[objc-i]);

                if (Tcl_StringMatch(key, pattern)) {
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

    return TCL_OK;
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
NsTclCacheGetObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    TclCache       *cPtr = NULL;
    Ns_Entry       *entry;
    char           *key;
    Tcl_Obj        *varNameObj = NULL, *resultObj;

    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cPtr,        arg},
        {"key",      Ns_ObjvString, &key,         NULL},
        {"?varName", Ns_ObjvObj,    &varNameObj,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    assert(cPtr != NULL);

    Ns_CacheLock(cPtr->cache);
    entry = Ns_CacheFindEntry(cPtr->cache, key);
    resultObj = entry ? Tcl_NewStringObj(Ns_CacheGetValue(entry), -1) : NULL;
    Ns_CacheUnlock(cPtr->cache);

    if (varNameObj) {
	Tcl_SetObjResult(interp, Tcl_NewIntObj(resultObj != NULL));
	if (resultObj) {
	    Tcl_ObjSetVar2(interp, varNameObj, NULL, resultObj, 0);
	}
    } else {
	if (resultObj) {
	    Tcl_SetObjResult(interp, resultObj);
	} else {
	    Tcl_AppendResult(interp, "no such key: ",
			     Tcl_GetString(objv[2]), NULL);
	    return TCL_ERROR;
	}
    }
    return TCL_OK;
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
NsTclCacheStatsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    TclCache       *cPtr = NULL;
    Ns_Cache       *cache;
    Ns_CacheSearch  search;
    Ns_DString      ds;
    int             contents = NS_FALSE, reset = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-contents", Ns_ObjvBool,  &contents, (void *) NS_TRUE},
        {"-reset",    Ns_ObjvBool,  &reset,    (void *) NS_TRUE},
        {"--",        Ns_ObjvBreak, NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache", ObjvCache, &cPtr, arg},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    assert(cPtr);
    cache = cPtr->cache;
    Ns_DStringInit(&ds);

    Ns_CacheLock(cache);
    if (contents) {
        Ns_Entry *entry;

        Tcl_DStringStartSublist(&ds);
        entry = Ns_CacheFirstEntry(cache, &search);
        while (entry != NULL) {
	    size_t   size = Ns_CacheGetSize(entry);
	    Ns_Time *timePtr = Ns_CacheGetExpirey(entry);

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
        Ns_CacheStats(cache, &ds);
    }
    if (reset) {
        Ns_CacheResetStats(cache);
    }
    Ns_CacheUnlock(cache);

    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
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
 *      Cache will be left locked if function returns entry.
 *
 *----------------------------------------------------------------------
 */

static Ns_Entry *
CreateEntry(NsInterp *itPtr, TclCache *cPtr, char *key, int *newPtr,
            Ns_Time *timeoutPtr)
{
    Ns_Cache *cache = cPtr->cache;
    Ns_Entry *entry;
    Ns_Time   time;

    if (timeoutPtr == NULL
        && (cPtr->timeout.sec > 0 || cPtr->timeout.usec > 0)) {
        timeoutPtr = Ns_AbsoluteTime(&time, &cPtr->timeout);
    } else {
        timeoutPtr = Ns_AbsoluteTime(&time, timeoutPtr);
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
    char    *string;
    int      len;
    Ns_Time  time;

    string = Tcl_GetStringFromObj(valObj, &len);
    if (cPtr->maxEntry > 0 && (size_t)len > cPtr->maxEntry) {
        Ns_CacheDeleteEntry(entry);
    } else {
        char *value = ns_malloc(len+1);

        memcpy(value, string, len);
        value[len] = '\0';
        if (expPtr == NULL
            && (cPtr->expires.sec > 0 || cPtr->expires.usec > 0)) {
            expPtr = Ns_AbsoluteTime(&time, &cPtr->expires);
        } else {
            expPtr = Ns_AbsoluteTime(&time, expPtr);
        }
        Ns_CacheSetValueExpires(entry, value, len, expPtr, cost);
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
          Tcl_Obj *CONST objv[])
{
    TclCache          **cPtrPtr = spec->dest;
    static CONST char  *cacheType = "ns:cache";

    if (unlikely(*objcPtr < 1)) {
        return TCL_ERROR;
    }
    if (unlikely(Ns_TclGetOpaqueFromObj(objv[0], cacheType, (void **) cPtrPtr) != TCL_OK)) {
	NsInterp      *itPtr   = spec->arg;
	NsServer      *servPtr = itPtr->servPtr;
        Tcl_HashEntry *hPtr;

        Ns_MutexLock(&servPtr->tcl.cachelock);
        hPtr = Tcl_FindHashEntry(&servPtr->tcl.caches, Tcl_GetString(objv[0]));
        Ns_MutexUnlock(&servPtr->tcl.cachelock);
        if (hPtr == NULL) {
            Tcl_AppendResult(interp, "no such cache: ", Tcl_GetString(objv[0]), NULL);
            return TCL_ERROR;
        }
        *cPtrPtr = Tcl_GetHashValue(hPtr);
        Ns_TclSetOpaqueObj(objv[0], cacheType, *cPtrPtr);
    }
    *objcPtr -= 1;

    return TCL_OK;
}
