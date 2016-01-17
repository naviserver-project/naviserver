/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
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
 * queue.c --
 *
 *  Routines for the managing the virtual server connection queue
 *  and service threads.
 */

#include "nsd.h"

/*
 * Local functions defined in this file
 */

static void ConnRun(const ConnThreadArg *argPtr, Conn *connPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void CreateConnThread(ConnPool *poolPtr)
    NS_GNUC_NONNULL(1);
static void JoinConnThread(Ns_Thread *threadPtr)
    NS_GNUC_NONNULL(1);

static void AppendConn(Tcl_DString *dsPtr, const Conn *connPtr, const char *state)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static void AppendConnList(Tcl_DString *dsPtr, const Conn *firstPtr, const char *state)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static int neededAdditionalConnectionThreads(const ConnPool *poolPtr) 
    NS_GNUC_NONNULL(1);

static void WakeupConnThreads(ConnPool *poolPtr) 
    NS_GNUC_NONNULL(1);


/*
 * Static variables defined in this file.
 */

static Ns_Tls argtls = NULL;
static int    poolid = 0;

/*
 * Debugging stuff
 */
#define ThreadNr(poolPtr, argPtr) (((argPtr) - (poolPtr)->tqueue.args))

#if 0
static void ConnThreadQueuePrint(ConnPool *poolPtr, char *key) {
    ConnThreadArg *aPtr;
  
    fprintf(stderr, "%s: thread queue (idle %d): ", key, poolPtr->threads.idle);
    Ns_MutexLock(&poolPtr->tqueue.lock);
    for (aPtr = poolPtr->tqueue.nextPtr; aPtr; aPtr = aPtr->nextPtr) {
	fprintf(stderr, "[%d] state %d, ", ThreadNr(poolPtr, aPtr), aPtr->state);
    }
    Ns_MutexUnlock(&poolPtr->tqueue.lock);
    fprintf(stderr, "\n");
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * NsInitQueue --
 *
 *      Init connection queue.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsInitQueue(void)
{
    Ns_TlsAlloc(&argtls, NULL);
    poolid = Ns_UrlSpecificAlloc();
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetConn --
 *
 *      Return the current connection in this thread.
 *
 * Results:
 *      Pointer to conn or NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Conn *
Ns_GetConn(void)
{
    ConnThreadArg *argPtr;

    argPtr = Ns_TlsGet(&argtls);
    return ((argPtr != NULL) ? ((Ns_Conn *) argPtr->connPtr) : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsMapPool --
 *
 *      Map a method/URL to the given pool.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Requests for given URL's will be serviced by given pool.
 *
 *----------------------------------------------------------------------
 */

void
NsMapPool(ConnPool *poolPtr, const char *map)
{
    const char **mv;
    const char *server;
    int  mc;

    NS_NONNULL_ASSERT(poolPtr != NULL);
    NS_NONNULL_ASSERT(map != NULL);

    server = poolPtr->servPtr->server;

    if (Tcl_SplitList(NULL, map, &mc, &mv) == TCL_OK) {
        if (mc == 2) {
            Ns_UrlSpecificSet(server, mv[0], mv[1], poolid, poolPtr, 0u, NULL);
            Ns_Log(Notice, "pool[%s]: mapped %s %s -> %s", 
		   server, mv[0], mv[1], poolPtr->pool);
        }
        Tcl_Free((char *) mv);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * neededAdditionalConnectionThreads --
 *
 *      Compute the number additional connection threads we should
 *      create. This function has to be called under a lock for the
 *      provided queue (such as &poolPtr->wqueue.lock).
 *
 * Results:
 *      Number of needed additional connection threads.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static int 
neededAdditionalConnectionThreads(const ConnPool *poolPtr) {
    int wantCreate;

    NS_NONNULL_ASSERT(poolPtr != NULL);

    /* 
     * Create new connection threads, if
     * 
     * - there is currently no connection thread being created, or
     *   parallel creates are allowed and there are more than
     *   highwatermark requests queued,
     *
     * - AND there are less idle-threads than min threads (the server
     *   tries to keep min-threads idle to be ready for short peaks),
     *
     * - AND there are not yet max-threads running.
     *
     */
    if ( (poolPtr->threads.creating == 0 
	  || poolPtr->wqueue.wait.num > poolPtr->wqueue.highwatermark
	  )
	 && (poolPtr->threads.current < poolPtr->threads.min
	     || (poolPtr->wqueue.wait.num > poolPtr->wqueue.lowwatermark)
	     )
	 && poolPtr->threads.current < poolPtr->threads.max
	 ) {

	Ns_MutexLock(&poolPtr->servPtr->pools.lock);
	wantCreate = (poolPtr->servPtr->pools.shutdown == NS_FALSE);
	Ns_MutexUnlock(&poolPtr->servPtr->pools.lock);
      
	/*Ns_Log(Notice, "[%s] wantCreate %d (creating %d current %d idle %d waiting %d)",
	     poolPtr->servPtr->server, 
	     wantCreate, 
	     poolPtr->threads.creating,
	     poolPtr->threads.current, 
	     poolPtr->threads.idle,
	     poolPtr->wqueue.wait.num
	     );*/
    } else {
        wantCreate = 0;
		
        /*Ns_Log(Notice, "[%s] do not wantCreate creating %d, idle %d < min %d, current %d < max %d, waiting %d)",
	       poolPtr->servPtr->server, 
	       poolPtr->threads.creating, 
	       poolPtr->threads.idle,
	       poolPtr->threads.min,
	       poolPtr->threads.current, 
	       poolPtr->threads.max,
	       poolPtr->wqueue.wait.num);*/
	
    }

    return wantCreate;
}


/*
 *----------------------------------------------------------------------
 *
 * NsEnsureRunningConnectionThreads --
 *
 *      Ensure that there are the right number if connection threads
 *      running. The function computes for the provided pool or for
 *      the default pool of the server the number of missing threads
 *      and creates a single connection thread when needed. This
 *      function is typically called from the driver.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potentially, a created connection thread.
 *
 *----------------------------------------------------------------------
 */

void
NsEnsureRunningConnectionThreads(const NsServer *servPtr, ConnPool *poolPtr) {
    int create;

    NS_NONNULL_ASSERT(servPtr != NULL);

    if (poolPtr == NULL) {
        /* 
	 * Use the default pool for the time being, if no pool was
	 * provided
	 */
        poolPtr = servPtr->pools.defaultPtr;
    }

    Ns_MutexLock(&poolPtr->wqueue.lock);
    Ns_MutexLock(&poolPtr->threads.lock);
    create = neededAdditionalConnectionThreads(poolPtr);

    if (create != 0) {
	poolPtr->threads.current ++;
	poolPtr->threads.creating ++;
    }

    Ns_MutexUnlock(&poolPtr->threads.lock);
    Ns_MutexUnlock(&poolPtr->wqueue.lock);

    if (create != 0) {
        Ns_Log(Notice, "NsEnsureRunningConnectionThreads wantCreate %d waiting %d idle %d current %d", 
	       create,
	       poolPtr->wqueue.wait.num,
	       poolPtr->threads.idle, 
	       poolPtr->threads.current);
        CreateConnThread(poolPtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsQueueConn --
 *
 *      Append a connection to the run queue.
 *
 * Results:
 *      1 if queued, 0 otherwise.
 *
 * Side effects:
 *      Conneciton will run shortly.
 *
 *----------------------------------------------------------------------
 */

int
NsQueueConn(Sock *sockPtr, const Ns_Time *nowPtr)
{
    ConnThreadArg *argPtr = NULL;
    NsServer *servPtr;
    ConnPool *poolPtr = NULL;
    Conn     *connPtr = NULL;
    int       create = 0;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(nowPtr != NULL);

    servPtr = sockPtr->servPtr;

    /*
     * Select server connection pool.
     */

    if (sockPtr->reqPtr != NULL) {
        poolPtr = NsUrlSpecificGet(servPtr,
                                   sockPtr->reqPtr->request.method,
                                   sockPtr->reqPtr->request.url,
                                   poolid, 0u, NS_URLSPACE_DEFAULT);
    }
    if (poolPtr == NULL) {
        poolPtr = servPtr->pools.defaultPtr;
    }

   /*
    * Queue connection if possible (e.g. no shutdown, a free Conn is
    * available, ...)
    */
  
    if (servPtr->pools.shutdown == NS_FALSE) {

	Ns_MutexLock(&poolPtr->wqueue.lock);
	if (poolPtr->wqueue.freePtr != NULL) {
	    connPtr = poolPtr->wqueue.freePtr;
	    poolPtr->wqueue.freePtr = connPtr->nextPtr;
            connPtr->nextPtr = NULL;
	}
	Ns_MutexUnlock(&poolPtr->wqueue.lock);

        if (connPtr != NULL) {
	    /*
	     * We have got a free connPtr from the pool. Initalize the
	     * connPtr and copy flags from the socket.
	     */
	  
	    /* ConnThreadQueuePrint(poolPtr, "driver");*/

	    Ns_MutexLock(&servPtr->pools.lock);
            connPtr->id = servPtr->pools.nextconnid++;
   	    poolPtr->stats.processed++;
	    Ns_MutexUnlock(&servPtr->pools.lock);

            connPtr->requestQueueTime     = *nowPtr;
            connPtr->sockPtr              = sockPtr;
            connPtr->drvPtr               = sockPtr->drvPtr;
            connPtr->poolPtr              = poolPtr;
            connPtr->server               = servPtr->server;
            connPtr->location             = sockPtr->location;
	    connPtr->flags                = sockPtr->flags;
	    if ((sockPtr->drvPtr->opts & NS_DRIVER_ASYNC) == 0u) {
		connPtr->acceptTime       = *nowPtr;
	    } else {
		connPtr->acceptTime       = sockPtr->acceptTime;
	    }
	    sockPtr->acceptTime.sec       = 0; /* invalidate time */

	    /*
	     * Try to get an entry from the connection thread queue,
	     * and dequeue it when possible.
	     */
	    if (poolPtr->tqueue.nextPtr != NULL) {
	        Ns_MutexLock(&poolPtr->tqueue.lock);
		if (poolPtr->tqueue.nextPtr != NULL) {
		  argPtr = poolPtr->tqueue.nextPtr;
		  poolPtr->tqueue.nextPtr = argPtr->nextPtr;
		}
	        Ns_MutexUnlock(&poolPtr->tqueue.lock);
	    }

	    if (argPtr != NULL) {
		/* 
		 * We could obtain an idle thread. Dequeue the entry,
		 * such that noone else might grab it, and fill in the
		 * connPtr that should be run by this thread.
		 */
	        
		assert(argPtr->state == connThread_idle);
		argPtr->connPtr = connPtr;

		Ns_MutexLock(&poolPtr->wqueue.lock);
		Ns_MutexLock(&poolPtr->threads.lock);
		create = neededAdditionalConnectionThreads(poolPtr);
		Ns_MutexUnlock(&poolPtr->threads.lock);
		Ns_MutexUnlock(&poolPtr->wqueue.lock);

	    } else {
	      /* 
	       * There is no connection thread ready, so we add the
	       * connection to the waiting queue.
	       */
		Ns_MutexLock(&poolPtr->wqueue.lock);
	        if (poolPtr->wqueue.wait.firstPtr == NULL) {
		    poolPtr->wqueue.wait.firstPtr = connPtr;
		} else {
		    poolPtr->wqueue.wait.lastPtr->nextPtr = connPtr;
		}
		poolPtr->wqueue.wait.lastPtr = connPtr;
		poolPtr->wqueue.wait.num ++;
		Ns_MutexLock(&poolPtr->threads.lock);
		poolPtr->stats.queued++;
		create = neededAdditionalConnectionThreads(poolPtr);
		Ns_MutexUnlock(&poolPtr->threads.lock);
		Ns_MutexUnlock(&poolPtr->wqueue.lock);
	    }
        }
    }

    if (connPtr == NULL) {
	Ns_Log(Notice, "[%s] All avaliable connections are used, waiting %d idle %d current %d",
	       poolPtr->servPtr->server, 
	       poolPtr->wqueue.wait.num,
	       poolPtr->threads.idle, 
	       poolPtr->threads.current);
	return 0;
    }

    if (argPtr != NULL) {
	/*
	 * We have a connection thread ready.
	 *
	 * Perform lock just at the "right" debug level.
	 */
	if (Ns_LogSeverityEnabled(Debug)) {
	    int idle; 

	    Ns_MutexLock(&poolPtr->threads.lock);
	    idle = poolPtr->threads.idle;
	    Ns_MutexUnlock(&poolPtr->threads.lock);

	    Ns_Log(Debug, "[%ld] dequeue thread connPtr %p idle %d state %d create %d", 
		   ThreadNr(poolPtr, argPtr), (void *)connPtr, idle, argPtr->state, create);
	}

	/*
	 * Signal the associated thread to start with this request.
	 */
	Ns_MutexLock(&argPtr->lock);
        Ns_CondSignal(&argPtr->cond);
	Ns_MutexUnlock(&argPtr->lock);

    } else {
	if (Ns_LogSeverityEnabled(Debug)) {
	    Ns_Log(Debug, "add waiting connPtr %p => waiting %d create %d", 
		   (void *)connPtr, poolPtr->wqueue.wait.num, create);
	}
    }

    if (create != 0) {
	int idle, current;

	Ns_MutexLock(&poolPtr->threads.lock);
	idle = poolPtr->threads.idle;
	current = poolPtr->threads.current;
	poolPtr->threads.current ++;
	poolPtr->threads.creating ++;
        Ns_MutexUnlock(&poolPtr->threads.lock);

        Ns_Log(Notice, "NsQueueConn wantCreate %d waiting %d idle %d current %d", 
	       create,
	       poolPtr->wqueue.wait.num,
	       idle, 
	       current);

	CreateConnThread(poolPtr);
    } 

    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclServerObjCmd --
 *
 *      Implement the ns_server Tcl command to return simple statistics
 *      about the running server.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclServerObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          subcmd = 0, value = 0;
    NsInterp    *itPtr = arg;
    NsServer    *servPtr = NULL;
    ConnPool    *poolPtr;
    char        *pool = NULL, *optArg = NULL, buf[100];
    Tcl_DString ds, *dsPtr = &ds;

    enum {
        SActiveIdx, SAllIdx, SConnectionsIdx, 
        SFiltersIdx,
        SKeepaliveIdx, 
        SMaxthreadsIdx, SMinthreadsIdx,
        SPagedirIdx, SPoolsIdx, SQueuedIdx, 
        SRequestprocsIdx,
        SServerdirIdx, SStatsIdx, 
        STcllibIdx, SThreadsIdx, STracesIdx,
        SUrl2fileIdx, SWaitingIdx
    };
    
    static Ns_ObjvTable subcmds[] = {
        {"active",       (unsigned int)SActiveIdx},
        {"all",          (unsigned int)SAllIdx},
        {"connections",  (unsigned int)SConnectionsIdx},
        {"filters",      (unsigned int)SFiltersIdx},
        {"keepalive",    (unsigned int)SKeepaliveIdx},
        {"maxthreads",   (unsigned int)SMaxthreadsIdx},
        {"minthreads",   (unsigned int)SMinthreadsIdx},
        {"pagedir",      (unsigned int)SPagedirIdx},
        {"pools",        (unsigned int)SPoolsIdx},
        {"queued",       (unsigned int)SQueuedIdx},
        {"requestprocs", (unsigned int)SRequestprocsIdx},
        {"serverdir",    (unsigned int)SServerdirIdx},
        {"stats",        (unsigned int)SStatsIdx},
        {"tcllib",       (unsigned int)STcllibIdx},
        {"threads",      (unsigned int)SThreadsIdx},
        {"traces",       (unsigned int)STracesIdx},
        {"url2file",     (unsigned int)SUrl2fileIdx},
        {"waiting",      (unsigned int)SWaitingIdx},
        {NULL,           0u}
    };
    
    Ns_ObjvSpec opts[] = {
        {"-server", Ns_ObjvServer,  &servPtr, NULL},
        {"-pool",   Ns_ObjvString,  &pool,    NULL},
        {"--",      Ns_ObjvBreak,   NULL,     NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"subcmd",  Ns_ObjvIndex,  &subcmd,   subcmds},
        {"?arg",    Ns_ObjvString, &optArg,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK)) {
        return TCL_ERROR;
    }
    
    if ((subcmd == SPoolsIdx 
	 || subcmd == SFiltersIdx
	 || subcmd == SPagedirIdx
	 || subcmd == SRequestprocsIdx
	 || subcmd == SUrl2fileIdx)
	&& pool != NULL) {	
	    Tcl_AppendResult(interp, 
			     "option -pool is not allowed for this subcommand", NULL);
	    return TCL_ERROR;
    }

    if (subcmd != SMinthreadsIdx && subcmd != SMaxthreadsIdx) {	
	/*
	 * just for backwards compatibility
	 */
	if (optArg != NULL) {
	    Ns_LogDeprecated(objv, objc, "ns_server ?-pool p? ...", 
			     "Passing pool as second argument is deprected.");
	    pool = optArg;
	}
    }

    if (servPtr == NULL) {
	servPtr = itPtr->servPtr;
    }

    if (pool != NULL) {
        poolPtr = servPtr->pools.firstPtr;
        while (poolPtr != NULL && !STREQ(poolPtr->pool, pool)) {
            poolPtr = poolPtr->nextPtr;
        }
        if (unlikely(poolPtr == NULL)) {
            Tcl_AppendResult(interp, "no such pool ", pool, " for server ", servPtr->server, NULL);
            return TCL_ERROR;
        }
    } else {
	poolPtr = servPtr->pools.defaultPtr;
    }
    
    switch (subcmd) {
	/* 
	 * These subcommands are server specific (do not allow -pool option)
	 */
    case SPoolsIdx:
        poolPtr = servPtr->pools.firstPtr;
        while (poolPtr != NULL) {
            Tcl_AppendElement(interp, poolPtr->pool);
            poolPtr = poolPtr->nextPtr;
        }
        break;

    case SFiltersIdx:
	Tcl_DStringInit(dsPtr);
        NsGetFilters(dsPtr, servPtr->server);
        Tcl_DStringResult(interp, dsPtr);
        break;

    case SPagedirIdx:
	Tcl_DStringInit(dsPtr);
        NsPageRoot(dsPtr, servPtr, NULL);
        Tcl_DStringResult(interp, dsPtr);
        break;

    case SServerdirIdx:
	Tcl_DStringInit(dsPtr);
	Tcl_DStringAppend(dsPtr, servPtr->fastpath.serverdir, -1);
        Tcl_DStringResult(interp, dsPtr);
        break;

    case SRequestprocsIdx:
	Tcl_DStringInit(dsPtr);
        NsGetRequestProcs(dsPtr, servPtr->server);
        Tcl_DStringResult(interp, dsPtr);
        break;

    case STracesIdx:
	Tcl_DStringInit(dsPtr);
        NsGetTraces(dsPtr, servPtr->server);
        Tcl_DStringResult(interp, dsPtr);
        break;

    case STcllibIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(servPtr->tcl.library, -1));
        break;

    case SUrl2fileIdx:
	Tcl_DStringInit(dsPtr);
        NsGetUrl2FileProcs(dsPtr, servPtr->server);
        Tcl_DStringResult(interp, dsPtr);
        break;

	/* 
	 * These subcommands are pool-specific (allow -pool option)
	 */
	
    case SWaitingIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->wqueue.wait.num));
        break;

    case SKeepaliveIdx:
	Ns_LogDeprecated(objv, objc, "ns_conn keepalive", NULL);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        break;

    case SMaxthreadsIdx:
	if (optArg != NULL) {
	    if (Ns_StrToInt(optArg, &value) != NS_OK || value < poolPtr->threads.min || value > poolPtr->wqueue.maxconns) {
		Tcl_AppendResult(interp, "argument is not an integer in valid range: ", optArg, NULL);
		return TCL_ERROR;
	    }
	    Ns_MutexLock(&poolPtr->threads.lock);
	    poolPtr->threads.max = value;
	    Ns_MutexUnlock(&poolPtr->threads.lock);
	}
        Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->threads.max));
	break;

    case SMinthreadsIdx:
	if (optArg != NULL) {
	    if (Ns_StrToInt(optArg, &value) != NS_OK || value < 1 || value > poolPtr->threads.max) {
		Tcl_AppendResult(interp, "argument is not a integer in the valid range: ", optArg, NULL);
		return TCL_ERROR;
	    }
	    Ns_MutexLock(&poolPtr->threads.lock);
	    poolPtr->threads.min = value;
	    Ns_MutexUnlock(&poolPtr->threads.lock);
	}
        Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->threads.min));
	break;
    
    case SConnectionsIdx:
        Tcl_SetObjResult(interp, Tcl_NewLongObj((long)poolPtr->stats.processed));
        break;

    case SStatsIdx:
        Tcl_DStringInit(dsPtr);

        Tcl_DStringAppendElement(dsPtr, "requests");
        snprintf(buf, sizeof(buf), "%lu", poolPtr->stats.processed);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "spools");
        snprintf(buf, sizeof(buf), "%lu", poolPtr->stats.spool);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "queued");
        snprintf(buf, sizeof(buf), "%lu", poolPtr->stats.queued);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "connthreads");
        snprintf(buf, sizeof(buf), "%lu", poolPtr->stats.connthreads);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "accepttime");
	Ns_DStringPrintf(dsPtr, " %" PRIu64 ".%06ld", 
			 (int64_t)poolPtr->stats.acceptTime.sec, 
			 poolPtr->stats.acceptTime.usec);

        Tcl_DStringAppendElement(dsPtr, "queuetime");
	Ns_DStringPrintf(dsPtr, " %" PRIu64 ".%06ld", 
		 (int64_t)poolPtr->stats.queueTime.sec,
		 poolPtr->stats.queueTime.usec);

        Tcl_DStringAppendElement(dsPtr, "filtertime");
	Ns_DStringPrintf(dsPtr, " %" PRIu64 ".%06ld", 
		 (int64_t)poolPtr->stats.filterTime.sec,
		 poolPtr->stats.filterTime.usec);
	
	Tcl_DStringAppendElement(dsPtr, "runtime");
	Ns_DStringPrintf(dsPtr, " %" PRIu64 ".%06ld", 
		 (int64_t)poolPtr->stats.runTime.sec, 
		 poolPtr->stats.runTime.usec);

        Tcl_DStringResult(interp, dsPtr);
        break;

    case SThreadsIdx:
        Ns_TclPrintfResult(interp,
            "min %d max %d current %d idle %d stopping 0",
            poolPtr->threads.min, poolPtr->threads.max,
            poolPtr->threads.current, poolPtr->threads.idle);
        break;

    case SActiveIdx:
    case SQueuedIdx:
    case SAllIdx:
        Tcl_DStringInit(dsPtr);
        if (subcmd != SQueuedIdx) {
	    int i;
	    Ns_MutexLock(&poolPtr->tqueue.lock);
	    for (i=0; i < poolPtr->threads.max; i++) {
	        ConnThreadArg *argPtr = &poolPtr->tqueue.args[i];
		if (argPtr->connPtr != NULL) {
		    AppendConnList(dsPtr, argPtr->connPtr, "running");
		}
	    }
	    Ns_MutexUnlock(&poolPtr->tqueue.lock);
        }
        if (subcmd != SActiveIdx) {
	    Ns_MutexLock(&poolPtr->wqueue.lock);
            AppendConnList(dsPtr, poolPtr->wqueue.wait.firstPtr, "queued");
	    Ns_MutexUnlock(&poolPtr->wqueue.lock);
        }
        Tcl_DStringResult(interp, dsPtr);
        break;
    default:
        /* should never happen */
        assert(subcmd && 0);
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartServer --
 *
 *      Start the core connection thread interface.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Minimum connection threads may be created.
 *
 *----------------------------------------------------------------------
 */

void
NsStartServer(const NsServer *servPtr)
{
    ConnPool *poolPtr;
    int       n;

    NS_NONNULL_ASSERT(servPtr != NULL);

    poolPtr = servPtr->pools.firstPtr;
    while (poolPtr != NULL) {
        poolPtr->threads.idle = 0;
        poolPtr->threads.current = poolPtr->threads.min;
	poolPtr->threads.creating = poolPtr->threads.min;
        for (n = 0; n < poolPtr->threads.min; ++n) {
            CreateConnThread(poolPtr);
        }
        poolPtr = poolPtr->nextPtr;
    }
    NsAsyncWriterQueueEnable();
}


/*
 *----------------------------------------------------------------------
 *
 * WakeupConnThreads --
 *
 *      Wake up every idle connection thread of the specified pool.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
WakeupConnThreads(ConnPool *poolPtr) {
    int i;

    NS_NONNULL_ASSERT(poolPtr != NULL);

    Ns_MutexLock(&poolPtr->tqueue.lock);
    for (i = 0; i < poolPtr->threads.max; i++) {
	ConnThreadArg *argPtr = &poolPtr->tqueue.args[i];

	if (argPtr->state == connThread_idle) {
	    assert(argPtr->connPtr == NULL);
	    Ns_MutexLock(&argPtr->lock);
	    Ns_CondSignal(&argPtr->cond);
	    Ns_MutexUnlock(&argPtr->lock);
	}
    }
    Ns_MutexUnlock(&poolPtr->tqueue.lock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsStopServer --
 *
 *      Signal and wait for connection threads to exit.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsStopServer(NsServer *servPtr)
{
    ConnPool *poolPtr;

    NS_NONNULL_ASSERT(servPtr != NULL);

    Ns_Log(Notice, "server [%s]: stopping", servPtr->server);
    servPtr->pools.shutdown = NS_TRUE;
    poolPtr = servPtr->pools.firstPtr;
    while (poolPtr != NULL) {
	WakeupConnThreads(poolPtr);
        poolPtr = poolPtr->nextPtr;
    }
}

void
NsWaitServer(NsServer *servPtr, const Ns_Time *toPtr)
{
    ConnPool  *poolPtr;
    Ns_Thread  joinThread;
    int        status;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(toPtr != NULL);

    status = NS_OK;
    poolPtr = servPtr->pools.firstPtr;
    Ns_MutexLock(&servPtr->pools.lock);
    while (poolPtr != NULL && status == NS_OK) {
        while (status == NS_OK &&
               (poolPtr->wqueue.wait.firstPtr != NULL
                || poolPtr->threads.current > 0)) {
            status = Ns_CondTimedWait(&poolPtr->wqueue.cond,
                                      &servPtr->pools.lock, toPtr);
        }
        poolPtr = poolPtr->nextPtr;
    }
    joinThread = servPtr->pools.joinThread;
    servPtr->pools.joinThread = NULL;
    Ns_MutexUnlock(&servPtr->pools.lock);
    if (status != NS_OK) {
      Ns_Log(Warning, "server [%s]: timeout waiting for connection thread exit", servPtr->server);
    } else {
        if (joinThread != NULL) {
            JoinConnThread(&joinThread);
        }
        Ns_Log(Notice, "server [%s]: connection threads stopped", servPtr->server);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsConnArgProc --
 *
 *      Ns_GetProcInfo callback for a running conn thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See AppendConn.
 *
 *----------------------------------------------------------------------
 */

void
NsConnArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const ConnThreadArg *argPtr = arg;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (arg != NULL) {
        ConnPool     *poolPtr = argPtr->poolPtr;

	Ns_MutexLock(&poolPtr->tqueue.lock);
        AppendConn(dsPtr, argPtr->connPtr, "running");
	Ns_MutexUnlock(&poolPtr->tqueue.lock);
    } else {
        Tcl_DStringAppendElement(dsPtr, "");
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsConnThread --
 *
 *      Main connection service thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are removed from the waiting queue and serviced.
 *
 *----------------------------------------------------------------------
 */

void
NsConnThread(void *arg)
{
    ConnThreadArg *argPtr = arg;
    ConnPool      *poolPtr = argPtr->poolPtr;
    NsServer      *servPtr = poolPtr->servPtr;
    Conn          *connPtr = NULL;
    Ns_Time        wait, *timePtr = &wait;
    uintptr_t      id;
    bool           shutdown;
    int            status = NS_OK, cpt, ncons, current, fromQueue;
    long           timeout;
    const char    *exitMsg;
    Ns_Mutex      *threadsLockPtr = &poolPtr->threads.lock;
    Ns_Mutex      *tqueueLockPtr  = &poolPtr->tqueue.lock;
    Ns_Mutex      *wqueueLockPtr  = &poolPtr->wqueue.lock;
    Ns_Thread      joinThread;

    NS_NONNULL_ASSERT(arg != NULL);

    /*
     * Set the ConnThreadArg into thread local storage and get the id
     * of the thread.
     */

    Ns_TlsSet(&argtls, argPtr);
    Ns_MutexLock(tqueueLockPtr);
    argPtr->state = connThread_warmup;
    Ns_MutexUnlock(tqueueLockPtr);

    Ns_MutexLock(threadsLockPtr);
    id = poolPtr->threads.nextid++;
    Ns_MutexUnlock(threadsLockPtr);

    /*
     * Set the conn thread name. The pool name is always non-null, but
     * might have an empty string as "name".
     */
    assert(poolPtr->pool != NULL);
    {
        const char *p = *poolPtr->pool != '\0' ? poolPtr->pool : NULL;
        Ns_ThreadSetName("-conn:%s%s%s:%" PRIuPTR "-",
                         servPtr->server, 
			 (p != NULL) ? ":" : "", 
			 (p != NULL) ? p : "",
                         id);
    }

    Ns_ThreadSelf(&joinThread);
    /*fprintf(stderr, "### starting conn thread %p <%s>\n", (void *)joinThread, Ns_ThreadGetName());*/
    
    cpt     = poolPtr->threads.connsperthread;
    ncons   = cpt;
    timeout = poolPtr->threads.timeout;

    /*
     * Initialize the connection thread with the blueprint to avoid
     * the initialization delay when the first connection comes in.
     */
    {
	Tcl_Interp *interp;
	Ns_Time     start, end, diff;

        Ns_GetTime(&start);
	interp = NsTclAllocateInterp(servPtr);
        Ns_GetTime(&end);
        Ns_DiffTime(&end, &start, &diff);
	Ns_Log(Notice, "thread initialized (%" PRIu64 ".%06ld secs)", 
	       (int64_t)diff.sec, diff.usec);
	Ns_TclDeAllocateInterp(interp);
	argPtr->state = connThread_ready;
    }

    /*
     * Start handling connections.
     */

    Ns_MutexLock(threadsLockPtr);
    if (poolPtr->threads.creating > 0) {
	poolPtr->threads.creating--;
    }
    Ns_MutexUnlock(threadsLockPtr);


    while (1) {

	/*
	 * We are ready to process requests. Pick it either a request
	 * from the waiting queue, or go to a waiting state and add
	 * jourself to the conn thread queue.
	 */
	assert(argPtr->connPtr == NULL);
	assert(argPtr->state == connThread_ready);

	if (poolPtr->wqueue.wait.firstPtr != NULL) {
	    connPtr = NULL;	
	    Ns_MutexLock(wqueueLockPtr);
	    if (poolPtr->wqueue.wait.firstPtr != NULL) {
		/* 
		 * There are waiting requests.  Pull the first connection of
		 * the waiting list and assign it to the ConnThreadArg.
		 */
		connPtr = poolPtr->wqueue.wait.firstPtr;
		poolPtr->wqueue.wait.firstPtr = connPtr->nextPtr;
		if (poolPtr->wqueue.wait.lastPtr == connPtr) {
		    poolPtr->wqueue.wait.lastPtr = NULL;
		}
		connPtr->nextPtr = NULL;
		poolPtr->wqueue.wait.num --;
	    }
	    Ns_MutexUnlock(wqueueLockPtr);
	  
	    argPtr->connPtr = connPtr;
	    fromQueue = 1;
	} else {
	    fromQueue = 0;
	}
      
	if (argPtr->connPtr == NULL) {
	    /*
	     * There is nothing urgent to do. We can add ourself to the
	     * conn thread queue.
	     */
	    Ns_MutexLock(threadsLockPtr);
	    poolPtr->threads.idle ++;
	    Ns_MutexUnlock(threadsLockPtr);

	    Ns_MutexLock(tqueueLockPtr);
	    argPtr->state = connThread_idle;
	    /*
	     * We put an entry into the thread queue. However, we must
	     * take care, that signals are not sent, before this thread
	     * is waiting for it. Therefore we lock the connection
	     * thread specific lock right here, also the signal sending
	     * code uses the same lock.
	     */
	    Ns_MutexLock(&argPtr->lock);

	    argPtr->nextPtr = poolPtr->tqueue.nextPtr; 
	    poolPtr->tqueue.nextPtr = argPtr;
	    Ns_MutexUnlock(tqueueLockPtr);

	    /*
	     * Wait until someone wakes us up, or a timeout happens.
	     */
	    while (servPtr->pools.shutdown == NS_FALSE) {

		Ns_GetTime(timePtr);
		Ns_IncrTime(timePtr, timeout, 0);
		
		status = Ns_CondTimedWait(&argPtr->cond, &argPtr->lock, timePtr);
		
		if (status == NS_TIMEOUT) {
		    if (argPtr->connPtr != NULL) {
			/* 
                         * This should not happen: we had a timeout, but there
                         * is a connection to be handled; when a connection
                         * comes in, we get signaled and should see therefore
                         * no timeout.  Maybe the signal was lost?
                         */
			Ns_Log(Warning, "signal lost, resuming after timeout");
			status = NS_OK;

		    } else if (poolPtr->threads.current <= poolPtr->threads.min) {
                        /* 
                         * We have a timeout, but we should not reduce the
                         * number of threads below min-threads.
                         */
                        continue;
                        
                    } else {
                        /* 
                         * We have a timeout, and the thread can exit 
                         */
                        break;
                    }
		}

		if (argPtr->connPtr != NULL) {
                    /*
                     * We got something to do
                     */
                    break;
                }

                Ns_Log(Debug, "Unexpected condition after CondTimedWait; maybe shutdown?");
	    }

	    Ns_MutexUnlock(&argPtr->lock);

	    assert(argPtr->state == connThread_idle);
	    
	    if (argPtr->connPtr == NULL) {
		/* 
		 * We were not signaled on purpose, so we have to dequeue
		 * the current thread.
		 */ 
		ConnThreadArg *aPtr, **prevPtr;
		
		Ns_MutexLock(tqueueLockPtr);
		for (aPtr = poolPtr->tqueue.nextPtr, prevPtr = &poolPtr->tqueue.nextPtr;
		     aPtr; 
		     prevPtr = &aPtr->nextPtr, aPtr = aPtr->nextPtr) {
		    if (aPtr == argPtr) {
			*prevPtr = aPtr->nextPtr;
			argPtr->nextPtr = NULL;
			break;
		    }
		}
		argPtr->state = connThread_busy;
		Ns_MutexUnlock(tqueueLockPtr);
	    } else {
		Ns_MutexLock(tqueueLockPtr);
		argPtr->state = connThread_busy;
		Ns_MutexUnlock(tqueueLockPtr);
	    }
	    
	    Ns_MutexLock(threadsLockPtr);
	    poolPtr->threads.idle --;
	    Ns_MutexUnlock(threadsLockPtr);
	    
	    if (servPtr->pools.shutdown == NS_TRUE) {
		exitMsg = "shutdown pending";
		break;
	    } else if (status == NS_TIMEOUT) {
		exitMsg = "idle thread terminates";
		break;
	    }
	}
	
	connPtr = argPtr->connPtr;
	assert(connPtr != NULL);

	Ns_GetTime(&connPtr->requestDequeueTime);

	/*
	 * Run the connection.
	 */
	ConnRun(argPtr, connPtr);

        /*
         * push connection to the free list.
         */
        argPtr->connPtr = NULL;

        if (connPtr->prevPtr != NULL) {
            connPtr->prevPtr->nextPtr = connPtr->nextPtr;
        }
        if (connPtr->nextPtr != NULL) {
            connPtr->nextPtr->prevPtr = connPtr->prevPtr;
        }
        connPtr->prevPtr = NULL;

        Ns_MutexLock(wqueueLockPtr);
        connPtr->nextPtr = poolPtr->wqueue.freePtr;
        poolPtr->wqueue.freePtr = connPtr;
	Ns_MutexUnlock(wqueueLockPtr);

	Ns_MutexLock(tqueueLockPtr);
	argPtr->state = connThread_ready;
	Ns_MutexUnlock(tqueueLockPtr);

	if (cpt != 0) {
	    int waiting, idle, lowwater;

	    --ncons;
	    
	    /*
	     * Get a consistent snapshot of the controlling variables.
	     */
            Ns_MutexLock(wqueueLockPtr);
            Ns_MutexLock(threadsLockPtr);
	    waiting  = poolPtr->wqueue.wait.num;
	    lowwater = poolPtr->wqueue.lowwatermark;
	    idle     = poolPtr->threads.idle;
	    current  = poolPtr->threads.current;
            Ns_MutexUnlock(threadsLockPtr);
            Ns_MutexUnlock(wqueueLockPtr);

	    if (Ns_LogSeverityEnabled(Debug)) {
		Ns_Time now, acceptTime, queueTime, filterTime, netRunTime, runTime, fullTime;

		Ns_DiffTime(&connPtr->requestQueueTime, &connPtr->acceptTime, &acceptTime);
		Ns_DiffTime(&connPtr->requestDequeueTime, &connPtr->requestQueueTime, &queueTime);
		Ns_DiffTime(&connPtr->filterDoneTime, &connPtr->requestDequeueTime, &filterTime);

		Ns_GetTime(&now);
		Ns_DiffTime(&now, &connPtr->requestDequeueTime, &runTime);
		Ns_DiffTime(&now, &connPtr->filterDoneTime,     &netRunTime);
		Ns_DiffTime(&now, &connPtr->requestQueueTime,   &fullTime);

		Ns_Log(Debug, "[%ld] end of job, waiting %d current %d idle %d ncons %d fromQueue %d"
		       " start %" PRIu64 ".%06ld"
		       " %" PRIu64 ".%06ld"
		       " accept %" PRIu64 ".%06ld"
		       " queue %" PRIu64 ".%06ld"
		       " filter %" PRIu64 ".%06ld"
		       " run %" PRIu64 ".%06ld"
		       " netrun %" PRIu64 ".%06ld"
		       " total %" PRIu64 ".%06ld",
		       ThreadNr(poolPtr, argPtr),
		       waiting, poolPtr->threads.current, idle, ncons, fromQueue,
		       (int64_t) connPtr->acceptTime.sec, connPtr->acceptTime.usec,
		       (int64_t) connPtr->requestQueueTime.sec, connPtr->requestQueueTime.usec,
		       (int64_t) acceptTime.sec, acceptTime.usec,
		       (int64_t) queueTime.sec, queueTime.usec,
		       (int64_t) filterTime.sec, filterTime.usec,
		       (int64_t) runTime.sec, runTime.usec,
		       (int64_t) netRunTime.sec, netRunTime.usec,
		       (int64_t) fullTime.sec, fullTime.usec
		       );
	    }
	    
	    if (waiting > 0) {
		/* 
		 * There are waiting requests. Work on those unless we
		 * are expiring or we are already under the lowwater
		 * mark of connection threads, or we are the last man
		 * standing.
		 */
		if (ncons > 0 || waiting > lowwater || current <= 1) {
		    continue;
		}
	    }
	    
	    if (ncons <= 0) {
		exitMsg = "exceeded max connections per thread";
		break;
	    }
	} else if (ncons <= 0) {
	  /* Served given # of connections in this thread */
	  exitMsg = "exceeded max connections per thread";
	  break;
        }
    }
    argPtr->state = connThread_dead;

    Ns_MutexLock(&servPtr->pools.lock);
    shutdown = servPtr->pools.shutdown;
    Ns_MutexUnlock(&servPtr->pools.lock);


    {
        bool wakeup; 
	/*
	 * Record the fact that this driver is exiting by decrementing the
	 * actually running threads and wakeup the driver to check against
	 * thread starvation (due to an insufficient number of connection
	 * threads).
	 */
	Ns_MutexLock(threadsLockPtr);
	poolPtr->threads.current--;
	wakeup = (poolPtr->threads.current < poolPtr->threads.min);
	Ns_MutexUnlock(threadsLockPtr);
	
	/* 
	 * During shutdown, we do not want to restart connection
	 * threads. The driver pointer might be already invalid. 
	 */
	if (wakeup == NS_TRUE && connPtr != NULL && shutdown == NS_FALSE) {
            assert(connPtr->drvPtr != NULL);
	    NsWakeupDriver(connPtr->drvPtr); 
	} 
    }
    
    /*
     * During shutdown, the main thread waits for signals on the
     * condition variable to check whether all threads have terminated
     * already.
     */
    if (shutdown == NS_TRUE) {
	Ns_CondSignal(&poolPtr->wqueue.cond); 
    }

    Ns_MutexLock(&servPtr->pools.lock);
    joinThread = servPtr->pools.joinThread;
    Ns_ThreadSelf(&servPtr->pools.joinThread);
    /*fprintf(stderr, "###stopping joinThread %p, self %p\n",
      joinThread, servPtr->pools.joinThread);*/
    
    if (joinThread != NULL) {
        JoinConnThread(&joinThread);
    }
    Ns_MutexUnlock(&servPtr->pools.lock);
    
    Ns_Log(Notice, "exiting: %s", exitMsg);

    Ns_MutexLock(tqueueLockPtr);
    argPtr->state = connThread_free; 
    Ns_MutexUnlock(tqueueLockPtr);

    Ns_ThreadExit(argPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * ConnRun --
 *
 *      Run a valid connection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connection request is read and parsed and the corresponding
 *      service routine is called.
 *
 *----------------------------------------------------------------------
 */
static void
ConnRun(const ConnThreadArg *argPtr, Conn *connPtr)
{
    Ns_Conn  *conn = (Ns_Conn *) connPtr;
    NsServer *servPtr = connPtr->poolPtr->servPtr;
    int       status = NS_OK;
    Sock     *sockPtr = connPtr->sockPtr;
    char     *auth;

    NS_NONNULL_ASSERT(argPtr != NULL);
    NS_NONNULL_ASSERT(connPtr != NULL);

    /*
     * Re-initialize and run the connection. 
     */
    if (sockPtr != NULL) {
	connPtr->reqPtr = NsGetRequest(sockPtr, &connPtr->requestDequeueTime);
    } else {
	connPtr->reqPtr = NULL;
    }

    if (connPtr->reqPtr == NULL) {
	Ns_Log(Warning, "connPtr %p has no reqPtr, close this connection", (void *)connPtr);
        (void) Ns_ConnClose(conn);
        return;
    }
    assert(sockPtr != NULL);

    /*
     * Make sure we update peer address with actual remote IP address
     */
    (void) Ns_ConnSetPeer(conn, &sockPtr->sa);

    connPtr->request = &connPtr->reqPtr->request;

    /*{ConnPool *poolPtr = argPtr->poolPtr;
    Ns_Log(Notice,"ConnRun [%d] connPtr %p req %p %s", ThreadNr(poolPtr, argPtr), connPtr, connPtr->request, connPtr->request->line);
    } */   
    connPtr->headers = connPtr->reqPtr->headers;
    connPtr->contentLength = connPtr->reqPtr->length;

    connPtr->nContentSent = 0u;
    connPtr->responseStatus = 200;
    connPtr->responseLength = -1;  /* -1 == unknown (stream), 0 == zero bytes. */
    connPtr->recursionCount = 0;
    connPtr->auth = NULL;

    connPtr->keep = -1;            /* Undecided, default keep-alive rules apply */

    Ns_ConnSetCompression(conn, (servPtr->compress.enable != 0) ? servPtr->compress.level : 0);
    connPtr->compress = -1;

    connPtr->outputEncoding = servPtr->encoding.outputEncoding;
    connPtr->urlEncoding = servPtr->encoding.urlEncoding;

    Tcl_InitHashTable(&connPtr->files, TCL_STRING_KEYS);
    snprintf(connPtr->idstr, sizeof(connPtr->idstr), "cns%" PRIuPTR, connPtr->id);
    connPtr->outputheaders = Ns_SetCreate(NULL);
    if (connPtr->request->version < 1.0) {
        conn->flags |= NS_CONN_SKIPHDRS;
    }
    if (servPtr->opts.hdrcase != Preserve) {
	size_t i;

        for (i = 0U; i < Ns_SetSize(connPtr->headers); ++i) {
            if (servPtr->opts.hdrcase == ToLower) {
                Ns_StrToLower(Ns_SetKey(connPtr->headers, i));
            } else {
                Ns_StrToUpper(Ns_SetKey(connPtr->headers, i));
            }
        }
    }
    auth = Ns_SetIGet(connPtr->headers, "authorization");
    if (auth != NULL) {
        NsParseAuth(connPtr, auth);
    }
    if (conn->request->method != NULL && STREQ(conn->request->method, "HEAD")) {
        conn->flags |= NS_CONN_SKIPBODY;
    }

    /*
     * Run the driver's private handler
     */

    if (connPtr->sockPtr->drvPtr->requestProc != NULL) {
        status = (*sockPtr->drvPtr->requestProc)(sockPtr->drvPtr->arg, conn);
    }

    /*
     * provide a default filterDoneTime
     */
    Ns_GetTime(&connPtr->filterDoneTime);

    /*
     * Run the rest of the request.
     */

    if (connPtr->request->protocol != NULL && connPtr->request->host != NULL) {
        status = NsConnRunProxyRequest((Ns_Conn *) connPtr);
    } else {
        if (status == NS_OK) {
            status = NsRunFilters(conn, NS_FILTER_PRE_AUTH);
	    Ns_GetTime(&connPtr->filterDoneTime);
        }
        if (status == NS_OK) {
            status = Ns_AuthorizeRequest(servPtr->server,
                                         connPtr->request->method,
                                         connPtr->request->url,
                                         Ns_ConnAuthUser(conn),
                                         Ns_ConnAuthPasswd(conn),
                                         Ns_ConnPeer(conn));
            switch (status) {
            case NS_OK:
                status = NsRunFilters(conn, NS_FILTER_POST_AUTH);
		Ns_GetTime(&connPtr->filterDoneTime);
                if (status == NS_OK) {
                    status = Ns_ConnRunRequest(conn);
                }
                break;

            case NS_FORBIDDEN:
                (void) Ns_ConnReturnForbidden(conn);
                break;

            case NS_UNAUTHORIZED:
                (void) Ns_ConnReturnUnauthorized(conn);
                break;

            case NS_ERROR:
            default:
                (void) Ns_ConnReturnInternalError(conn);
                break;
            }
        } else if (status != NS_FILTER_RETURN) {
            /*
             * If not ok or filter_return, then the pre-auth filter coughed
             * an error.  We are not going to proceed, but also we
             * can't count on the filter to have sent a response
             * back to the client.  So, send an error response.
             */
            (void) Ns_ConnReturnInternalError(conn);
            status = NS_FILTER_RETURN; /* to allow tracing to happen */
        }
    }

    (void) Ns_ConnClose(conn);
    Ns_ConnTimeStats(conn);

    if (status == NS_OK || status == NS_FILTER_RETURN) {
        status = NsRunFilters(conn, NS_FILTER_TRACE);
        if (status == NS_OK) {
            (void) NsRunFilters(conn, NS_FILTER_VOID_TRACE);
            NsRunTraces(conn);
        }
    }

    /*
     * Perform various garbage collection tasks.  Note
     * the order is significant:  The driver freeProc could
     * possibly use Tcl and Tcl deallocate callbacks
     * could possibly access header and/or request data.
     */

    NsRunCleanups(conn);
    NsClsCleanup(connPtr);
    NsFreeConnInterp(connPtr);

    /*
     * Deactivate stream writer, if defined
     */
    if (connPtr->fd != 0) {
	connPtr->fd = 0;
    }
    if (connPtr->strWriter != NULL) {
	WriterSock *wrPtr;

	NsWriterLock();
	wrPtr = connPtr->strWriter;
	if (wrPtr != NULL) {
	    NsWriterFinish(wrPtr);
	    connPtr->strWriter = NULL;
	}
	NsWriterUnlock();
    }

    /*
     * Free Structures
     */
    Ns_ConnClearQuery(conn);
    Ns_SetFree(connPtr->auth);
    connPtr->auth = NULL;
    Ns_SetFree(connPtr->outputheaders);
    connPtr->outputheaders = NULL;
    NsFreeRequest(connPtr->reqPtr);
    connPtr->reqPtr = NULL;
    if (connPtr->clientData != NULL) {
        ns_free(connPtr->clientData);
        connPtr->clientData = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * CreateConnThread --
 *
 *      Create a connection thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      New thread.
 *
 *----------------------------------------------------------------------
 */

static void
CreateConnThread(ConnPool *poolPtr)
{
    Ns_Thread      thread;
    ConnThreadArg *argPtr = NULL;
    int i;

#if !defined(NDEBUG)
    { const char *threadName = Ns_ThreadGetName();
      assert(strncmp("-driver:", threadName, 8U) == 0 
	     || strncmp("-main-", threadName, 6U) == 0
	     || strncmp("-spooler", threadName, 8U) == 0
	     || strncmp("-service-", threadName, 9U) == 0
	     );
    }
#endif

    NS_NONNULL_ASSERT(poolPtr != NULL);

    /*
     * Get first free connection thread slot; selecting a slot and
     * occupying it has to be done under a mutex lock, since we do not
     * want someone else to pick the same. We are competing
     * potentially against driver/spooler threads and the main thread.
     *
     * TODO: Maybe we could do better than the linear search, but the queue
     * is usually short...
     */
    Ns_MutexLock(&poolPtr->tqueue.lock);
    for (i = 0; likely(i < poolPtr->threads.max); i++) {
      if (poolPtr->tqueue.args[i].state == connThread_free) {
	argPtr = &(poolPtr->tqueue.args[i]);
	break;
      }
    }
    if (likely(argPtr != NULL)) {
        argPtr->state = connThread_initial;
        poolPtr->stats.connthreads++;
        Ns_MutexUnlock(&poolPtr->tqueue.lock);

        /* Ns_Log(Notice, "CreateConnThread use thread slot [%d]", i);*/

        argPtr->poolPtr = poolPtr;
        argPtr->connPtr = NULL;
        argPtr->nextPtr = NULL;
        argPtr->cond = NULL;
        
        Ns_ThreadCreate(NsConnThread, argPtr, 0, &thread);
    } else {
        Ns_MutexUnlock(&poolPtr->tqueue.lock);
        Ns_Log(Notice, "Cannot create connection thread, all available slots (%d) are used\n", i);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * JoinConnThread --
 *
 *      Join a connection thread, freeing the threads connPtrPtr.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
JoinConnThread(Ns_Thread *threadPtr)
{
    NS_NONNULL_ASSERT(threadPtr != NULL);

    Ns_ThreadJoin(threadPtr, NULL);
    /*
     * There is no need to free ConnThreadArg here, since it is
     * allocated in the driver
     */
}


/*
 *----------------------------------------------------------------------
 *
 * AppendConn --
 *
 *      Append connection data to a dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
AppendConn(Tcl_DString *dsPtr, const Conn *connPtr, const char *state)
{
    Ns_Time now, diff;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(state != NULL);

    Tcl_DStringStartSublist(dsPtr);

    /*
     * An annoying race condition can be lethal here.
     */
    if (connPtr != NULL) {
	char  buf[100];

        Tcl_DStringAppendElement(dsPtr, connPtr->idstr);
	if (connPtr->reqPtr != NULL) {
	    Tcl_DStringAppendElement(dsPtr, Ns_ConnPeer((const Ns_Conn *) connPtr));
	} else {
	    /* Actually, this should not happen, but it does, maybe due
	     * to the above mentioned race condition; we notice in the
	     * errlog the fact and return a placeholder value
	     */
	    Ns_Log(Notice, "AppendConn: no reqPtr in state %s; ignore conn in output", state);
	    Tcl_DStringAppendElement(dsPtr, "unknown");
	}
        Tcl_DStringAppendElement(dsPtr, state);

        if (connPtr->request != NULL) {
            Tcl_DStringAppendElement(dsPtr, (connPtr->request->method != NULL) ? connPtr->request->method : "?");
            Tcl_DStringAppendElement(dsPtr, (connPtr->request->url    != NULL) ? connPtr->request->url : "?");
	} else {
	    Ns_Log(Notice, "AppendConn: no request in state %s; ignore conn in output", state);
	    Tcl_DStringAppendElement(dsPtr, "unknown");
	    Tcl_DStringAppendElement(dsPtr, "unknown");
	}
	Ns_GetTime(&now);
        Ns_DiffTime(&now, &connPtr->requestQueueTime, &diff);
        snprintf(buf, sizeof(buf), "%" PRId64 ".%06ld", (int64_t) diff.sec, diff.usec);
        Tcl_DStringAppendElement(dsPtr, buf);
        snprintf(buf, sizeof(buf), "%" PRIuz, connPtr->nContentSent);
        Tcl_DStringAppendElement(dsPtr, buf);
    }
    Tcl_DStringEndSublist(dsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * AppendConnList --
 *
 *      Append list of connection data to a dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
AppendConnList(Tcl_DString *dsPtr, const Conn *firstPtr, const char *state)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(state != NULL);

    while (firstPtr != NULL) {
        AppendConn(dsPtr, firstPtr, state);
        firstPtr = firstPtr->nextPtr;
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
