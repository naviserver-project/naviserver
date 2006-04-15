/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
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

NS_RCSID("@(#) $Header$");


/*
 * Local functions defined in this file
 */

static int CacheAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                             Tcl_Obj *CONST objv[], int append);
static Ns_Entry *CreateEntry(NsInterp *itPtr, Ns_Cache *cache, char *key,
                             int *newPtr, int timeout);
static void SetEntry(Tcl_Interp *interp, Ns_Entry *entry, Tcl_Obj *valObj,
                     int ttl);
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
    Ns_Cache      *cache;
    char          *name;
    int            new, maxSize, ttl = 0;

    Ns_ObjvSpec opts[] = {
        {"-ttl",     Ns_ObjvInt,   &ttl, NULL},
        {"--",       Ns_ObjvBreak, NULL,     NULL},
        {NULL, NULL, NULL, NULL}
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
    hPtr = Tcl_CreateHashEntry(&servPtr->tcl.caches, name, &new);
    if (new) {
        cache = Ns_CacheCreateEx(name, TCL_STRING_KEYS,
                                 ttl, maxSize, ns_free);
        Tcl_SetHashValue(hPtr, cache);
    }
    Ns_MutexUnlock(&servPtr->tcl.cachelock);

    if (!new) {
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
 * Results:
 *      TCL result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheEvalObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    Ns_Cache *cache;
    Ns_Entry *entry;
    char     *key;
    int       new, status = TCL_OK;
    int       nargs, timeout = -1, ttl = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvInt,   &timeout, NULL},
        {"-ttl",     Ns_ObjvInt,   &ttl,     NULL},
        {"--",       Ns_ObjvBreak, NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cache,  arg},
        {"key",      Ns_ObjvString, &key,    NULL},
        {"args",     Ns_ObjvArgs,   &nargs,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if ((entry = CreateEntry(itPtr, cache, key, &new, timeout)) == NULL) {
        return TCL_ERROR;
    }
    if (!new) {
        Tcl_SetStringObj(Tcl_GetObjResult(interp),
                         Ns_CacheGetValue(entry), Ns_CacheGetSize(entry));
    } else {
        Ns_CacheUnlock(cache);
        if (nargs == 1) {
            status = Tcl_EvalObjEx(interp, objv[objc-1], 0);
        } else {
            status = Tcl_EvalObjv(interp, nargs, objv + (objc-nargs), 0);
        }
        Ns_CacheLock(cache);
        entry = Ns_CacheCreateEntry(cache, key, &new);
        if (status != TCL_OK && status != TCL_RETURN) {
            status = TCL_ERROR;
            Ns_CacheUnsetValue(entry);
            Ns_CacheDeleteEntry(entry);
        } else {
            status = TCL_OK;
            SetEntry(interp, entry, NULL, ttl);
        }
        Ns_CacheBroadcast(cache);
    }
    Ns_CacheUnlock(cache);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheIncrObjCmd --
 *
 *      Treat the value of the cached object as in integer and
 *      increment it.  No value is treated as starting at zero.a
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
    Ns_Cache *cache;
    Ns_Entry *entry;
    char     *key;
    int       new, cur, incr = 1;
    int       timeout = -1, ttl = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvInt,   &timeout, NULL},
        {"-ttl",     Ns_ObjvInt,   &ttl,     NULL},
        {"--",       Ns_ObjvBreak, NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cache, arg},
        {"key",      Ns_ObjvString, &key,   NULL},
        {"?incr",    Ns_ObjvInt,    &incr,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if ((entry = CreateEntry(itPtr, cache, key, &new, timeout)) == NULL) {
        return TCL_ERROR;
    }
    if (new) {
        cur = 0;
    } else if (Tcl_GetInt(interp, Ns_CacheGetValue(entry), &cur) != TCL_OK) {
        Ns_CacheUnlock(cache);
        return TCL_ERROR;
    }
    SetEntry(interp, entry, Tcl_NewIntObj(cur + incr), ttl);
    Ns_CacheUnlock(cache);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheExistsObjCmd --
 *
 *      Returns 1 if entry exists in the cache and not expired yet
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
NsTclCacheExistsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Cache *cache;
    Ns_Entry *entry;
    char     *key;
    int       rc = 0;

    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cache, arg},
        {"key",      Ns_ObjvString, &key,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    Ns_CacheLock(cache);
    if ((entry = Ns_CacheFindEntry(cache, key)) != NULL) {
        rc = 1;
    }
    Ns_CacheUnlock(cache);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheGetObjCmd --
 *
 *      Returns entry value if entry exists in the cache and not expired yet
 *
 * Results:
 *      TCL result with entry value or empty result
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheGetObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Cache *cache;
    Ns_Entry *entry;
    char     *key;

    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cache, arg},
        {"key",      Ns_ObjvString, &key,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    Ns_CacheLock(cache);
    if ((entry = Ns_CacheFindEntry(cache, key)) != NULL) {
        Tcl_SetStringObj(Tcl_GetObjResult(interp),
                         Ns_CacheGetValue(entry), Ns_CacheGetSize(entry));
    }
    Ns_CacheUnlock(cache);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheSetObjCmd --
 *
 *      Set new value of the cache entry
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
NsTclCacheSetObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    Ns_Cache *cache;
    Ns_Entry *entry;
    char     *key, *value = 0;
    int       new, timeout = -1, ttl = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvInt,   &timeout, NULL},
        {"-ttl",     Ns_ObjvInt,   &ttl,     NULL},
        {"--",       Ns_ObjvBreak, NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cache, arg},
        {"key",      Ns_ObjvString, &key,   NULL},
        {"value",    Ns_ObjvString, &value, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if ((entry = CreateEntry(itPtr, cache, key, &new, timeout)) == NULL) {
        return TCL_ERROR;
    }
    Tcl_SetStringObj(Tcl_GetObjResult(interp), value, -1);
    SetEntry(interp, entry, NULL, ttl);
    Ns_CacheUnlock(cache);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheAppendObjCmd, NsTclCacheLappendObjCmd --
 *
 *      Append one or more elements to cache value.
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
    Ns_Cache *cache;
    Ns_Entry *entry;
    Tcl_Obj  *resultObj;
    char     *key;
    int       i, new, nelements;
    int       timeout = -1, ttl = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout", Ns_ObjvInt,   &timeout, NULL},
        {"-ttl",     Ns_ObjvInt,   &ttl,     NULL},
        {"--",       Ns_ObjvBreak, NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache", ObjvCache,     &cache,     arg},
        {"key",   Ns_ObjvString, &key,       NULL},
        {"args",  Ns_ObjvArgs,   &nelements, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if ((entry = CreateEntry(itPtr, cache, key, &new, timeout)) == NULL) {
        return TCL_ERROR;
    }
    resultObj = Tcl_GetObjResult(interp);
    if (!new) {
        Tcl_SetStringObj(resultObj,
            Ns_CacheGetValue(entry), Ns_CacheGetSize(entry));
    }
    for (i = objc - nelements; i < objc; i++) {
        if (append) {
            Tcl_AppendObjToObj(resultObj, objv[i]);
        } else if (Tcl_ListObjAppendElement(interp, resultObj, objv[i])
                   != TCL_OK) {
            Ns_CacheUnlock(cache);
            return TCL_ERROR;
        }
    }
    SetEntry(interp, entry, NULL, ttl);
    Ns_CacheUnlock(cache);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheNamesObjCmd --
 *
 *      Spit back a list of cache names.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      A list of cache names will be appended to the interp result.
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
 *      Get a list of all keys in a cache, or only those matching
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

int
NsTclCacheKeysObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Cache       *cache;
    Ns_Entry       *entry;
    Ns_CacheSearch  search;
    char           *key, *pattern = NULL;
    Ns_DString      ds;
    
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,     &cache,   arg},
        {"?pattern", Ns_ObjvString, &pattern, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_DStringInit(&ds);
    Ns_CacheLock(cache);
    entry = Ns_CacheFirstEntry(cache, &search);
    while (entry != NULL) {
        key = Ns_CacheKey(entry);
        if (pattern == NULL || Tcl_StringMatch(key, pattern)) {
            Tcl_AppendElement(interp, key);
        }
        entry = Ns_CacheNextEntry(&search);
    }
    Ns_CacheUnlock(cache);
    Ns_DStringFree(&ds);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheFlushObjCmd --
 *
 *      Flush all entries from a cache, or the single entry identified
 *      by key.  Return the number of entries flushed.
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
    Ns_Cache       *cache;
    Ns_Entry       *entry;
    Ns_CacheSearch  search;
    char           *key, *pattern;
    int             i, nflushed = 0, glob = NS_FALSE, npatterns = 0;
    
    Ns_ObjvSpec opts[] = {
        {"-glob",    Ns_ObjvBool,  &glob, (void *) NS_TRUE},
        {"--",       Ns_ObjvBreak, NULL,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"cache",    ObjvCache,    &cache,     arg},
        {"?args",    Ns_ObjvArgs,  &npatterns, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_CacheLock(cache);
    if (npatterns == 0) {
        nflushed = Ns_CacheFlush(cache);
    } else if (!glob) {
        for (i = npatterns; i > 0; i--) {
            entry = Ns_CacheFindEntry(cache, Tcl_GetString(objv[objc-i]));
            if (entry != NULL) {
                Ns_CacheFlushEntry(entry);
                nflushed++;
            }
        }
    } else {
        entry = Ns_CacheFirstEntry(cache, &search);
        while (entry != NULL) {
            key = Ns_CacheKey(entry);
            for (i = npatterns; i > 0; i--) {
                pattern = Tcl_GetString(objv[objc-i]);
                if (Tcl_StringMatch(key, pattern)) {
                    Ns_CacheFlushEntry(entry);
                    nflushed++;
                    break;
                }
            }
            entry = Ns_CacheNextEntry(&search);
        }
    }
    Ns_CacheBroadcast(cache);
    Ns_CacheUnlock(cache);
    Tcl_SetIntObj(Tcl_GetObjResult(interp), nflushed);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCacheStatsObjCmds --
 *
 *      Returns stats on a cache.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Results will be appended to interp.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCacheStatsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Cache   *cache;
    Ns_DString  ds;

    Ns_ObjvSpec args[] = {
        {"cache", ObjvCache, &cache, arg},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    Ns_DStringInit(&ds);
    Ns_CacheLock(cache);
    Ns_CacheStats(cache, &ds);
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
 *      Pointer to entry, or NULL on flush or timeout.
 *
 * Side effects:
 *      Cache will be left locked if function returns entry.
 *
 *----------------------------------------------------------------------
 */

static Ns_Entry *
CreateEntry(NsInterp *itPtr, Ns_Cache *cache, char *key, int *newPtr,
            int timeout)
{
    Ns_Entry *entry;

    if (timeout < 0) {
        timeout = itPtr->servPtr->tcl.cacheTimeout;
    }
    Ns_CacheLock(cache);
    entry = Ns_CacheWaitCreateEntry(cache, key, newPtr, timeout);
    if (entry == NULL) {
        Ns_CacheUnlock(cache);
        Tcl_AppendResult(itPtr->interp,
            "timeout waiting for update or entry flushed: ", key, NULL);
    }
    return entry;
}


/*
 *----------------------------------------------------------------------
 *
 * SetEntry --
 *
 *      Set the value of the cache entry using valObj.  If interp
 *      is not NULL, also set it as the Tcl result.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      valObj will be owned by interp if not NULL.
 *
 *----------------------------------------------------------------------
 */

static void
SetEntry(Tcl_Interp *interp, Ns_Entry *entry, Tcl_Obj *valObj, int ttl)
{
    char    *string, *value;
    int      len;
    Tcl_Obj *objPtr = valObj;

    if (objPtr == NULL) {
        objPtr = Tcl_GetObjResult(interp);
    }
    string = Tcl_GetStringFromObj(objPtr, &len);
    value = ns_malloc(len+1);
    memcpy(value, string, len);
    value[len] = 0;
    Ns_CacheSetValueExpires(entry, value, len, ttl);
    if (valObj != NULL) {
        Tcl_SetObjResult(interp, valObj);
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
    Ns_Cache          **cachePtrPtr = spec->dest;
    NsInterp           *itPtr = spec->arg;
    NsServer           *servPtr = itPtr->servPtr;
    Tcl_HashEntry      *hPtr;
    static CONST char  *cacheType = "ns:cache";

    if (*objcPtr < 1) {
        return TCL_ERROR;
    }
    if (Ns_TclGetOpaqueFromObj(objv[0], cacheType, (void **) cachePtrPtr) != TCL_OK) {
        Ns_MutexLock(&servPtr->tcl.cachelock);
        hPtr = Tcl_FindHashEntry(&servPtr->tcl.caches, Tcl_GetString(objv[0]));
        Ns_MutexUnlock(&servPtr->tcl.cachelock);
        if (hPtr == NULL) {
            Tcl_AppendResult(interp, "no such cache: ", Tcl_GetString(objv[0]), NULL);
            return TCL_ERROR;
        }
        *cachePtrPtr = Tcl_GetHashValue(hPtr);
        Ns_TclSetOpaqueObj(objv[0], cacheType, *cachePtrPtr);
    }
    *objcPtr -= 1;

    return TCL_OK;
}
