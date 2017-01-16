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
 * dbinit.c --
 *
 *	This file contains routines for creating and accessing
 *	pools of database handles.
 */

#include "db.h"

Ns_LogSeverity Ns_LogSqlDebug;

/*
 * The following structure defines a database pool.
 */

struct Handle;

typedef struct Pool {
    const char     *name;
    const char     *desc;
    const char     *source;
    const char     *user;
    const char     *pass;
    Ns_Mutex	    lock;
    Ns_Cond	    waitCond;
    Ns_Cond	    getCond;
    const char	   *driver;
    struct DbDriver  *driverPtr;
    int		    waiting;
    int             nhandles;
    struct Handle  *firstPtr;
    struct Handle  *lastPtr;
    bool            fVerboseError;
    time_t          maxidle;
    time_t          maxopen;
    int             stale_on_close;
    Tcl_WideInt     statementCount;
    Tcl_WideInt     getHandleCount;
    Ns_Time         waitTime;
    Ns_Time         sqlTime;
    Ns_Time         minDuration;
}  Pool;

/*
 * The following structure defines the internal
 * state of a database handle.
 */

typedef struct Handle {
    const char     *driver;
    const char     *datasource;
    const char     *user;
    const char     *password;
    void           *connection;
    const char     *poolname;
    bool            connected;
    bool            verbose;   /* kept just for backwards compatibility, should be replaced by Ns_LogSqlDebug */
    Ns_Set         *row;
    char            cExceptionCode[6];
    Ns_DString      dsExceptionMsg;
    void           *context;
    void           *statement;
    int             fetchingRows;
    /* Members above must match Ns_DbHandle */
    struct Handle  *nextPtr;
    struct Pool	   *poolPtr;
    time_t          otime;
    time_t          atime;
    bool            stale;
    int             stale_on_close;
    bool            used;
} Handle;

/*
 * The following structure maintains per-server data.
 */

typedef struct ServData {
    const char *defpool;
    const char *allowed;
} ServData;

/*
 * Local functions defined in this file
 */

static Pool     *GetPool(const char *pool)                    NS_GNUC_NONNULL(1);
static void      ReturnHandle(Handle *handlePtr)              NS_GNUC_NONNULL(1);
static bool      IsStale(const Handle *handlePtr, time_t now) NS_GNUC_NONNULL(1);
static Ns_ReturnCode Connect(Handle *handlePtr)               NS_GNUC_NONNULL(1);
static Pool     *CreatePool(const char *pool, const char *path, const char *driver)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int	 IncrCount(const Pool *poolPtr, int incr)     NS_GNUC_NONNULL(1);
static ServData *GetServer(const char *server)                NS_GNUC_NONNULL(1);

/*
 * Static variables defined in this file
 */

static Ns_TlsCleanup FreeTable;
static Ns_Callback CheckPool;
static Ns_ArgProc CheckArgProc;

