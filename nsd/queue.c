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
#include <math.h>

/*
 * Local functions defined in this file
 */

static void ConnRun(ConnThreadArg *argPtr, Conn *connPtr); 
static void CreateConnThread(ConnPool *poolPtr);
static void JoinConnThread(Ns_Thread *threadPtr);
static void AppendConn(Tcl_DString *dsPtr, Conn *connPtr, char *state);
static void AppendConnList(Tcl_DString *dsPtr, Conn *firstPtr, char *state);

/*
 * Static variables defined in this file.
 */

static Ns_Tls argtls;
static int    poolid;

/*
 * Debugging stuff
 */
#define ThreadNr(poolPtr, argPtr) (int)(argPtr ? (argPtr - poolPtr->tqueue.args) : -1)

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
    return (argPtr ? ((Ns_Conn *) argPtr->connPtr) : NULL);
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
NsMapPool(ConnPool *poolPtr, char *map)
{
    char *server = poolPtr->servPtr->server;
    char **mv;
    int  mc;

    if (Tcl_SplitList(NULL, map, &mc, (CONST char***)&mv) == TCL_OK) {
        if (mc == 2) {
            Ns_UrlSpecificSet(server, mv[0], mv[1], poolid, poolPtr, 0, NULL);
            Ns_Log(Notice, "pool[%s]: mapped %s %s -> %s", 
		   server, mv[0], mv[1], poolPtr->pool);
        }
        ckfree((char *) mv);
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
neededAdditionalConnectionThreads(ConnPool *poolPtr) {
    int wantCreate;

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
	 //&& poolPtr->threads.idle < poolPtr->threads.min
	 //&& poolPtr->threads.idle < 1
	 && (poolPtr->threads.current < poolPtr->threads.min
	     || (poolPtr->wqueue.wait.num > poolPtr->wqueue.lowwatermark)
	     //	     || (poolPtr->threads.idle < 1)
	     )
	 && poolPtr->threads.current < poolPtr->threads.max
	 ) {
      //wantCreate = poolPtr->threads.min - poolPtr->threads.idle;
      wantCreate = 1;
      
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
NsEnsureRunningConnectionThreads(NsServer *servPtr, ConnPool *poolPtr) {
    int create;

    assert(servPtr);

    if (poolPtr == NULL) {
        /* 
	 * Use the default pool for the time being, if no pool was
	 * provided
	 */
        poolPtr = servPtr->pools.defaultPtr;
    }

    Ns_MutexLock(&servPtr->pools.lock);

    Ns_MutexLock(&poolPtr->wqueue.lock);
    create = neededAdditionalConnectionThreads(poolPtr);
    Ns_MutexUnlock(&poolPtr->wqueue.lock);

    if (create) {
	poolPtr->threads.current ++;
	poolPtr->threads.creating ++;
    }

    Ns_MutexUnlock(&servPtr->pools.lock);

    if (create) {
        Ns_Log(Notice, "NsEnsureRunningConnectionThreads wantCreate %d", create);
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
NsQueueConn(Sock *sockPtr, Ns_Time *nowPtr)
{
    ConnThreadArg *argPtr = NULL;
    NsServer *servPtr = sockPtr->servPtr;
    ConnPool *poolPtr = NULL;
    Conn     *connPtr = NULL;
    int       create = 0;

    /*
     * Select server connection pool.
     */

    if (sockPtr->reqPtr != NULL) {
        poolPtr = NsUrlSpecificGet(servPtr,
                                   sockPtr->reqPtr->request.method,
                                   sockPtr->reqPtr->request.url,
                                   poolid, 0);
    }
    if (poolPtr == NULL) {
        poolPtr = servPtr->pools.defaultPtr;
    }

   /*
    * Queue connection if a free Conn is available.
    */
    //Ns_Log(Notice, "NsQueueConn sock %p reqPtr %p request %p argPtr %p",  
    //	   sockPtr, sockPtr->reqPtr, &sockPtr->reqPtr->request, poolPtr->tqueue.nextPtr);
   
    if (!servPtr->pools.shutdown) {

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
	  
	    //ConnThreadQueuePrint(poolPtr, "driver");

	    Ns_MutexLock(&servPtr->pools.lock);
            connPtr->id        = servPtr->pools.nextconnid++;
	    Ns_MutexUnlock(&servPtr->pools.lock);

            connPtr->requestQueueTime    = *nowPtr;
            connPtr->sockPtr             = sockPtr;
            connPtr->drvPtr              = sockPtr->drvPtr;
            connPtr->servPtr             = servPtr;
            connPtr->server              = servPtr->server;
            connPtr->location            = sockPtr->location;
	    connPtr->acceptTime          = sockPtr->acceptTime;
	    connPtr->flags = 0;

	    if (sockPtr->flags & NS_CONN_ENTITYTOOLARGE) {
	        connPtr->flags |= NS_CONN_ENTITYTOOLARGE;
		sockPtr->flags &= ~NS_CONN_ENTITYTOOLARGE;
	    } else if (sockPtr->flags & NS_CONN_REQUESTURITOOLONG) {
	        connPtr->flags |= NS_CONN_REQUESTURITOOLONG;
		sockPtr->flags &= ~NS_CONN_REQUESTURITOOLONG;
	    } else if (sockPtr->flags & NS_CONN_LINETOOLONG) {
	        connPtr->flags |= NS_CONN_LINETOOLONG;
		sockPtr->flags &= ~NS_CONN_LINETOOLONG;
	    }

	    /*
	     * Try to get an entry from the connection thread queue,
	     * and dequeue it when possible.
	     */
	    if (poolPtr->tqueue.nextPtr) {
	        Ns_MutexLock(&poolPtr->tqueue.lock);
		if (poolPtr->tqueue.nextPtr) {
		  argPtr = poolPtr->tqueue.nextPtr;
		  poolPtr->tqueue.nextPtr = argPtr->nextPtr;
		}
	        Ns_MutexUnlock(&poolPtr->tqueue.lock);
	    }
	    /*fprintf(stderr, "NsQueueConn idle %d argPtr %p\n",  poolPtr->threads.idle, argPtr);*/

	    if (argPtr) {
		/* 
		 * We could obtain an idle thread. Dequeue the entry,
		 * such that noone else might grab it, and fill in the
		 * connPtr.
		 */
	        
		assert(argPtr->state == connThread_idle);
		argPtr->connPtr = connPtr;

		Ns_MutexLock(&poolPtr->wqueue.lock);
		create = neededAdditionalConnectionThreads(poolPtr);
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
		poolPtr->servPtr->stats.queued++;
		create = neededAdditionalConnectionThreads(poolPtr);
		Ns_MutexUnlock(&poolPtr->wqueue.lock);
	    }
        }
    }

    /*Ns_Log(Notice, "driver: NsQueueConn connPtr %p sock %p reqPtr %p request %p create %d argPtr %p thread [%d]", 
      connPtr, sockPtr, sockPtr->reqPtr, &sockPtr->reqPtr->request, create, argPtr, ThreadNr(poolPtr, argPtr));*/


    if (connPtr == NULL) {
	Ns_Log(Notice, "[%s] All avaliable connections are used, waiting %d idle %d current %d ",
	       poolPtr->servPtr->server, 
	       poolPtr->wqueue.wait.num,
	       poolPtr->threads.idle, 
	       poolPtr->threads.current);
	return 0;
    }

    if (argPtr) {
        //Ns_Log(Notice, "[%d] dequeue thread connPtr %p idle %d state %d create %d", 
	//       ThreadNr(poolPtr, argPtr), connPtr, poolPtr->threads.idle, argPtr->state, create);
	Ns_MutexLock(&argPtr->lock);
        Ns_CondSignal(&argPtr->cond);
	Ns_MutexUnlock(&argPtr->lock);
    } else {
        //Ns_Log(Notice, "[%d] add waiting connPtr %p => waiting %d create %d", 
	//       ThreadNr(poolPtr, argPtr), connPtr, poolPtr->wqueue.wait.num, create);
    }

    if (create) {
        Ns_Log(Notice, "NsQueueConn wantCreate %d", create);
        Ns_MutexLock(&servPtr->pools.lock);
	poolPtr->threads.current ++;
	poolPtr->threads.creating ++;
        Ns_MutexUnlock(&servPtr->pools.lock);
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
NsTclServerObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    int          opt;
    NsInterp    *itPtr = arg;
    NsServer    *servPtr = itPtr->servPtr;
    ConnPool    *poolPtr;
    char        *pool, buf[100];
    Tcl_DString ds, *dsPtr = &ds;

    static CONST char *opts[] = {
        "active", "all", "connections", "keepalive", "pools", "queued",
        "threads", "stats", "waiting", NULL,
    };

    enum {
        SActiveIdx, SAllIdx, SConnectionsIdx, SKeepaliveIdx, SPoolsIdx,
        SQueuedIdx, SThreadsIdx, SStatsIdx, SWaitingIdx,
    };

    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?pool?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }
    if (objc == 2) {
        poolPtr = servPtr->pools.defaultPtr;
    } else {
        pool = Tcl_GetString(objv[2]);
        poolPtr = servPtr->pools.firstPtr;
        while (poolPtr != NULL && !STREQ(poolPtr->pool, pool)) {
            poolPtr = poolPtr->nextPtr;
        }
        if (poolPtr == NULL) {
            Tcl_AppendResult(interp, "no such pool: ", pool, NULL);
            return TCL_ERROR;
        }
    }

    switch (opt) {
    case SPoolsIdx:
        poolPtr = servPtr->pools.firstPtr;
        while (poolPtr != NULL) {
            Tcl_AppendElement(interp, poolPtr->pool);
            poolPtr = poolPtr->nextPtr;
        }
        break;

    case SWaitingIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->wqueue.wait.num));
        break;

    case SKeepaliveIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        break;

    case SConnectionsIdx:
        Tcl_SetObjResult(interp, Tcl_NewLongObj(servPtr->pools.nextconnid));
        break;

    case SStatsIdx:
        Tcl_DStringInit(dsPtr);

        Tcl_DStringAppendElement(dsPtr, "requests");
        snprintf(buf, sizeof(buf), "%lu", servPtr->pools.nextconnid);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "spools");
        snprintf(buf, sizeof(buf), "%lu", servPtr->stats.spool);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "queued");
        snprintf(buf, sizeof(buf), "%lu", servPtr->stats.queued);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "connthreads");
        snprintf(buf, sizeof(buf), "%lu", servPtr->stats.connthreads);
        Tcl_DStringAppendElement(dsPtr, buf);

        Tcl_DStringAppendElement(dsPtr, "accepttime");
	Ns_DStringPrintf(dsPtr, " %" PRIu64 ".%06ld", 
			 (int64_t)servPtr->stats.acceptTime.sec, 
			 servPtr->stats.acceptTime.usec);

        Tcl_DStringAppendElement(dsPtr, "queuetime");
	Ns_DStringPrintf(dsPtr, " %" PRIu64 ".%06ld", 
		 (int64_t)servPtr->stats.queueTime.sec,
		 servPtr->stats.queueTime.usec);
	
	Tcl_DStringAppendElement(dsPtr, "runtime");
	Ns_DStringPrintf(dsPtr, " %" PRIu64 ".%06ld", 
		 (int64_t)servPtr->stats.runTime.sec, 
		 servPtr->stats.runTime.usec);

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
        if (opt != SQueuedIdx) {
	    int i;
	    Ns_MutexLock(&poolPtr->tqueue.lock);
	    for (i=0; i < poolPtr->threads.max; i++) {
	        ConnThreadArg *argPtr = &poolPtr->tqueue.args[i];
		if (argPtr->connPtr) {
		    AppendConnList(dsPtr, argPtr->connPtr, "running");
		}
	    }
	    Ns_MutexUnlock(&poolPtr->tqueue.lock);
        }
        if (opt != SActiveIdx) {
	    Ns_MutexLock(&poolPtr->wqueue.lock);
            AppendConnList(dsPtr, poolPtr->wqueue.wait.firstPtr, "queued");
	    Ns_MutexUnlock(&poolPtr->wqueue.lock);
        }
        Tcl_DStringResult(interp, dsPtr);
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
NsStartServer(NsServer *servPtr)
{
    ConnPool *poolPtr;
    int       n;

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
 * NsWakeupConnThreads --
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
NsWakeupConnThreads(ConnPool *poolPtr) {
    int i;
    //Ns_Log(Notice, "NsWakeupConnThreads: pool '%s' %p", poolPtr->pool, poolPtr);

    Ns_MutexLock(&poolPtr->tqueue.lock);
    for (i = 0; i < poolPtr->threads.max; i++) {
	ConnThreadArg *argPtr = &poolPtr->tqueue.args[i];
	//Ns_Log(Notice, "check conn thread %d state %d",i, argPtr->state);
	if (argPtr->state == connThread_idle) {
	    assert(argPtr->connPtr == NULL);
	    //Ns_Log(Notice, "wakeup conn thread %d",i);
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

    Ns_Log(Notice, "serv: stopping server: %s", servPtr->server);
    servPtr->pools.shutdown = 1;
    poolPtr = servPtr->pools.firstPtr;
    while (poolPtr != NULL) {
	NsWakeupConnThreads(poolPtr);
        poolPtr = poolPtr->nextPtr;
    }
}

void
NsWaitServer(NsServer *servPtr, Ns_Time *toPtr)
{
    ConnPool  *poolPtr;
    Ns_Thread  joinThread;
    int        status;

    // Ns_Log(Notice, "NsWaitServer server: %s", servPtr->server);

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
        Ns_Log(Warning, "serv: timeout waiting for connection thread exit");
    } else {
        if (joinThread != NULL) {
            JoinConnThread(&joinThread);
        }
        Ns_Log(Notice, "serv: connection threads stopped");
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
NsConnArgProc(Tcl_DString *dsPtr, void *arg)
{
    ConnThreadArg *argPtr = arg;

    if (arg != NULL) {
        ConnPool     *poolPtr = argPtr->poolPtr;
        NsServer     *servPtr = poolPtr->servPtr;
	
        Ns_MutexLock(&servPtr->pools.lock);
        AppendConn(dsPtr, argPtr->connPtr, "running");
        Ns_MutexUnlock(&servPtr->pools.lock);
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

// these macros just take care about the local pairing of lock/unlock calls
#define MutexLock(lockPtr) \
  assert(check_##lockPtr == 0); Ns_MutexLock(lockPtr); check_##lockPtr++
#define MutexUnlock(lockPtr) \
  assert(check_##lockPtr == 1); Ns_MutexUnlock(lockPtr); check_##lockPtr--


void
NsConnThread(void *arg)
{
    ConnThreadArg *argPtr = arg;
    ConnPool     *poolPtr = argPtr->poolPtr;
    NsServer     *servPtr = poolPtr->servPtr;
    Conn         *connPtr = NULL;
    Ns_Time       wait, *timePtr = &wait;
    unsigned int  id;
    int           status = NS_OK, cpt, ncons, timeout, current, fromQueue;
    char         *p, *path, *exitMsg;
    Ns_Mutex     *poolsLockPtr  = &servPtr->pools.lock;
    Ns_Mutex     *tqueueLockPtr = &poolPtr->tqueue.lock;
    Ns_Mutex     *wqueueLockPtr = &poolPtr->wqueue.lock;
    Ns_Thread     joinThread;

    /*
     * Set the conn thread name.
     */

    Ns_TlsSet(&argtls, argPtr);
    argPtr->tid = Ns_ThreadId();  // unused
    argPtr->state = connThread_warmup;

    //Ns_Log(Notice, "[%d] NsConnThread state %d", ThreadNr(poolPtr, argPtr), argPtr->state);

    Ns_MutexLock(poolsLockPtr);
    id = poolPtr->threads.nextid++;
    Ns_MutexUnlock(poolsLockPtr);

    p = (poolPtr->pool != NULL && *poolPtr->pool ? poolPtr->pool : 0);
    Ns_ThreadSetName("-conn:%s%s%s:%d", servPtr->server, p ? ":" : "", p ? p : "", id);

    /*
     * See how many connections this thread should run.  Setting
     * connsperthread to > 0 will cause the thread to graceously exit,
     * after processing that many requests, thus initiating kind-of
     * Tcl-level garbage collection. 
     */

    path   = Ns_ConfigGetPath(servPtr->server, NULL, NULL);
    cpt    = Ns_ConfigIntRange(path, "connsperthread", 0, 0, INT_MAX);
    
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
	interp = Ns_TclAllocateInterp(servPtr->server);
        Ns_GetTime(&end);
        Ns_DiffTime(&end, &start, &diff);
	Ns_Log(Notice, "thread initialized (%.3f ms)", 
	       ((double)diff.sec * 1000.0) + ((double)diff.usec / 1000.0));
	Ns_TclDeAllocateInterp(interp);
	argPtr->state = connThread_ready;
    }

    /*
     * Start handling connections.
     */

    Ns_MutexLock(poolsLockPtr);
    if (poolPtr->threads.creating > 0) {
	poolPtr->threads.creating--;
    }
    Ns_MutexUnlock(poolsLockPtr);


    while (1) {

	/*
	 * We are ready to process requests. Pick it either a request
	 * from the waiting queue, or go to a waiting state and add
	 * jourself to the conn thread queue.
	 */
	assert(argPtr->connPtr == NULL);
	assert(argPtr->state == connThread_ready);

	if (poolPtr->wqueue.wait.firstPtr) {
	    connPtr = NULL;	
	    Ns_MutexLock(wqueueLockPtr);
	    if (poolPtr->wqueue.wait.firstPtr) {
		/* 
		 * There are waiting requests.  Pull the first connection of
		 * the waiting list.
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
	    //Ns_Log(Notice, "**** take conn ptr from the queue %p", argPtr->connPtr);
	    fromQueue = 1;
	} else {
	    fromQueue = 0;
	}
      
	if (argPtr->connPtr == NULL) {
	    /*
	     * There is nothing urgent to do. We can add ourself to the
	     * conn thread queue.
	     */
	    Ns_MutexLock(poolsLockPtr);
	    poolPtr->threads.idle ++;
	    Ns_MutexUnlock(poolsLockPtr);

	    argPtr->state = connThread_idle;

	    Ns_MutexLock(tqueueLockPtr);
	    /*
	     * We put an entry into the thread queue. However, we must
	     * take care, that signals are not sent, before this thread
	     * is waiting for it. therefore. We lock the connection
	     * thread specific lock right here, also the signal sending
	     * code uses the same lock.
	     */
	    Ns_MutexLock(&argPtr->lock);

	    argPtr->nextPtr = poolPtr->tqueue.nextPtr; 
	    poolPtr->tqueue.nextPtr = argPtr;
	    Ns_MutexUnlock(tqueueLockPtr);
	    //Ns_Log(Notice, "[%d] enqueue thread idle %d", ThreadNr(poolPtr, argPtr), poolPtr->threads.idle);

	    /*
	     * Wait until someone wakes us up, or a timeout happens.
	     */
	    while (1) {

		if (servPtr->pools.shutdown) break;
		
		Ns_GetTime(timePtr);
		Ns_IncrTime(timePtr, timeout, 0);
		
		// Ns_Log(Notice, "[%d] wait thread state %d", 
		//                 ThreadNr(poolPtr, argPtr), argPtr->state);

		status = Ns_CondTimedWait(&argPtr->cond, &argPtr->lock, timePtr);
		
		//Ns_Log(Notice, "[%d] woke up thread state %d connPtr %p", 
		//                ThreadNr(poolPtr, argPtr), argPtr->state, argPtr->connPtr);

		if (status == NS_TIMEOUT) {
		    if (argPtr->connPtr) {
			/* this should not happen; probably a signal was lost */
			Ns_Log(Warning, "signal lost, resuming after timeout");
			status = NS_OK;
		    }
		    if (poolPtr->threads.current <= poolPtr->threads.min) continue;
		    //fprintf(stderr, "timeout can die\n");
		    break;
		}
		if (argPtr->connPtr) break;
		
		//Ns_Log(Notice, "CondTimedWait returned an unexpected result, maybe shutdown?");
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
		Ns_MutexUnlock(tqueueLockPtr);
	    }
	    
	    Ns_MutexLock(poolsLockPtr);
	    poolPtr->threads.idle --;
	    Ns_MutexUnlock(poolsLockPtr);
	    
	    argPtr->state = connThread_busy;
	    
	    if (servPtr->pools.shutdown) {
		exitMsg = "shutdown pending";
		break;
	    } else if (status == NS_TIMEOUT) {
		exitMsg = "idle thread terminates";
		break;
	    }
	}
	
	connPtr = argPtr->connPtr;
	assert(connPtr);

	{
	    Ns_Time now;
	    Ns_GetTime(&now);
	    connPtr->requestDequeueTime = now;
	}

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

#if 0
	// What is this for?
        if (connPtr->nextPtr == NULL) {
            /*
             * If this thread just free'd up the busy server,
             * run the ready procs to signal other subsystems.
             */
            NsRunAtReadyProcs();
        }
#endif
	argPtr->state = connThread_ready;
	if (cpt) {
	    int waiting, idle, lowwater;

	    --ncons;
	    
	    /*
	     * Get a consistent snapshot of the controlling variables.
	     */
            Ns_MutexLock(poolsLockPtr);
	    waiting  = poolPtr->wqueue.wait.num;
	    lowwater = poolPtr->wqueue.lowwatermark;
	    idle     = poolPtr->threads.idle;
	    current    = poolPtr->threads.current;
            Ns_MutexUnlock(poolsLockPtr);

	    {   // this is intended for development only. 
		Ns_Time now, acceptTime, queueTime, runTime, totalTime;
		Ns_DiffTime(&connPtr->requestQueueTime, &connPtr->acceptTime, &acceptTime);
		Ns_DiffTime(&connPtr->requestDequeueTime, &connPtr->requestQueueTime, &queueTime);
		Ns_GetTime(&now);
		Ns_DiffTime(&now, &connPtr->requestDequeueTime, &runTime);
		Ns_DiffTime(&now, &connPtr->acceptTime, &totalTime);

	    Ns_Log(Notice, "[%d] end of job, waiting %d current %d idle %d ncons %d fromQueue %d"
		   " accept %" PRIu64 ".%06ld"
		   " queue %" PRIu64 ".%06ld"
		   " run %" PRIu64 ".%06ld"
		   " total %" PRIu64 ".%06ld",
		   ThreadNr(poolPtr, argPtr),
		   waiting, poolPtr->threads.current, idle, ncons, fromQueue,
		   (int64_t) acceptTime.sec, acceptTime.usec,
		   (int64_t) queueTime.sec, queueTime.usec,
		   (int64_t) runTime.sec, runTime.usec,
		   (int64_t) totalTime.sec, totalTime.usec
		   );
	    }
	    
	    if (waiting > 0) {
		/* 
		 * There are waiting requests. Work on those unless we
		 * are expiring or we are already under the lowwater
		 * mark of connection threads.
		 */
		if (ncons > 0 || waiting > lowwater || current < 2) {
		    //Ns_Log(Notice, "*** work on waiting request (waiting %d)", waiting);
		    continue;
		}
		// Ns_Log(Notice, "??? don't work on waiting requests");
	    }
	    
	    if (ncons <= 0) {
	        Ns_Log(Notice, "thread is working overtime due to stress %d, waiting %d",
		       ncons, waiting);
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

    { int wakeup; 
	/*
	 * Record the fact that this driver is exiting by decrementing
	 * the actually running threads and wakeup the driver to check
	 * against thread starvation starvation (due to an insufficient
	 * number of connection threads).
	 */
	Ns_MutexLock(poolsLockPtr);
	poolPtr->threads.current--;
	wakeup = (poolPtr->threads.current < poolPtr->threads.min);
	Ns_MutexUnlock(poolsLockPtr);
	
	/* 
	 * During shutdown, we do not want to restart connection
	 * threads. The driver pointer might be already invalid. 
	 */
	if (wakeup && !servPtr->pools.shutdown) { 
	    NsWakeupDriver(connPtr->drvPtr); 
	} 
    }
    
    /*
     * During shutdown, the main thread waits for signals on the
     * condition variable to check whether all threads have terminated
     * already.
     */
    if (servPtr->pools.shutdown) {
	Ns_CondSignal(&poolPtr->wqueue.cond); 
    }

    joinThread = servPtr->pools.joinThread;
    Ns_ThreadSelf(&servPtr->pools.joinThread);
    if (joinThread != NULL) {
        JoinConnThread(&joinThread);
    }

    Ns_Log(Notice, "exiting: %s", exitMsg);
    argPtr->state = connThread_free; 
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
ConnRun(ConnThreadArg *argPtr, Conn *connPtr)
{
    Ns_Conn  *conn = (Ns_Conn *) connPtr;
    NsServer *servPtr = connPtr->servPtr;
    int       status = NS_OK;
    char     *auth;

    /*
     * Re-initialize and run the connection. 
     */
    if (connPtr->sockPtr) {
	connPtr->reqPtr = NsGetRequest(connPtr->sockPtr);
    } else {
	connPtr->reqPtr = NULL;
    }
    
    if (connPtr->reqPtr == NULL) {
        Ns_ConnClose(conn);
        return;
    }

    /*
     * Make sure we update peer address with actual remote IP address
     */

    connPtr->reqPtr->port = ntohs(connPtr->sockPtr->sa.sin_port);
    strcpy(connPtr->reqPtr->peer, ns_inet_ntoa(connPtr->sockPtr->sa.sin_addr));

    connPtr->request = &connPtr->reqPtr->request;

    /*{ConnPool *poolPtr = argPtr->poolPtr;
    Ns_Log(Notice,"ConnRun [%d] connPtr %p req %p %s", ThreadNr(poolPtr, argPtr), connPtr, connPtr->request, connPtr->request->line);
    } */   
    connPtr->headers = connPtr->reqPtr->headers;
    connPtr->contentLength = connPtr->reqPtr->length;

    connPtr->nContentSent = 0;
    connPtr->responseStatus = 200;
    connPtr->responseLength = -1;  /* -1 == unknown (stream), 0 == zero bytes. */
    connPtr->recursionCount = 0;
    connPtr->auth = NULL;

    connPtr->keep = -1;                   /* Default keep-alive rules apply */

    Ns_ConnSetCompression(conn, servPtr->compress.enable ? servPtr->compress.level : 0);
    connPtr->compress = -1;

    connPtr->outputEncoding = servPtr->encoding.outputEncoding;
    connPtr->urlEncoding = servPtr->encoding.urlEncoding;

    Tcl_InitHashTable(&connPtr->files, TCL_STRING_KEYS);
    snprintf(connPtr->idstr, sizeof(connPtr->idstr), "cns%d", connPtr->id);
    connPtr->outputheaders = Ns_SetCreate(NULL);
    if (connPtr->request->version < 1.0) {
        conn->flags |= NS_CONN_SKIPHDRS;
    }
    if (servPtr->opts.hdrcase != Preserve) {
        int i;

        for (i = 0; i < Ns_SetSize(connPtr->headers); ++i) {
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
        status = (*connPtr->sockPtr->drvPtr->requestProc)(connPtr->sockPtr->drvPtr->arg, conn);
    }

    /*
     * Run the rest of the request.
     */

    if (connPtr->request->protocol != NULL && connPtr->request->host != NULL) {
        status = NsConnRunProxyRequest((Ns_Conn *) connPtr);
    } else {
        if (status == NS_OK) {
            status = NsRunFilters(conn, NS_FILTER_PRE_AUTH);
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
                if (status == NS_OK) {
                    status = Ns_ConnRunRequest(conn);
                }
                break;

            case NS_FORBIDDEN:
                Ns_ConnReturnForbidden(conn);
                break;

            case NS_UNAUTHORIZED:
                Ns_ConnReturnUnauthorized(conn);
                break;

            case NS_ERROR:
            default:
                Ns_ConnReturnInternalError(conn);
                break;
            }
        } else if (status != NS_FILTER_RETURN) {
            /*
             * If not ok or filter_return, then the pre-auth filter coughed
             * an error.  We are not going to proceed, but also we
             * can't count on the filter to have sent a response
             * back to the client.  So, send an error response.
             */
            Ns_ConnReturnInternalError(conn);
            status = NS_FILTER_RETURN; /* to allow tracing to happen */
        }
    }

    Ns_ConnClose(conn);
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

    Ns_ConnClearQuery(conn);
    Ns_SetFree(connPtr->auth);
    connPtr->auth = NULL;
    Ns_SetFree(connPtr->outputheaders);
    connPtr->outputheaders = NULL;
    NsFreeRequest(connPtr->reqPtr);
    connPtr->reqPtr = NULL;
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
    
    /*
     * Get first free connection thread slot; selecting a slot and
     * occupying it has to be done under a mutex lock, since we do not
     * want someone else to pick the same. We are competing
     * potentially against driver/spooler threads and the main thread.
     */
    { char *threadName = Ns_ThreadGetName();
      //fprintf(stderr, "NAME <%s>\n", threadName);
      assert(strncmp("-driver:", threadName, 8) == 0 
	     || strncmp("-main-", threadName, 6) == 0
	     || strncmp("-spooler", threadName, 8) == 0
	     );
    }

    /*
     * TODO: we could do better than the linear search, but the queue
     * is short...
     */
    Ns_MutexLock(&poolPtr->tqueue.lock);
    for (i = 0; i < poolPtr->threads.max; i++) {
      if (poolPtr->tqueue.args[i].state == connThread_free) {
	argPtr = &(poolPtr->tqueue.args[i]);
	break;
      }
    }
    argPtr->state = connThread_initial;
    poolPtr->servPtr->stats.connthreads++;
    Ns_MutexUnlock(&poolPtr->tqueue.lock);

    //Ns_Log(Notice, "CreateConnThread use thread slot [%d]", i);

    argPtr->poolPtr = poolPtr;
    argPtr->connPtr = NULL;
    argPtr->nextPtr = NULL;
    argPtr->cond = NULL;
    Ns_ThreadCreate(NsConnThread, argPtr, 0, &thread);
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
    void *argArg;

    Ns_ThreadJoin(threadPtr, &argArg);
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
AppendConn(Tcl_DString *dsPtr, Conn *connPtr, char *state)
{
    Ns_Time now, diff;

    Tcl_DStringStartSublist(dsPtr);

    /*
     * An annoying race condition can be lethal here.
     */
    if (connPtr != NULL) {
	char  buf[100];

        Tcl_DStringAppendElement(dsPtr, connPtr->idstr);
	if (connPtr->reqPtr != NULL) {
	    Tcl_DStringAppendElement(dsPtr, Ns_ConnPeer((Ns_Conn *) connPtr));
	} else {
	    /* Actually, this should not happen, but it does, maybe due
	     * to the above mentioned race condition; we notice in the
	     * errlog the fact and return a placeholder value
	     */
	    Ns_Log(Notice, "AppendConn: no reqPtr in state %s; ignore conn in output", state);
	    Tcl_DStringAppendElement(dsPtr, "unknown");
	}
        Tcl_DStringAppendElement(dsPtr, state);

        /*
         * Carefully copy the bytes to avoid chasing a pointer
         * which may be changing in the connection thread.  This
         * is not entirely safe but acceptible for a seldom-used
         * admin command.
         */
        if (connPtr->request) {
	    char *p;
	    p = connPtr->request->method ? connPtr->request->method : "?";
	    Tcl_DStringAppendElement(dsPtr, strncpy(buf, p, sizeof(buf)));
	    p = connPtr->request->url ? connPtr->request->url : "?";
	    Tcl_DStringAppendElement(dsPtr, strncpy(buf, p, sizeof(buf)));
	} else {
	    Ns_Log(Notice, "AppendConn: no request in state %s; ignore conn in output", state);
	    Tcl_DStringAppendElement(dsPtr, "unknown");
	    Tcl_DStringAppendElement(dsPtr, "unknown");
	}
	Ns_GetTime(&now);
        Ns_DiffTime(&now, &connPtr->acceptTime, &diff);
        snprintf(buf, sizeof(buf), "%" PRIu64 ".%ld", (int64_t) diff.sec, diff.usec);
        Tcl_DStringAppendElement(dsPtr, buf);
        snprintf(buf, sizeof(buf), "%" TCL_LL_MODIFIER "d", connPtr->nContentSent);
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
AppendConnList(Tcl_DString *dsPtr, Conn *firstPtr, char *state)
{
    while (firstPtr != NULL) {
        AppendConn(dsPtr, firstPtr, state);
        firstPtr = firstPtr->nextPtr;
    }
}