static Tcl_HashTable poolsTable;
static Tcl_HashTable serversTable;
static Ns_Tls tls;


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolDescription --
 *
 *	Return the pool's description string.
 *
 * Results:
 *	Configured description string or NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_DbPoolDescription(const char *pool)
{
    const Pool *poolPtr;
    const char *result;

    NS_NONNULL_ASSERT(pool != NULL);

    poolPtr = GetPool(pool);
    if (poolPtr == NULL) {
        result = NULL;
    } else {
        result = poolPtr->desc;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolDefault --
 *
 *	Return the default pool.
 *
 * Results:
 *	String name of default pool or NULL if no default is defined.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_DbPoolDefault(const char *server)
{
    const ServData *sdataPtr;

    NS_NONNULL_ASSERT(server != NULL);

    sdataPtr = GetServer(server);
    return ((sdataPtr != NULL) ? sdataPtr->defpool : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolList --
 *
 *	Return the list of all pools.
 *
 * Results:
 *	Double-null terminated list of pool names.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_DbPoolList(const char *server)
{
    const ServData *sdataPtr;

    NS_NONNULL_ASSERT(server != NULL);
    
    sdataPtr = GetServer(server);
    return ((sdataPtr != NULL) ? sdataPtr->allowed : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolAllowable --
 *
 *	Check that access is allowed to a pool.
 *
 * Results:
 *	NS_TRUE if allowed, NS_FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_DbPoolAllowable(const char *server, const char *pool)
{
    register const char *p;
    bool           result = NS_FALSE;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(pool != NULL);

    p = Ns_DbPoolList(server);
    if (p != NULL) {
        while (*p != '\0') {
            if (STREQ(pool, p)) {
                result = NS_TRUE;
                break;
            }
            p = p + strlen(p) + 1;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolPutHandle --
 *
 *	Cleanup and then return a handle to its pool.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Handle is flushed, reset, and possibly closed as required.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DbPoolPutHandle(Ns_DbHandle *handle)
{
    Handle	*handlePtr;
    Pool	*poolPtr;
    time_t	 now;

    NS_NONNULL_ASSERT(handle != NULL);

    handlePtr = (Handle *) handle;
    poolPtr = handlePtr->poolPtr;

    /*
     * Cleanup the handle.
     */

    (void) Ns_DbFlush(handle);
    (void) Ns_DbResetHandle(handle);

    Ns_DStringFree(&handle->dsExceptionMsg);
    handle->cExceptionCode[0] = '\0';

    /*
     * Close the handle if it's stale, otherwise update
     * the last access time.
     */

    time(&now);
    if (IsStale(handlePtr, now) == NS_TRUE) {
        NsDbDisconnect(handle);
    } else {
        handlePtr->atime = now;
    }
    (void) IncrCount(poolPtr, -1);
    Ns_MutexLock(&poolPtr->lock);
    ReturnHandle(handlePtr);
    if (poolPtr->waiting != 0) {
	Ns_CondSignal(&poolPtr->getCond);
    }
    Ns_MutexUnlock(&poolPtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolTimedGetHandle --
 *
 *	Return a single handle from a pool within the given number of
 *	seconds.
 *
 * Results:
 *	Pointer to Ns_DbHandle or NULL on error or timeout.
 *
 * Side effects:
 *	Database may be opened if needed.
 *
 *----------------------------------------------------------------------
 */

Ns_DbHandle *
Ns_DbPoolTimedGetHandle(const char *pool, const Ns_Time *wait)
{
    Ns_DbHandle *handle;

    NS_NONNULL_ASSERT(pool != NULL);

    if (Ns_DbPoolTimedGetMultipleHandles(&handle, pool, 1, wait) != NS_OK) {
        handle = NULL;
    }
    return handle;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolGetHandle --
 *
 *	Return a single handle from a pool.
 *
 * Results:
 *	Pointer to Ns_DbHandle or NULL on error.
 *
 * Side effects:
 *	Database may be opened if needed.
 *
 *----------------------------------------------------------------------
 */

Ns_DbHandle *
Ns_DbPoolGetHandle(const char *pool)
{
    NS_NONNULL_ASSERT(pool != NULL);
    
    return Ns_DbPoolTimedGetHandle(pool, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolGetMultipleHandles --
 *
 *	Return 1 or more handles from a pool.
 *
 * Results:
 *	NS_OK if the handlers where allocated, NS_ERROR
 *	otherwise.
 *
 * Side effects:
 *	Given array of handles is updated with pointers to allocated
 *	handles.  Also, database may be opened if needed.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_DbPoolGetMultipleHandles(Ns_DbHandle **handles, const char *pool, int nwant)
{
    NS_NONNULL_ASSERT(handles != NULL);
    NS_NONNULL_ASSERT(pool != NULL);

    return Ns_DbPoolTimedGetMultipleHandles(handles, pool, nwant, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolTimedGetMultipleHandles --
 *
 *	Return 1 or more handles from a pool within the given number
 *	of seconds.
 *
 * Results:
 *	NS_OK if the handlers where allocated, NS_TIMEOUT if the
 *	thread could not wait long enough for the handles, NS_ERROR
 *	otherwise.
 *
 * Side effects:
 *	Given array of handles is updated with pointers to allocated
 *	handles.  Also, database may be opened if needed.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_DbPoolTimedGetMultipleHandles(Ns_DbHandle **handles, const char *pool, 
    				 int nwant, const Ns_Time *wait)
{
    Handle         *handlePtr;
    Handle        **handlesPtrPtr = (Handle **) handles;
    Pool           *poolPtr;
    Ns_Time         timeout, startTime, endTime, diffTime;
    const Ns_Time  *timePtr;
    int             i, ngot;
    Ns_ReturnCode   status;

    NS_NONNULL_ASSERT(pool != NULL);
    NS_NONNULL_ASSERT(handles != NULL);

    /*
     * Verify the pool, the number of available handles in the pool,
     * and that the calling thread does not already own handles from
     * this pool.
     */
     
    poolPtr = GetPool(pool);
    if (poolPtr == NULL) {
	Ns_Log(Error, "dbinit: no such pool '%s'", pool);
	return NS_ERROR;
    }
    if (poolPtr->nhandles < nwant) {
	Ns_Log(Error, "dbinit: "
	       "failed to get %d handles from a db pool of only %d handles: '%s'",
	       nwant, poolPtr->nhandles, pool);
	return NS_ERROR;
    }
    ngot = IncrCount(poolPtr, nwant);
    if (ngot > 0) {
	Ns_Log(Error, "dbinit: db handle limit exceeded: "
	       "thread already owns %d handle%s from pool '%s'",
	       ngot, ngot == 1 ? "" : "s", pool);
	(void) IncrCount(poolPtr, -nwant);
	return NS_ERROR;
    }
    
    /*
     * Wait until this thread can be the exclusive thread acquiring
     * handles and then wait until all requested handles are available,
     * watching for timeout in either of these waits.
     */
    Ns_GetTime(&startTime);     
    if (wait == NULL) {
	timePtr = NULL;
    } else {
    	Ns_GetTime(&timeout);
    	Ns_IncrTime(&timeout, wait->sec, wait->usec);
	timePtr = &timeout;
    }
    status = NS_OK;
    Ns_MutexLock(&poolPtr->lock);
    while (status == NS_OK && poolPtr->waiting != 0) {
	status = Ns_CondTimedWait(&poolPtr->waitCond, &poolPtr->lock, timePtr);
    }
    if (status == NS_OK) {
    	poolPtr->waiting = 1;
    	while (status == NS_OK && ngot < nwant) {
	    while (status == NS_OK && poolPtr->firstPtr == NULL) {
	    	status = Ns_CondTimedWait(&poolPtr->getCond, &poolPtr->lock,
					  timePtr);
	    }
	    if (poolPtr->firstPtr != NULL) {
		handlePtr = poolPtr->firstPtr;
		poolPtr->firstPtr = handlePtr->nextPtr;
		handlePtr->nextPtr = NULL;
		if (poolPtr->lastPtr == handlePtr) {
		    poolPtr->lastPtr = NULL;
		}
                handlePtr->used = NS_TRUE;
		handlesPtrPtr[ngot++] = handlePtr;
	    }
	}
	poolPtr->waiting = 0;
    	Ns_CondSignal(&poolPtr->waitCond);
    }
    Ns_MutexUnlock(&poolPtr->lock);

    /*
     * Handle special race condition where the final requested handle
     * arrived just as the condition wait was timing out.
     */

    if (status == NS_TIMEOUT && ngot == nwant) {
	status = NS_OK;
    }

    /*
     * If status is still ok, connect any handles not already connected,
     * otherwise return any allocated handles back to the pool, then
     * update the final number of handles owned by this thread.
     */

    for (i = 0; status == NS_OK && i < ngot; ++i) {
	handlePtr = handlesPtrPtr[i];
	if (!handlePtr->connected) {
	    status = Connect(handlePtr);
	}
    }
    if (status != NS_OK) {
	Ns_MutexLock(&poolPtr->lock);
	while (ngot > 0) {
	    ReturnHandle(handlesPtrPtr[--ngot]);
	}
	if (poolPtr->waiting != 0) {
	    Ns_CondSignal(&poolPtr->getCond);
	}
	Ns_MutexUnlock(&poolPtr->lock);
	(void) IncrCount(poolPtr, -nwant);
    }
    
    Ns_GetTime(&endTime);     
    (void)Ns_DiffTime(&endTime, &startTime, &diffTime);
    Ns_IncrTime(&poolPtr->waitTime, diffTime.sec, diffTime.usec);
    poolPtr->getHandleCount++;
    
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbBouncePool --
 *
 *	Close all handles in the pool.
 *
 * Results:
 *	NS_OK if pool was bounce, NS_ERROR otherwise.
 *
 * Side effects:
 *	Handles are all marked stale and then closed by CheckPool.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_DbBouncePool(const char *pool)
{
    Pool	 *poolPtr;
    Handle	 *handlePtr;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(pool != NULL);

    poolPtr = GetPool(pool);
    if (poolPtr == NULL) {
	status = NS_ERROR;
        
    } else {
        Ns_MutexLock(&poolPtr->lock);
        poolPtr->stale_on_close++;
        handlePtr = poolPtr->firstPtr;
        while (handlePtr != NULL) {
            if (handlePtr->connected) {
                handlePtr->stale = NS_TRUE;
            }
            handlePtr->stale_on_close = poolPtr->stale_on_close;
            handlePtr = handlePtr->nextPtr;
        }
        Ns_MutexUnlock(&poolPtr->lock);
        CheckPool(poolPtr);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsDbInitPools --
 *
 *	Initialize the database pools at startup.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Pools may be created as configured.
 *
 *----------------------------------------------------------------------
 */

void
NsDbInitPools(void)
{
    const Pool   *poolPtr;
    const Ns_Set *pools;
    const char   *path, *driver;
    int	          isNew;
    size_t        i;

    Ns_TlsAlloc(&tls, FreeTable);

    /*
     * Attempt to create each database pool.
     */

    Tcl_InitHashTable(&serversTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&poolsTable, TCL_STRING_KEYS);
    pools = Ns_ConfigGetSection("ns/db/pools");

    for (i = 0u; (pools != NULL) && (i < Ns_SetSize(pools)); ++i) {
	const char    *pool = Ns_SetKey(pools, i);
        Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&poolsTable, pool, &isNew);

	if (isNew == 0) {
	    Ns_Log(Error, "dbinit: duplicate pool: %s", pool);
	    continue;	
	}
	path = Ns_ConfigGetPath(NULL, NULL, "db", "pool", pool, (char *)0);
	driver = Ns_ConfigGetValue(path, "driver");
	poolPtr = CreatePool(pool, path, driver);
	if (poolPtr == NULL) {
	    Tcl_DeleteHashEntry(hPtr);
	} else {
	    Tcl_SetHashValue(hPtr, poolPtr);
	}
    }
    Ns_RegisterProcInfo(CheckPool, "nsdb:check", CheckArgProc);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbPoolStats --
 *
 *	return usage statistics from pools
 *
 * Results:
 *	Tcl result code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
int
Ns_DbPoolStats(Tcl_Interp *interp)
{
    const Ns_Set *pools;
    size_t        i;
    Tcl_Obj      *resultObj;
    int           result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);

    resultObj = Tcl_NewListObj(0, NULL);
    pools = Ns_ConfigGetSection("ns/db/pools");

    for (i = 0u; (pools != NULL) && (i < Ns_SetSize(pools)); ++i) {
        const char    *pool = Ns_SetKey(pools, i);
        Pool	      *poolPtr;
        
        poolPtr = GetPool(pool);
        if (poolPtr == NULL) {
            Ns_Log(Warning, "Ignore invalid pool: %s", pool);
        } else {
            const Handle  *handlePtr;
            Tcl_Obj       *valuesObj;
            int            unused = 0, len;
            char           buf[100];

            /*
             * Iterate over the handles of this pool, which are currently
             * unused. Some of the currently unused handles might have been never
             * used. by subtracting the never used handles from the total handles,
             * we determine the used handles.
             */
            Ns_MutexLock(&poolPtr->lock);
            for (handlePtr = poolPtr->firstPtr; handlePtr != NULL; handlePtr = handlePtr->nextPtr) {
                if (!handlePtr->used) {
                    unused ++;
                }
            }
            Ns_MutexUnlock(&poolPtr->lock);

            valuesObj = Tcl_NewListObj(0, NULL);
            result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj("statements", 10));
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewWideIntObj(poolPtr->statementCount));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj("gethandles", 10));
            }
            if (likely(result == TCL_OK)) {            
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewWideIntObj(poolPtr->getHandleCount));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj("handles", 7));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewIntObj(poolPtr->nhandles));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj("used", 4));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewIntObj(poolPtr->nhandles - unused));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj("waittime", 8));
            }
            /* 
             * We could use Ns_TclNewTimeObj here (2x), when the default representation 
             * of the obj would be floating point format
             *  Tcl_ListObjAppendElement(interp, valuesObj, Ns_TclNewTimeObj(&poolPtr->waitTime));
            */
            if (likely(result == TCL_OK)) {
                len = snprintf(buf, sizeof(buf), "%" PRId64 ".%06ld", (int64_t) poolPtr->waitTime.sec, poolPtr->waitTime.usec);
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj(buf, len));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj("sqltime", 7));
            }
            if (likely(result == TCL_OK)) {
                len = snprintf(buf, sizeof(buf), "%" PRId64 ".%06ld", (int64_t) poolPtr->sqlTime.sec, poolPtr->sqlTime.usec);
                result = Tcl_ListObjAppendElement(interp, valuesObj, Tcl_NewStringObj(buf, len));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(pool, -1));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, resultObj, valuesObj);
            }
            if (unlikely(result != TCL_OK)) {
                break;
            }
        }
    }
    if (likely(result == TCL_OK)) {
        Tcl_SetObjResult(interp, resultObj);
    }
    
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsDbInitServer --
 *
 *	Initialize a virtual server allowed and default options.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
NsDbInitServer(const char *server)
{
    ServData	   *sdataPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    const char     *path, *pool;
    Ns_DString	    ds;
    int		    isNew;

    path = Ns_ConfigGetPath(server, NULL, "db", (char *)0);

    /*
     * Verify the default pool exists, if any.
     */

    sdataPtr = ns_malloc(sizeof(ServData));
    hPtr = Tcl_CreateHashEntry(&serversTable, server, &isNew);
    Tcl_SetHashValue(hPtr, sdataPtr);
    sdataPtr->defpool = Ns_ConfigGetValue(path, "defaultpool");
    if (sdataPtr->defpool != NULL &&
	(Tcl_FindHashEntry(&poolsTable, sdataPtr->defpool) == NULL)) {
	Ns_Log(Error, "dbinit: no such default pool '%s'", sdataPtr->defpool);
	sdataPtr->defpool = NULL;
    }

    /*
     * Construct the allowed list and call the server-specific init.
     */

    sdataPtr->allowed = "";
    pool = Ns_ConfigGetValue(path, "pools");
    if (pool != NULL && poolsTable.numEntries > 0) {
        const Pool *poolPtr;
        char       *allowed;

	Ns_DStringInit(&ds);
    	if (STREQ(pool, "*")) {
	    hPtr = Tcl_FirstHashEntry(&poolsTable, &search);
	    while (hPtr != NULL) {
	    	poolPtr = Tcl_GetHashValue(hPtr);
	    	NsDbDriverInit(server, poolPtr->driverPtr);
	    	Ns_DStringAppendArg(&ds, poolPtr->name);
		hPtr = Tcl_NextHashEntry(&search);
	    }
	} else {
	    char *p, *toDelete, *pool2;
	    toDelete = p = pool2 = ns_strdup(pool);
	    while (p != NULL && *p != '\0') {
		p = strchr(pool2, INTCHAR(','));
		if (p != NULL) {
		    *p = '\0';
		}
		hPtr = Tcl_FindHashEntry(&poolsTable, pool2);
		if (hPtr != NULL) {
		    poolPtr = Tcl_GetHashValue(hPtr);
	    	    NsDbDriverInit(server, poolPtr->driverPtr);
	    	    Ns_DStringAppendArg(&ds, poolPtr->name);
		}
		if (p != NULL) {
		    *p++ = ',';
		}
		pool2 = p;
	    }
	    ns_free(toDelete);
	}
    	allowed = ns_malloc((size_t)ds.length + 1u);
    	memcpy(allowed, ds.string, (size_t)ds.length + 1u);
        sdataPtr->allowed = allowed;
    	Ns_DStringFree(&ds);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsDbDisconnect --
 *
 *	Disconnect a handle by closing the database if needed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
NsDbDisconnect(Ns_DbHandle *handle)
{
    Handle *handlePtr = (Handle *) handle;

    (void)NsDbClose(handle);
    
    handlePtr->connected = NS_FALSE;
    handlePtr->atime = handlePtr->otime = 0;
    handlePtr->stale = NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * NsDbLogSql --
 *
 *	Log a SQL statement depending on the verbose state of the
 *	handle.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
NsDbLogSql(const Ns_Time *startTime, const Ns_DbHandle *handle, const char *sql)
{
    Pool   *poolPtr;

    NS_NONNULL_ASSERT(startTime != NULL);
    NS_NONNULL_ASSERT(handle != NULL);
    NS_NONNULL_ASSERT(sql != NULL);

    poolPtr = ((const Handle *)handle)->poolPtr;
    poolPtr->statementCount++;

    if (handle->dsExceptionMsg.length > 0) {
        /*
         * An exception occured.
         */
        if (poolPtr->fVerboseError) {
	    
            Ns_Log(Error, "dbinit: error(%s,%s): '%s'",
		   handle->datasource, handle->dsExceptionMsg.string, sql);
        }
    } else {
        /*
         * No exception log entries, if sql debug is enabled and runtime
         * is above threshold.
         */
        Ns_Time endTime, diffTime;
        
        Ns_GetTime(&endTime);
        (void)Ns_DiffTime(&endTime, startTime, &diffTime);
        Ns_IncrTime(&poolPtr->sqlTime, diffTime.sec, diffTime.usec);

        if (Ns_LogSeverityEnabled(Ns_LogSqlDebug) == NS_TRUE) {
            long delta = Ns_DiffTime(&poolPtr->minDuration, &diffTime, NULL);

            if (delta < 1) {
                Ns_Log(Ns_LogSqlDebug, "pool %s duration %" PRIu64 ".%06ld secs: '%s'",
                       handle->poolname, (int64_t)diffTime.sec, diffTime.usec, sql);
            }
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsDbGetDriver --
 *
 *	Return a pointer to the driver structure for a handle.
 *
 * Results:
 *	Pointer to driver or NULL on error.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

struct DbDriver *
NsDbGetDriver(const Ns_DbHandle *handle)
{
    struct DbDriver *result;
    const Handle    *handlePtr = (const Handle *) handle;

    if (handlePtr != NULL && handlePtr->poolPtr != NULL) {
	result = handlePtr->poolPtr->driverPtr;
    } else {
        result = NULL;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GetPool --
 *
 *	Return the Pool structure for the given pool name.
 *
 * Results:
 *	Pointer to Pool structure or NULL if pool does not exist.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Pool *
GetPool(const char *pool)
{
    Pool *               result;
    const Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(pool != NULL);

    hPtr = Tcl_FindHashEntry(&poolsTable, pool);
    if (hPtr == NULL) {
	result = NULL;
    } else {
	result = (Pool *) Tcl_GetHashValue(hPtr);
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnHandle --
 *
 *	Return a handle to its pool.  Connected handles are pushed on
 *	the front of the list, disconnected handles are appened to the
 *	end.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Handle is returned to the pool.  Note:  The pool lock must be
 *	held by the caller and this function does not signal a thread
 *	waiting for handles.
 *
 *----------------------------------------------------------------------
 */

static void
ReturnHandle(Handle *handlePtr)
{
    Pool         *poolPtr;

    NS_NONNULL_ASSERT(handlePtr != NULL);

    poolPtr = handlePtr->poolPtr;
    if (poolPtr->firstPtr == NULL) {
	poolPtr->firstPtr = poolPtr->lastPtr = handlePtr;
    	handlePtr->nextPtr = NULL;
    } else if (handlePtr->connected) {
	handlePtr->nextPtr = poolPtr->firstPtr;
	poolPtr->firstPtr = handlePtr;
    } else {
	poolPtr->lastPtr->nextPtr = handlePtr;
	poolPtr->lastPtr = handlePtr;
    	handlePtr->nextPtr = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * IsStale --
 *
 *	Check to see if a handle is stale.
 *
 * Results:
 *	NS_TRUE if handle stale, NS_FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bool
IsStale(const Handle *handlePtr, time_t now)
{
    bool result = NS_FALSE;
    
    NS_NONNULL_ASSERT(handlePtr != NULL);

    if (handlePtr->connected) {
        time_t    minAccess, minOpen;

	minAccess = now - handlePtr->poolPtr->maxidle;
	minOpen   = now - handlePtr->poolPtr->maxopen;
	if ((handlePtr->poolPtr->maxidle > 0 && handlePtr->atime < minAccess) || 
	    (handlePtr->poolPtr->maxopen > 0 && (handlePtr->otime < minOpen)) ||
	    (handlePtr->stale) ||
	    (handlePtr->poolPtr->stale_on_close > handlePtr->stale_on_close)) {

            Ns_Log(Ns_LogSqlDebug, "dbinit: closing %s handle in pool '%s'",
                   handlePtr->atime < minAccess ? "idle" : "old",
                   handlePtr->poolname);

	    result = NS_TRUE;
	}
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CheckArgProc --
 *
 *	Ns_ArgProc callback for the pool checker.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Copies name of pool to given dstring.
 *
 *----------------------------------------------------------------------
 */

static void
CheckArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const Pool *poolPtr = arg;

    Tcl_DStringAppendElement(dsPtr, poolPtr->name);
}


/*
 *----------------------------------------------------------------------
 *
 * CheckPool --
 *
 *	Verify all handles in a pool are not stale.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Stale handles, if any, are closed.
 *
 *----------------------------------------------------------------------
 */

static void
CheckPool(void *arg)
{
    Pool 	 *poolPtr = arg;
    Handle       *handlePtr, *nextPtr;
    Handle       *checkedPtr;
    time_t	  now;

    time(&now);
    checkedPtr = NULL;

    /*
     * Grab the entire list of handles from the pool.
     */

    Ns_MutexLock(&poolPtr->lock);
    handlePtr = poolPtr->firstPtr;
    poolPtr->firstPtr = poolPtr->lastPtr = NULL;
    Ns_MutexUnlock(&poolPtr->lock);

    /*
     * Run through the list of handles, closing any
     * which have gone stale, and then return them
     * all to the pool.
     */

    if (handlePtr != NULL) {
    	while (handlePtr != NULL) {
	    nextPtr = handlePtr->nextPtr;
	    if (IsStale(handlePtr, now) == NS_TRUE) {
                NsDbDisconnect((Ns_DbHandle *) handlePtr);
	    }
	    handlePtr->nextPtr = checkedPtr;
	    checkedPtr = handlePtr;
	    handlePtr = nextPtr;
    	}

	Ns_MutexLock(&poolPtr->lock);
	handlePtr = checkedPtr;
	while (handlePtr != NULL) {
	    nextPtr = handlePtr->nextPtr;
	    ReturnHandle(handlePtr);
	    handlePtr = nextPtr;
	}
	if (poolPtr->waiting != 0) {
	    Ns_CondSignal(&poolPtr->getCond);
	}
	Ns_MutexUnlock(&poolPtr->lock);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * CreatePool --
 *
 *	Create a new pool using the given driver.
 *
 * Results:
 *	Pointer to newly allocated Pool structure.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Pool  *
CreatePool(const char *pool, const char *path, const char *driver)
{
    Pool            *poolPtr;
    struct DbDriver *driverPtr;

    NS_NONNULL_ASSERT(pool != NULL);
    NS_NONNULL_ASSERT(path != NULL);

    if (driver == NULL) {
	Ns_Log(Error, "dbinit: no driver for pool '%s'", pool);
        driverPtr = NULL;
    } else {
        driverPtr = NsDbLoadDriver(driver);
    }

    if (driverPtr == NULL) {
        poolPtr = NULL;
        
    } else {
        int          i;
        const char  *source, *minDurationString;

        /*
         * Load the configured values.
         */
        source = Ns_ConfigGetValue(path, "datasource");
        if (source == NULL) {
            Ns_Log(Error, "dbinit: missing datasource for pool '%s'", pool);
            return NULL;
        }
        /*
         * Allocate Pool structure and initialize its members
         */
        poolPtr = ns_calloc(1u, sizeof(Pool));
        poolPtr->driver = driver;
        poolPtr->driverPtr = driverPtr;
        Ns_MutexInit(&poolPtr->lock);
        Ns_MutexSetName2(&poolPtr->lock, "nsdb", pool);
        Ns_CondInit(&poolPtr->waitCond);
        Ns_CondInit(&poolPtr->getCond);
        poolPtr->source = source;
        poolPtr->name = pool;
        poolPtr->user = Ns_ConfigGetValue(path, "user");
        poolPtr->pass = Ns_ConfigGetValue(path, "password");
        poolPtr->desc = Ns_ConfigGetValue("ns/db/pools", pool);
        poolPtr->stale_on_close = 0;
        poolPtr->fVerboseError = Ns_ConfigBool(path, "logsqlerrors", NS_FALSE);
        poolPtr->nhandles = Ns_ConfigIntRange(path, "connections", 2, 0, INT_MAX);
        poolPtr->maxidle = Ns_ConfigIntRange(path, "maxidle", 600, 0, INT_MAX);
        poolPtr->maxopen = Ns_ConfigIntRange(path, "maxopen", 3600, 0, INT_MAX);
        minDurationString = Ns_ConfigGetValue(path, "logminduration");
        if (minDurationString != NULL) {
            if (Ns_GetTimeFromString(NULL, minDurationString, &poolPtr->minDuration) != TCL_OK) {
                Ns_Log(Error, "dbinit: invalid LogMinDuration '%s' specified", minDurationString);
            } else {
                Ns_Log(Notice, "dbinit: set LogMinDuration for pool %s over %s to %" PRIu64 ".%06ld",
                       pool, minDurationString,
                       (int64_t)poolPtr->minDuration.sec,
                       poolPtr->minDuration.usec);
            }
        }

        /*
         * Allocate the handles in the pool
         */
        poolPtr->firstPtr = poolPtr->lastPtr = NULL;
        for (i = 0; i < poolPtr->nhandles; ++i) {
            Handle *handlePtr = ns_malloc(sizeof(Handle));
            
            Ns_DStringInit(&handlePtr->dsExceptionMsg);
            handlePtr->poolPtr = poolPtr;
            handlePtr->connection = NULL;
            handlePtr->connected = NS_FALSE;
            handlePtr->fetchingRows = 0;
            handlePtr->row = Ns_SetCreate(NULL);
            handlePtr->cExceptionCode[0] = '\0';
            handlePtr->otime = handlePtr->atime = 0;
            handlePtr->stale = NS_FALSE;
            handlePtr->stale_on_close = 0;

            /*
             * The following elements of the Handle structure could be
             * obtained by dereferencing the poolPtr.  They're only needed
             * to maintain the original Ns_DbHandle structure definition
             * which was designed to allow handles outside of pools, a
             * feature no longer supported.
             */

            handlePtr->driver = driver;
            handlePtr->datasource = poolPtr->source;
            handlePtr->user = poolPtr->user;
            handlePtr->password = poolPtr->pass;
            handlePtr->verbose = NS_FALSE;
            handlePtr->poolname = pool;
            ReturnHandle(handlePtr);
        }
        (void) Ns_ScheduleProc(CheckPool, poolPtr, 0,
                               Ns_ConfigIntRange(path, "checkinterval", 600, 0, INT_MAX));
    }
    return poolPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Connect --
 *
 *	Connect a handle by opening the database.
 *
 * Results:
 *	NS_OK if connect ok, NS_ERROR otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
Connect(Handle *handlePtr)
{
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(handlePtr != NULL);

    status = NsDbOpen((Ns_DbHandle *) handlePtr);
    if (status != NS_OK) {
    	handlePtr->connected = NS_FALSE;
    	handlePtr->atime = handlePtr->otime = 0;
	handlePtr->stale = NS_FALSE;
    } else {
    	handlePtr->connected = NS_TRUE;
    	handlePtr->atime = handlePtr->otime = time(NULL);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * IncrCount --
 *
 *	Update per-thread count of allocated handles.
 *
 * Results:
 *	Previous count of allocated handles.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
IncrCount(const Pool *poolPtr, int incr)
{
    Tcl_HashTable *tablePtr;
    Tcl_HashEntry *hPtr;
    int prev, count, isNew;

    NS_NONNULL_ASSERT(poolPtr != NULL);

    tablePtr = Ns_TlsGet(&tls);
    if (tablePtr == NULL) {
	tablePtr = ns_malloc(sizeof(Tcl_HashTable));
	Tcl_InitHashTable(tablePtr, TCL_ONE_WORD_KEYS);
	Ns_TlsSet(&tls, tablePtr);
    }
    hPtr = Tcl_CreateHashEntry(tablePtr, (const char *) poolPtr, &isNew);
    if (isNew != 0) {
	prev = 0;
    } else {
	prev = PTR2INT(Tcl_GetHashValue(hPtr));
    }
    count = prev + incr;
    if (count == 0) {
	Tcl_DeleteHashEntry(hPtr);
    } else {
        Tcl_SetHashValue(hPtr, INT2PTR(count));
    }
    return prev;
}


/*
 *----------------------------------------------------------------------
 *
 * GetServer --
 *
 *	Get per-server data.
 *
 * Results:
 *	Pointer to per-server data.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static ServData *
GetServer(const char *server)
{
    ServData            *result = NULL;
    const Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(server != NULL);

    hPtr = Tcl_FindHashEntry(&serversTable, server);
    if (hPtr != NULL) {
	result = Tcl_GetHashValue(hPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeTable --
 *
 *	Free the per-thread count of allocated handles table.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
FreeTable(void *arg)
{
    Tcl_HashTable  *tablePtr = arg;

    Tcl_DeleteHashTable(tablePtr);
    ns_free(tablePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbListMinDurations --
 *
 *	Introspection function to list min duration for every available
 *	pool.
 *
 * Results:
 *	Tcl_ListObj containing pairs of pool names and minDurations.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
Ns_DbListMinDurations(Tcl_Interp *interp, const char *server)
{
    Tcl_Obj    *resultObj;
    const char *pool;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(server != NULL);

    resultObj = Tcl_NewListObj(0, NULL);
    pool = Ns_DbPoolList(server);
    if (pool != NULL) {
        for ( ; *pool != '\0'; pool += strlen(pool) + 1u) {
            char          buffer[100];
            const Pool   *poolPtr;
            int           len;
            
            poolPtr = GetPool(pool);
            (void) Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(pool, -1));
            len = snprintf(buffer, sizeof(buffer), "%" PRId64 ".%06ld",
                           (int64_t) poolPtr->minDuration.sec, poolPtr->minDuration.usec);
            (void) Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(buffer, len));
        }
    }
    return resultObj;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbGetMinDuration --
 *
 *	Return the minDuration of the specified pool in the third
 *	argument.
 *
 * Results:
 *	Tcl result code
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int 
Ns_DbGetMinDuration(Tcl_Interp *interp, const char *pool, Ns_Time **minDuration)
{
    Pool *poolPtr;
    int   result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(pool != NULL);
    NS_NONNULL_ASSERT(minDuration != NULL);

    /*
     * Get the poolPtr
     */
    poolPtr = GetPool(pool);
    if (poolPtr == NULL) {
        Ns_TclPrintfResult(interp, "Invalid pool '%s'", pool);
        result = TCL_ERROR;
    } else {
        /*
         * Return the duration.
         */
        *minDuration = &poolPtr->minDuration;
        result = TCL_OK;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbSetMinDuration --
 *
 *	Set the minDuration of the specified pool
 *
 * Results:
 *	Tcl result code
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int 
Ns_DbSetMinDuration(Tcl_Interp *interp, const char *pool, const Ns_Time *minDuration)
{
    Pool *poolPtr;
    int   result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(pool != NULL);
    NS_NONNULL_ASSERT(minDuration != NULL);

    /*
     * Get the poolPtr
     */
    poolPtr = GetPool(pool);
    if (poolPtr == NULL) {
        Ns_TclPrintfResult(interp, "Invalid pool '%s'", pool);
        result = TCL_ERROR;
    } else {
        /*
         * Set the duration.
         */
        poolPtr->minDuration = *minDuration;
        result = TCL_OK;
    }
    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 72
 * indent-tabs-mode: nil
 * End:
 */
