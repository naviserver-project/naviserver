/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
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

static void ConnRun(Conn *connPtr)
    NS_GNUC_NONNULL(1);

static void CreateConnThread(ConnPool *poolPtr)
    NS_GNUC_NONNULL(1);

static void AppendConn(Tcl_DString *dsPtr, const Conn *connPtr, const char *state, bool checkforproxy)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static void AppendConnList(Tcl_DString *dsPtr, const Conn *firstPtr, const char *state, bool checkforproxy)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static bool neededAdditionalConnectionThreads(const ConnPool *poolPtr)
    NS_GNUC_NONNULL(1);

static void WakeupConnThreads(ConnPool *poolPtr)
    NS_GNUC_NONNULL(1);

static Ns_ReturnCode MapspecParse(Tcl_Interp *interp, Tcl_Obj *mapspecObj, char **method, char **url,
                                  NsUrlSpaceContextSpec **specPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static int ServerMaxThreadsObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                                  ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static int ServerMinThreadsObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                                  ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);


static int ServerConnectionRateLimitObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                                           ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);


static int ServerPoolRateLimitObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                                     ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);


static int ServerMapObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                           NsServer  *servPtr, ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

static int ServerMappedObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                              NsServer *servPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static int ServerUnmapObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                             NsServer *servPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static void ConnThreadSetName(const char *server, const char *pool, uintptr_t threadId, uintptr_t connId)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int ServerListActiveCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                               ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
static int ServerListAllCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                            ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
static int ServerListQueuedCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                               ConnPool *poolPtr, TCL_SIZE_T nargs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static void ServerListActive(Tcl_DString *dsPtr, ConnPool *poolPtr, bool checkforproxy)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void ServerListQueued(Tcl_DString *dsPtr, ConnPool *poolPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int SetPoolAttribute(Tcl_Interp *interp, TCL_SIZE_T nargs, ConnPool *poolPtr, int *valuePtr, int value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static Ns_ArgProc WalkCallback;

/*
 * Static variables defined in this file.
 */

static Ns_Tls argtls = NULL;
static int    poolid = 0;

/*
 * Debugging stuff
 */
#define ThreadNr(poolPtr, argPtr) (int)(((argPtr) - (poolPtr)->tqueue.args))

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
    const ConnThreadArg *argPtr;

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
NsMapPool(ConnPool *poolPtr, const char *mapString, unsigned int flags)
{
    const char            *server;
    char                  *method, *url;
    Tcl_Obj               *mapspecObj;
    NsUrlSpaceContextSpec *specPtr;

    NS_NONNULL_ASSERT(poolPtr != NULL);
    NS_NONNULL_ASSERT(mapString != NULL);

    mapspecObj = Tcl_NewStringObj(mapString, TCL_INDEX_NONE);
    server = poolPtr->servPtr->server;

    Tcl_IncrRefCount(mapspecObj);
    if (MapspecParse(NULL, mapspecObj, &method, &url, &specPtr) == NS_OK) {
        Ns_UrlSpecificSet2(server, method, url, poolid, poolPtr, flags, NULL, specPtr);

    } else {
        Ns_Log(Warning,
               "invalid mapspec '%s'; must be 2- or 3-element list "
               "containing HTTP method, URL, and optionally a filtercontext",
               mapString);
    }
    Tcl_DecrRefCount(mapspecObj);
}

/*
 *----------------------------------------------------------------------
 *
 * NsPoolName --
 *
 *      Return a printable pool name. In essence, it translates the empty pool
 *      name (the default pool) to the string "defaullr" for printing
 *      purposes.
 *
 * Results:
 *      Printable string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
NsPoolName(const char *poolName)
{
    const char *result;

    NS_NONNULL_ASSERT(poolName != NULL);

    if (*poolName == '\0') {
        result = "default";
    } else {
        result = poolName;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsPoolAllocateThreadSlot --
 *
 *      Allocate a thread slot for this pool. When bandwidth management is
 *      activated for a pool, one has to aggregate the pool data from multiple
 *      writer threads. This happens via slots allocated to the
 *      threads. Currently, writer threads only exit when the server goes
 *      down, so there is no need to reuse slots from writer thread, and the
 *      associated slots are stable once allocated. This function will be
 *      called only once per writer thread and pool. The thread ID will become
 *      necessary, when the writer threads are dynamic.
 *
 * Results:
 *      Allocated slot id for this thread.
 *
 * Side effects:
 *      Maybe the DList allocates additional memory.
 *
 *----------------------------------------------------------------------
 */
size_t
NsPoolAllocateThreadSlot(ConnPool *poolPtr, uintptr_t UNUSED(threadID))
{
    Ns_DList *dlPtr;

    dlPtr = &(poolPtr->rate.writerRates);

    /*
     * Appending must be locked, since in rare cases, a realloc might happen
     * under the hood when appending
     */
    Ns_MutexLock(&poolPtr->rate.lock);
    Ns_DListAppend(dlPtr, 0u);
    Ns_MutexUnlock(&poolPtr->rate.lock);

    return (dlPtr->size - 1u);
}

/*
 *----------------------------------------------------------------------
 *
 * NsPoolTotalRate --
 *
 *      Calculate the total rate form all writer threads. The function simply
 *      adds the data from all allocated slots and reports the number of
 *      associated writer threads for estimating rates per writer threads.
 *
 * Results:
 *      Actual total rate for a pool (sum of rates per writer thread).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsPoolTotalRate(ConnPool *poolPtr, size_t slot, int rate, int *writerThreadCount)
{
    Ns_DList *dlPtr;
    size_t    i;
    uintptr_t totalRate = 0u;

    assert(rate >= 0);

    dlPtr = &(poolPtr->rate.writerRates);
    dlPtr->data[slot] = UINT2PTR(rate);

    Ns_MutexLock(&poolPtr->rate.lock);
    for (i = 0u; i < dlPtr->size; i ++) {
        totalRate = totalRate + (uintptr_t)dlPtr->data[i];
    }
    poolPtr->rate.currentRate = (int)totalRate;
    Ns_MutexUnlock(&poolPtr->rate.lock);

    *writerThreadCount = (int)dlPtr->size;

    return (int)totalRate;
}

void
NsPoolAddBytesSent(ConnPool *poolPtr, Tcl_WideInt bytesSent)
{
    Ns_MutexLock(&poolPtr->rate.lock);
    poolPtr->rate.bytesSent += bytesSent;
    Ns_MutexUnlock(&poolPtr->rate.lock);
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
static bool
neededAdditionalConnectionThreads(const ConnPool *poolPtr) {
    bool wantCreate;

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
        wantCreate = (!poolPtr->servPtr->pools.shutdown);
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
        wantCreate = NS_FALSE;

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
    bool create;
    int  waitnum;

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

    if (create) {
        poolPtr->threads.current ++;
        poolPtr->threads.creating ++;
    }
    waitnum = poolPtr->wqueue.wait.num;

    Ns_MutexUnlock(&poolPtr->threads.lock);
    Ns_MutexUnlock(&poolPtr->wqueue.lock);

    if (create) {
        Ns_Log(Notice, "NsEnsureRunningConnectionThreads wantCreate %d waiting %d idle %d current %d",
               (int)create,
               waitnum,
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
 *      Append a connection to the run queue of a connection pool when
 *      possible (e.g. no shutdown, a free connection thread is available,
 *      ...)
 *
 * Results:
 *      NS_OK (queued), NS_ERROR (return error), NS_TIMEOUT (try again)
 *
 * Side effects:
 *      Connection will run shortly.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsQueueConn(Sock *sockPtr, const Ns_Time *nowPtr)
{
    ConnThreadArg *argPtr = NULL;
    NsServer      *servPtr;
    ConnPool      *poolPtr = NULL;
    Conn          *connPtr = NULL;
    bool           create = NS_FALSE;
    int            queued = NS_OK;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(nowPtr != NULL);
    assert(sockPtr->drvPtr != NULL);

    sockPtr->drvPtr->stats.received++;
    servPtr = sockPtr->servPtr;

    /*
     * Perform no queuing on shutdown.
     */
    if (unlikely(servPtr->pools.shutdown)) {
        return NS_ERROR;
    }

    /*
     * Select connection pool. For non-HTTP drivers, the request.method
     * won't be provided.
     */
    if ((sockPtr->poolPtr == NULL)
        && (sockPtr->reqPtr != NULL)
        && (sockPtr->reqPtr->request.method != NULL)) {
        NsUrlSpaceContext ctx;

        NsUrlSpaceContextInit(&ctx, sockPtr, sockPtr->reqPtr->headers);
        poolPtr = NsUrlSpecificGet((Ns_Server*)servPtr,
                                   sockPtr->reqPtr->request.method,
                                   sockPtr->reqPtr->request.url,
                                   poolid, 0u, NS_URLSPACE_DEFAULT,
                                   NULL,
                                   NsUrlSpaceContextFilterEval, &ctx);
        sockPtr->poolPtr = poolPtr;

    } else if (sockPtr->poolPtr != NULL) {
        poolPtr = sockPtr->poolPtr;
        Ns_Log(Notice , "=== NsQueueConn URL <%s> was already assigned to pool <%s>",
               sockPtr->reqPtr->request.url, poolPtr->pool);
    }
    if (poolPtr == NULL) {
        poolPtr = servPtr->pools.defaultPtr;
    }

   /*
    * We know the pool. Try to add connection into the queue of this pool
    * (either into a free slot or into its waiting list, or, when everything
    * fails signal an error or timeout (for retry attempts) to the caller.
    */
    Ns_MutexLock(&poolPtr->wqueue.lock);
    if (poolPtr->wqueue.freePtr != NULL) {
        connPtr = poolPtr->wqueue.freePtr;
        poolPtr->wqueue.freePtr = connPtr->nextPtr;
        connPtr->nextPtr = NULL;
    }
    Ns_MutexUnlock(&poolPtr->wqueue.lock);

    if (likely(connPtr != NULL)) {
        /*
         * We have got a free connPtr from the pool. Initialize the
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
        /*
         * sockPtr->location is always a mallocated string provided by the
         * driver, no need to strncopy it here.
         */
        connPtr->location             = sockPtr->location;
        connPtr->flags                = sockPtr->flags;
        if ((sockPtr->drvPtr->opts & NS_DRIVER_ASYNC) == 0u) {
            connPtr->acceptTime       = *nowPtr;
        } else {
            connPtr->acceptTime       = sockPtr->acceptTime;
        }
        connPtr->rateLimit            = poolPtr->rate.defaultConnectionLimit;

        /*
         * Reset members of sockPtr, which have been passed to connPtr.
         */
        sockPtr->acceptTime.sec       = 0;
        sockPtr->flags                = 0u;
        sockPtr->location             = NULL;

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
             * such that no one else might grab it, and fill in the
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

    if (unlikely(connPtr == NULL)) {
        /*
         * The connection thread pool queue is full.  We can either keep the
         * sockPtr in a waiting state, or we can reject the queue overrun with
         * a 503 - depending on the configuration.
         */
        queued = NS_TIMEOUT;
        create = NS_FALSE;

        if ((sockPtr->flags & NS_CONN_SOCK_WAITING) == 0u) {
            /*
             * The flag NS_CONN_SOCK_WAITING is just used to avoid reporting
             * the same request multiple times as unsuccessful queueing
             * attempts (when rejectoverrun is false).
             */
            sockPtr->flags |= NS_CONN_SOCK_WAITING;
            Ns_Log(Notice, "[%s pool %s] All available connections are used, waiting %d idle %d current %d",
                   poolPtr->servPtr->server,
                   poolPtr->pool,
                   poolPtr->wqueue.wait.num,
                   poolPtr->threads.idle,
                   poolPtr->threads.current);

            if (poolPtr->wqueue.rejectoverrun) {
                Ns_MutexLock(&poolPtr->threads.lock);
                poolPtr->stats.dropped++;
                Ns_MutexUnlock(&poolPtr->threads.lock);
                queued = NS_ERROR;
            }
        }

    } else if (argPtr != NULL) {
        /*
         * We have a connection thread ready.
         *
         * Perform lock just in the debugging case to avoid race condition.
         */
        if (Ns_LogSeverityEnabled(Debug)) {
            int idle;

            Ns_MutexLock(&poolPtr->threads.lock);
            idle = poolPtr->threads.idle;
            Ns_MutexUnlock(&poolPtr->threads.lock);

            Ns_Log(Debug, "[%d] dequeue thread connPtr %p idle %d state %d create %d",
                   ThreadNr(poolPtr, argPtr), (void *)connPtr, idle, argPtr->state, (int)create);
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
                   (void *)connPtr, poolPtr->wqueue.wait.num, (int)create);
        }
    }

    if (create) {
        int idle, current;

        Ns_MutexLock(&poolPtr->threads.lock);
        idle = poolPtr->threads.idle;
        current = poolPtr->threads.current;
        poolPtr->threads.current ++;
        poolPtr->threads.creating ++;
        Ns_MutexUnlock(&poolPtr->threads.lock);

        Ns_Log(Notice, "NsQueueConn wantCreate %d waiting %d idle %d current %d",
               (int)create,
               poolPtr->wqueue.wait.num,
               idle,
               current);

        CreateConnThread(poolPtr);
    }

    return queued;
}

/*
 *----------------------------------------------------------------------
 *
 * WalkCallback --
 *
 *    Callback for Ns_UrlSpecificWalk() used in "ns_server map".  Currently a
 *    placeholder, might output useful information in the future.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */
static void
WalkCallback(Tcl_DString *dsPtr, const void *arg)
{
    const ConnPool *poolPtr = (ConnPool *)arg;
    Tcl_DStringAppendElement(dsPtr, poolPtr->pool);
}



/*
 *----------------------------------------------------------------------
 *
 * SetPoolAttribute --
 *
 *    Helper function to factor out common code when modifying integer
 *    attributes in the pools structure.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Sets interp result.
 *
 *----------------------------------------------------------------------
 */
static int
SetPoolAttribute(Tcl_Interp *interp, TCL_SIZE_T nargs, ConnPool *poolPtr, int *valuePtr, int value) {

    if (nargs == 1) {
        Ns_MutexLock(&poolPtr->threads.lock);
        *valuePtr = value;
        Ns_MutexUnlock(&poolPtr->threads.lock);
    } else {
        /*
         * Called without an argument, just return the current setting.
         */
        assert(nargs == 0);
    }

    Tcl_SetObjResult(interp, Tcl_NewIntObj(*valuePtr));
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerMaxThreadsObjCmd, subcommand of NsTclServerObjCmd --
 *
 *    Implements "ns_server ... maxthreads ...".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Might update maxthreads setting of a pool
 *
 *----------------------------------------------------------------------
 */

static int
ServerMaxThreadsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                       ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {poolPtr->threads.min, poolPtr->wqueue.maxconns};
    Ns_ObjvSpec       args[] = {
        {"?value",   Ns_ObjvInt, &value, &range},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(NULL, args, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        result = SetPoolAttribute(interp, nargs, poolPtr, &poolPtr->threads.max, value);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerMinThreadsObjCmd, subcommand of NsTclServerObjCmd --
 *
 *    Implements "ns_server ... minthreads ...".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Might update minthreads setting of a pool
 *
 *----------------------------------------------------------------------
 */
static int
ServerMinThreadsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                       ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {1, poolPtr->threads.max};
    Ns_ObjvSpec       args[] = {
        {"?value", Ns_ObjvInt, &value, &range},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(NULL, args, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        result = SetPoolAttribute(interp, nargs, poolPtr, &poolPtr->threads.min, value);
    }
    return result;
}

static int
ServerPoolRateLimitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                       ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {-1, INT_MAX};
    Ns_ObjvSpec       args[] = {
        {"?value", Ns_ObjvInt, &value, &range},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(NULL, args, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        result = SetPoolAttribute(interp, nargs, poolPtr, &poolPtr->rate.poolLimit, value);
    }
    return result;
}
static int
ServerConnectionRateLimitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                                ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {-1, INT_MAX};
    Ns_ObjvSpec       args[] = {
        {"?value", Ns_ObjvInt, &value, &range},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(NULL, args, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        result = SetPoolAttribute(interp, nargs, poolPtr, &poolPtr->rate.defaultConnectionLimit, value);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * MapspecParse --
 *
 *      Check, if the mapspec Tcl_Obj in the first argument is of the right
 *      syntax and return its components as strings. Note that the lifetime of
 *      the returned strings depends on the lifetime of the first argument.
 *
 * Results:
 *      Ns_ReturnCode NS_OK or NS_ERROR;
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
MapspecParse(Tcl_Interp *interp, Tcl_Obj *mapspecObj, char **method, char **url,
             NsUrlSpaceContextSpec **specPtr) {
    Ns_ReturnCode status = NS_ERROR;
    TCL_SIZE_T    oc;
    Tcl_Obj     **ov;

    NS_NONNULL_ASSERT(mapspecObj != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(specPtr != NULL);

    if (Tcl_ListObjGetElements(NULL, mapspecObj, &oc, &ov) == TCL_OK) {
        if (oc == 2 || oc == 3) {
            const char *errorMsg;

            if (!Ns_PlainUrlPath(Tcl_GetString(ov[1]), &errorMsg)) {
                status = NS_ERROR;
            } else {
                status = NS_OK;
                *method = Tcl_GetString(ov[0]);
                *url = Tcl_GetString(ov[1]);
                if (oc == 3) {
                    *specPtr = NsObjToUrlSpaceContextSpec(interp, ov[2]);
                    if (*specPtr == NULL) {
                        status = NS_ERROR;
                    }
                } else {
                    *specPtr = NULL;
                }
            }
        }
    }
    if (unlikely(status == NS_ERROR) && interp != NULL) {
        Ns_TclPrintfResult(interp,
                           "invalid mapspec '%s'; must be 2- or 3-element list "
                           "containing HTTP method, plain URL path, and optionally a filtercontext",
                           Tcl_GetString(mapspecObj));
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerMapObjCmd, subcommand of NsTclServerObjCmd --
 *
 *    Implements "ns_server ... map ...".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    Map method + URL to specified pool
 *
 *----------------------------------------------------------------------
 */
static int
ServerMapObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                NsServer  *servPtr, ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int             result = TCL_OK, noinherit = 0;
    Tcl_Obj        *mapspecObj = NULL;
    Ns_ObjvSpec     lopts[] = {
        {"-noinherit", Ns_ObjvBool,   &noinherit, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?mapspec",   Ns_ObjvObj, &mapspecObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(lopts, args, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (mapspecObj != NULL) {
        char *method, *url;
        NsUrlSpaceContextSpec *specPtr = NULL;

        if (MapspecParse(interp, mapspecObj, &method, &url, &specPtr) != NS_OK) {
            result = TCL_ERROR;
        } else {
            unsigned int flags = 0u;
            Tcl_DString ds;

            if (noinherit != 0) {
                flags |= NS_OP_NOINHERIT;
            }

            Ns_MutexLock(&servPtr->urlspace.lock);
            Ns_UrlSpecificSet2(servPtr->server, method, url, poolid, poolPtr, flags, NULL, specPtr);
            Ns_MutexUnlock(&servPtr->urlspace.lock);

            Tcl_DStringInit(&ds);
            Ns_Log(Notice, "pool[%s]: mapped %s %s%s -> %s",
                   servPtr->server, method, url,
                   (specPtr == NULL ? "" : NsUrlSpaceContextSpecAppend(&ds, specPtr)),
                   poolPtr->pool);
            Tcl_DStringFree(&ds);
        }

    } else {
        Tcl_DString  ds, *dsPtr = &ds;
        Tcl_Obj    **ov, *fullListObj;
        TCL_SIZE_T   oc;

        /*
         * Return the current mappings just in the case, when the map
         * operation was called without the optional argument.
         */
        Tcl_DStringInit(dsPtr);

        Ns_MutexLock(&servPtr->urlspace.lock);
        Ns_UrlSpecificWalk(poolid, servPtr->server, WalkCallback, dsPtr);
        Ns_MutexUnlock(&servPtr->urlspace.lock);

        /*
         * Convert the Tcl_Dstring into a list, and filter the elements
         * from different pools.
         */
        fullListObj = Tcl_NewStringObj(dsPtr->string, dsPtr->length);
        Tcl_IncrRefCount(fullListObj);

        result = Tcl_ListObjGetElements(interp, fullListObj, &oc, &ov);
        if (result == TCL_OK) {
            Tcl_Obj *resultObj;
            TCL_SIZE_T i;

            /*
             * The result should be always a proper list, so the potential
             * error should not occur.
             */
            resultObj = Tcl_NewListObj(0, NULL);

            for (i = 0; i < oc; i++) {
                Tcl_Obj   *elemObj = ov[i];
                TCL_SIZE_T length;

                /*
                 * Get the last element, which is the pool, and compare it
                 * with the current pool name.
                 */
                result = Tcl_ListObjLength(interp, elemObj, &length);
                if (result == TCL_OK) {
                    Tcl_Obj *lastSubElem;

                    result = Tcl_ListObjIndex(interp, elemObj, length-1, &lastSubElem);
                    if (result == TCL_OK) {
                        const char *pool = Tcl_GetString(lastSubElem);

                        if (!STREQ(poolPtr->pool, pool)) {
                            continue;
                        }
                    }
                }
                /*
                 * The element is from the current pool. Remove the last
                 * element (poolname) from the list...
                 */
                if (result == TCL_OK) {
                    result = Tcl_ListObjReplace(interp, elemObj, length-1, 1, 0, NULL);
                }
                /*
                 * ... and append the element.
                 */
                if (result == TCL_OK) {
                    Tcl_ListObjAppendElement(interp, resultObj, elemObj);
                } else {
                    break;
                }
            }
            if (result == TCL_OK) {
                Tcl_SetObjResult(interp, resultObj);
            } else {
                Ns_TclPrintfResult(interp, "invalid result from mapped URLs");
            }
        }
        Tcl_DecrRefCount(fullListObj);
        Tcl_DStringFree(dsPtr);

    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerMappedObjCmd, subcommand of NsTclServerObjCmd --
 *
 *    Implements "ns_server ... mapped ".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */
static int
ServerMappedObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                  NsServer *servPtr, TCL_SIZE_T nargs)
{
    int          result = TCL_OK, noinherit = 0, exact = 0, all = 0;
    Tcl_Obj     *mapspecObj = NULL;
    char        *method, *url;
    NsUrlSpaceContextSpec *specPtr;
    Ns_ObjvSpec  lopts[] = {
        {"-all",       Ns_ObjvBool,   &all,       INT2PTR(NS_TRUE)},
        {"-exact",     Ns_ObjvBool,   &exact,     INT2PTR(NS_TRUE)},
        {"-noinherit", Ns_ObjvBool,   &noinherit, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"mapspec", Ns_ObjvObj, &mapspecObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);

    if (Ns_ParseObjv(lopts, args, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (MapspecParse(interp, mapspecObj, &method, &url, &specPtr) != NS_OK) {
        result = TCL_ERROR;
    } else {
        unsigned int    flags = 0u;
        const ConnPool *mappedPoolPtr;
        Ns_UrlSpaceOp   op;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }

        if (exact == (int)NS_TRUE) {
            op = NS_URLSPACE_EXACT;
        } else {
            op = NS_URLSPACE_DEFAULT;
        }

        Ns_MutexLock(&servPtr->urlspace.lock);
        mappedPoolPtr = (ConnPool *)NsUrlSpecificGet((Ns_Server*)servPtr,
                                                     method, url, poolid, flags, op,
                                                     NULL, NULL, NULL);
        Ns_MutexUnlock(&servPtr->urlspace.lock);
        if (mappedPoolPtr == NULL) {
            mappedPoolPtr = servPtr->pools.defaultPtr;
        }

        if (all) {
            Tcl_Obj     *dictObj = Tcl_NewDictObj();
            Ns_OpProc   *procPtr;
            Ns_Callback *deletePtr;
            void        *argPtr;
            unsigned int requestFlags;
            Tcl_DString  ds;

            Tcl_DStringInit(&ds);

            Tcl_DictObjPut(interp, dictObj, Tcl_NewStringObj("pool", 4),
                           Tcl_NewStringObj(mappedPoolPtr->pool, TCL_INDEX_NONE));

            NsGetRequest2(servPtr, method, url, flags, op, NULL, NULL,
                          &procPtr, &deletePtr, &argPtr, &requestFlags);
            Ns_GetProcInfo(&ds, (ns_funcptr_t)procPtr, argPtr);

            Tcl_DictObjPut(interp, dictObj, Tcl_NewStringObj("handler", 7),
                           Tcl_NewStringObj(ds.string, ds.length));
            Tcl_SetObjResult(interp, dictObj);
            Tcl_DStringFree(&ds);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(mappedPoolPtr->pool, TCL_INDEX_NONE));
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerUnmapObjCmd, subcommand of NsTclServerObjCmd --
 *
 *    Implements "ns_server ... unmap ...".
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    might unmap a method/url pair.
 *
 *----------------------------------------------------------------------
 */
static int
ServerUnmapObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                  NsServer *servPtr, TCL_SIZE_T nargs)
{
    int          result = TCL_OK, noinherit = 0;
    char        *method, *url;
    Tcl_Obj     *mapspecObj = NULL;
    NsUrlSpaceContextSpec *specPtr;
    Ns_ObjvSpec  lopts[] = {
        {"-noinherit", Ns_ObjvBool, &noinherit, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"mapspec",   Ns_ObjvObj, &mapspecObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);

    if (Ns_ParseObjv(lopts, args, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (MapspecParse(interp, mapspecObj, &method, &url, &specPtr) != NS_OK) {
        result = TCL_ERROR;
    } else {
        bool          success;
        unsigned int  flags = 0u;
        const void   *data;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }
        // TODO: for the time being
        flags |= NS_OP_ALLCONSTRAINTS;

        Ns_MutexLock(&servPtr->urlspace.lock);
        data = Ns_UrlSpecificDestroy(servPtr->server,  method, url, poolid, flags);
        Ns_MutexUnlock(&servPtr->urlspace.lock);

        success = (data != NULL);
        // TODO: data is no good indicator when (all) context constraints are deleted.
        //if (success) {
        //    Ns_Log(Notice, "pool[%s]: unmapped %s %s", servPtr->server, method, url);
        //} else {
        //    Ns_Log(Warning, "pool[%s]: could not unmap %s %s", servPtr->server, method, url);
        //}
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(success));
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerListActive, ServerListQueued --
 *
 *    Backend for the "ns_server ... active ..." and
 *    the "ns_server ... queued ..." commands.
 *
 * Results:
 *    void
 *
 * Side effects:
 *    Appends list data about active/queued connections to the Tcl_DString
 *    provided in the first argument.
 *
 *----------------------------------------------------------------------
 */
static void
ServerListActive(Tcl_DString *dsPtr, ConnPool *poolPtr, bool checkforproxy)
{
    int i;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    Ns_MutexLock(&poolPtr->tqueue.lock);
    for (i = 0; i < poolPtr->threads.max; i++) {
        const ConnThreadArg *argPtr = &poolPtr->tqueue.args[i];

        if (argPtr->connPtr != NULL) {
            AppendConnList(dsPtr, argPtr->connPtr, "running", checkforproxy);
        }
    }
    Ns_MutexUnlock(&poolPtr->tqueue.lock);
}

static void
ServerListQueued(Tcl_DString *dsPtr, ConnPool *poolPtr)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    Ns_MutexLock(&poolPtr->wqueue.lock);
    AppendConnList(dsPtr, poolPtr->wqueue.wait.firstPtr, "queued", NS_FALSE);
    Ns_MutexUnlock(&poolPtr->wqueue.lock);
}


/*
 *----------------------------------------------------------------------
 *
 * ServerListActiveCmd, ServerListAllCmd, ServerListQueuedCmd --
 *
 *    Stubs for the "ns_server ... active ...", "ns_server ... all ..."
 *    and the "ns_server ... queued ..." commands.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

static int
ServerListActiveCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                 ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int         result = TCL_OK, checkforproxy = (int)NS_FALSE;
    Ns_ObjvSpec opts[] = {
        {"-checkforproxy", Ns_ObjvBool, &checkforproxy, INT2PTR(NS_TRUE)},
        {NULL, NULL,  NULL, NULL}
    };

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(opts, NULL, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        ServerListActive(dsPtr, poolPtr, (bool)checkforproxy);
    }
    return result;
}

static int
ServerListQueuedCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                 ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        ServerListQueued(dsPtr, poolPtr);
    }
    return result;
}

static int
ServerListAllCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv,
                 ConnPool *poolPtr, TCL_SIZE_T nargs)
{
    int         result = TCL_OK, checkforproxy = (int)NS_FALSE;
    Ns_ObjvSpec opts[] = {
        {"-checkforproxy", Ns_ObjvBool, &checkforproxy, INT2PTR(NS_TRUE)},
        {NULL, NULL,  NULL, NULL}
    };

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objv != NULL);
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (Ns_ParseObjv(opts, NULL, interp, objc-nargs, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        ServerListActive(dsPtr, poolPtr, (bool)checkforproxy);
        ServerListQueued(dsPtr, poolPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclServerObjCmd --
 *
 *      Implements "ns_server". This command provides configuration and status
 *      information about a server.
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
NsTclServerObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    int             subcmd = 0, effective = 0, result = TCL_OK;
    TCL_SIZE_T      nargs = 0;
    NsServer       *servPtr = NULL;
    ConnPool       *poolPtr;
    char           *pool = NULL;
    Tcl_DString     ds, *dsPtr = &ds;
#ifdef NS_WITH_DEPRECATED
    char           *optArg = NULL;
#endif

    enum {
        SActiveIdx, SAllIdx,
        SConnectionRateLimitIdx, SConnectionsIdx,
        SFiltersIdx,
        SHostsIdx,
#ifdef NS_WITH_DEPRECATED
        SKeepaliveIdx,
#endif
        SLogdirIdx,
        SMapIdx, SMappedIdx,
        SMaxthreadsIdx, SMinthreadsIdx,
        SPagedirIdx, SPoolRateLimitIdx, SPoolsIdx,
        SQueuedIdx,
        SRequestprocsIdx,
        SServerdirIdx, SStatsIdx,
        STcllibIdx, SThreadsIdx, STracesIdx,
        SUnmapIdx,
        SUrl2fileIdx, SVhostenabledIdx, SWaitingIdx
    };

    static Ns_ObjvTable subcmds[] = {
        {"active",              (unsigned int)SActiveIdx},
        {"all",                 (unsigned int)SAllIdx},
        {"connectionratelimit", (unsigned int)SConnectionRateLimitIdx},
        {"connections",         (unsigned int)SConnectionsIdx},
        {"filters",             (unsigned int)SFiltersIdx},
        {"hosts",               (unsigned int)SHostsIdx},
#ifdef NS_WITH_DEPRECATED
        {"keepalive",           (unsigned int)SKeepaliveIdx},
#endif
        {"logdir",              (unsigned int)SLogdirIdx},
        {"map",                 (unsigned int)SMapIdx},
        {"mapped",              (unsigned int)SMappedIdx},
        {"maxthreads",          (unsigned int)SMaxthreadsIdx},
        {"minthreads",          (unsigned int)SMinthreadsIdx},
        {"pagedir",             (unsigned int)SPagedirIdx},
        {"poolratelimit",       (unsigned int)SPoolRateLimitIdx},
        {"pools",               (unsigned int)SPoolsIdx},
        {"queued",              (unsigned int)SQueuedIdx},
        {"requestprocs",        (unsigned int)SRequestprocsIdx},
        {"serverdir",           (unsigned int)SServerdirIdx},
        {"stats",               (unsigned int)SStatsIdx},
        {"tcllib",              (unsigned int)STcllibIdx},
        {"threads",             (unsigned int)SThreadsIdx},
        {"traces",              (unsigned int)STracesIdx},
        {"unmap",               (unsigned int)SUnmapIdx},
        {"url2file",            (unsigned int)SUrl2fileIdx},
        {"vhostenabled",        (unsigned int)SVhostenabledIdx},
        {"waiting",             (unsigned int)SWaitingIdx},
        {NULL,                  0u}
    };
    Ns_ObjvSpec opts[] = {
        {"-server", Ns_ObjvServer,  &servPtr, NULL},
        {"-pool",   Ns_ObjvString,  &pool,    NULL},
        {"--",      Ns_ObjvBreak,   NULL,     NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec diropts[] = {
        {"-effective", Ns_ObjvBool,   &effective, INT2PTR(NS_TRUE)},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"subcmd",  Ns_ObjvIndex,  &subcmd,   subcmds},
        {"?arg",    Ns_ObjvArgs,   &nargs,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK)) {
        return TCL_ERROR;
    }

    if ((subcmd == SPoolsIdx
         || subcmd == SFiltersIdx
         || subcmd == SHostsIdx
         || subcmd == SLogdirIdx
         || subcmd == SPagedirIdx
         || subcmd == SRequestprocsIdx
         || subcmd == SUrl2fileIdx
         || subcmd == SVhostenabledIdx
         )
        && pool != NULL) {
            Ns_TclPrintfResult(interp, "option -pool is not allowed for this subcommand");
            return TCL_ERROR;
    }

#ifdef NS_WITH_DEPRECATED
    /*
     * Legacy handling for the following commands:
     *
     *    ns_server active ?pool?
     *    ns_server all ?pool?
     *    ns_server queued ?pool?
     *    ns_server connections ?pool?
     *    ns_server keepalive ?pool?
     *    ns_server pools ?pattern?
     *    ns_server waiting ?pool?
     */

    if (subcmd == SActiveIdx
        || subcmd == SAllIdx
        || subcmd == SQueuedIdx
        || subcmd == SConnectionsIdx
        || subcmd == SKeepaliveIdx
        || subcmd == SPoolsIdx
        || subcmd == SQueuedIdx
        || subcmd == SWaitingIdx
        ) {
        /*
         * Just for backwards compatibility
         */
        if (nargs > 0) {
            const char *last = Tcl_GetString(objv[objc-1]);
            bool        validPoolSyntax = (*last != '-');
            bool        legacy = NS_FALSE;

            if (objc >= 1) {
                const char *subCmdName = subcmds[subcmd].key;
                const char *secondLast = Tcl_GetString(objv[objc-2]);

                if (strcmp(subCmdName, secondLast) == 0 && validPoolSyntax) {
                    legacy = NS_TRUE;
                }
            }

            if (legacy) {
                Ns_LogDeprecated(objv, objc, "ns_server ?-pool /value/? ...",
                                 "Passing pool as second argument is deprecated.");
                optArg = Tcl_GetString(objv[objc-1]);
                pool = optArg;

            } else if (validPoolSyntax) {
                /*
                 * trigger usage error
                 */
                if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) != NS_OK) {
                    return TCL_ERROR;
                }
            }
        }
    }
#endif

    if (servPtr == NULL) {
        servPtr = itPtr->servPtr;
    }

    if (pool != NULL) {
        poolPtr = servPtr->pools.firstPtr;
        while (poolPtr != NULL && !STREQ(poolPtr->pool, pool)) {
            poolPtr = poolPtr->nextPtr;
            if (poolPtr != NULL) {
            }
        }
        if (unlikely(poolPtr == NULL)) {
            Ns_TclPrintfResult(interp, "no such pool '%s' for server '%s'", pool, servPtr->server);
            return TCL_ERROR;
        }
    } else {
        poolPtr = servPtr->pools.defaultPtr;
    }

    result = TCL_ERROR;

    switch (subcmd) {
        /*
         * The following subcommands are server specific (do not allow -pool option)
         */
    case SPoolsIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            for (poolPtr = servPtr->pools.firstPtr; poolPtr != NULL; poolPtr = poolPtr->nextPtr) {
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(poolPtr->pool, TCL_INDEX_NONE));
            }
            Tcl_SetObjResult(interp, listObj);
            result = TCL_OK;
        }
        break;

    case SFiltersIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_DStringInit(dsPtr);
            NsGetFilters(dsPtr, servPtr);
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;

    case SHostsIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_HashSearch  search;
            Tcl_HashEntry  *hPtr;
            Tcl_Obj        *listObj = Tcl_NewListObj(0, NULL);

            hPtr = Tcl_FirstHashEntry(&servPtr->hosts, &search);
            while (hPtr != NULL) {
                Tcl_ListObjAppendElement(interp, listObj,
                                         Tcl_NewStringObj(Tcl_GetHashKey(&servPtr->hosts, hPtr), TCL_INDEX_NONE));
                hPtr = Tcl_NextHashEntry(&search);
            }
            Tcl_SetObjResult(interp, listObj);
            result = TCL_OK;
        }
        break;

    case SPagedirIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_DStringInit(dsPtr);
            NsPageRoot(dsPtr, servPtr, NULL);
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;

    case SLogdirIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {

            Tcl_DStringInit(dsPtr);
            Ns_LogPath(dsPtr, servPtr->server, "");
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;

    case SServerdirIdx: {
        if (Ns_ParseObjv(diropts, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_DStringInit(dsPtr);
            if (effective) {
                Ns_ServerPath(dsPtr, servPtr->server, NS_SENTINEL);
            } else {
                Tcl_DStringAppend(dsPtr, servPtr->opts.serverdir, TCL_INDEX_NONE);
            }
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;
    }

    case SRequestprocsIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_DStringInit(dsPtr);
            NsGetRequestProcs(dsPtr, servPtr->server);
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;

    case STracesIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_DStringInit(dsPtr);
            NsGetTraces(dsPtr, servPtr);
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;

    case STcllibIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(servPtr->tcl.library, TCL_INDEX_NONE));
            result = TCL_OK;
        }
        break;

    case SUrl2fileIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_DStringInit(dsPtr);
            NsGetUrl2FileProcs(dsPtr, servPtr->server);
            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;

    case SVhostenabledIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(servPtr->vhost.enabled));
            result = TCL_OK;
        }
        break;

        /*
         * The following subcommands are pool-specific (allow -pool option)
         */

    case SWaitingIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->wqueue.wait.num));
            result = TCL_OK;
        }
        break;

#ifdef NS_WITH_DEPRECATED
    case SKeepaliveIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Ns_LogDeprecated(objv, objc, "ns_conn keepalive", NULL);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
            result = TCL_OK;
        }
        break;
#endif

    case SMapIdx:
        result = ServerMapObjCmd(clientData, interp, objc, objv, servPtr, poolPtr, (TCL_SIZE_T)nargs);
        break;

    case SMappedIdx:
        result = ServerMappedObjCmd(clientData, interp, objc, objv, servPtr, (TCL_SIZE_T)nargs);
        break;

    case SUnmapIdx:
        result = ServerUnmapObjCmd(clientData, interp, objc, objv, servPtr, (TCL_SIZE_T)nargs);
        break;

    case SMaxthreadsIdx:
        result = ServerMaxThreadsObjCmd(clientData, interp, objc, objv, poolPtr, (TCL_SIZE_T)nargs);
        break;

    case SPoolRateLimitIdx:
        result = ServerPoolRateLimitObjCmd(clientData, interp, objc, objv, poolPtr, (TCL_SIZE_T)nargs);
        break;

    case SConnectionRateLimitIdx:
        result = ServerConnectionRateLimitObjCmd(clientData, interp, objc, objv, poolPtr, (TCL_SIZE_T)nargs);
        break;

    case SMinthreadsIdx:
        result = ServerMinThreadsObjCmd(clientData, interp, objc, objv, poolPtr, (TCL_SIZE_T)nargs);
        break;

    case SConnectionsIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_SetObjResult(interp, Tcl_NewLongObj((long)poolPtr->stats.processed));
            result = TCL_OK;
        }
        break;

    case SStatsIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Tcl_DStringInit(dsPtr);

            Ns_DStringPrintf(dsPtr, "requests %lu ", poolPtr->stats.processed);
            Ns_DStringPrintf(dsPtr, "spools %lu ", poolPtr->stats.spool);
            Ns_DStringPrintf(dsPtr, "queued %lu ", poolPtr->stats.queued);
            Ns_DStringPrintf(dsPtr, "dropped %lu ", poolPtr->stats.dropped);
            Ns_DStringPrintf(dsPtr, "sent %" TCL_LL_MODIFIER "d ", poolPtr->rate.bytesSent);
            Ns_DStringPrintf(dsPtr, "connthreads %lu", poolPtr->stats.connthreads);

            Tcl_DStringAppend(dsPtr, " accepttime ", 12);
            Ns_DStringAppendTime(dsPtr, &poolPtr->stats.acceptTime);

            Tcl_DStringAppend(dsPtr, " queuetime ", 11);
            Ns_DStringAppendTime(dsPtr, &poolPtr->stats.queueTime);

            Tcl_DStringAppend(dsPtr, " filtertime ", 12);
            Ns_DStringAppendTime(dsPtr, &poolPtr->stats.filterTime);

            Tcl_DStringAppend(dsPtr, " runtime ", 9);
            Ns_DStringAppendTime(dsPtr, &poolPtr->stats.runTime);

            Tcl_DStringAppend(dsPtr, " tracetime ", 11);
            Ns_DStringAppendTime(dsPtr, &poolPtr->stats.traceTime);

            Tcl_DStringResult(interp, dsPtr);
            result = TCL_OK;
        }
        break;

    case SThreadsIdx:
        if (Ns_ParseObjv(NULL, NULL, interp, objc-nargs, objc, objv) == NS_OK) {
            Ns_MutexLock(&poolPtr->threads.lock);
            Ns_TclPrintfResult(interp,
                               "min %d max %d current %d idle %d stopping 0",
                               poolPtr->threads.min, poolPtr->threads.max,
                               poolPtr->threads.current, poolPtr->threads.idle);
            Ns_MutexUnlock(&poolPtr->threads.lock);
            result = TCL_OK;
        }
        break;

    case SActiveIdx:
        Tcl_DStringInit(dsPtr);
        result = ServerListActiveCmd(dsPtr, interp, objc, objv, poolPtr, (TCL_SIZE_T)nargs);
        if (likely(result == TCL_OK)) {
            Tcl_DStringResult(interp, dsPtr);
        } else {
            Tcl_DStringFree(dsPtr);
        }
        break;

    case SQueuedIdx:
        Tcl_DStringInit(dsPtr);
        result = ServerListQueuedCmd(dsPtr, interp, objc, objv, poolPtr, (TCL_SIZE_T)nargs);
        if (likely(result == TCL_OK)) {
            Tcl_DStringResult(interp, dsPtr);
        } else {
            Tcl_DStringFree(dsPtr);
        }
        break;

    case SAllIdx:
        Tcl_DStringInit(dsPtr);
        result = ServerListAllCmd(dsPtr, interp, objc, objv, poolPtr, (TCL_SIZE_T)nargs);
        if (likely(result == TCL_OK)) {
            Tcl_DStringResult(interp, dsPtr);
        } else {
            Tcl_DStringFree(dsPtr);
        }
        break;

    default:
        /* should never happen */
        assert(subcmd && 0);
    }

    return result;
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
    ConnPool     *poolPtr;
    Ns_Thread     joinThread;
    Ns_ReturnCode status;

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
            Ns_ThreadJoin(&joinThread, NULL);
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
        AppendConn(dsPtr, argPtr->connPtr, "running", NS_FALSE);
        Ns_MutexUnlock(&poolPtr->tqueue.lock);
    } else {
        Tcl_DStringAppendElement(dsPtr, NS_EMPTY_STRING);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ConnThreadSetName --
 *
 *      Set the conn thread name based on server name, pool name, threadID and
 *      connID. The pool name is always non-null, but might have an empty
 *      string as "name".
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Update thread name (internally just a printf operation)
 *
 *----------------------------------------------------------------------
 */

static void
ConnThreadSetName(const char *server, const char *pool, uintptr_t threadId, uintptr_t connId)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(pool != NULL);

    Ns_ThreadSetName("-conn:%s:%s:%" PRIuPTR ":%" PRIuPTR "-",
                     server, NsPoolName(pool), threadId, connId);
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
    ConnThreadArg *argPtr;
    ConnPool      *poolPtr;
    NsServer      *servPtr;
    Conn          *connPtr = NULL;
    Ns_Time        wait, *timePtr = &wait;
    uintptr_t      threadId;
    bool           duringShutdown, fromQueue;
    int            cpt, ncons, current;
    Ns_ReturnCode  status = NS_OK;
    Ns_Time        timeout;
    const char    *exitMsg;
    Ns_Thread      joinThread;
    Ns_Mutex      *threadsLockPtr, *tqueueLockPtr, *wqueueLockPtr;

    NS_NONNULL_ASSERT(arg != NULL);

    /*
     * Set the ConnThreadArg into thread local storage and get the id
     * of the thread.
     */
    argPtr = arg;
    poolPtr = argPtr->poolPtr;
    assert(poolPtr != NULL);

    tqueueLockPtr  = &poolPtr->tqueue.lock;
    Ns_TlsSet(&argtls, argPtr);

    Ns_MutexLock(tqueueLockPtr);
    argPtr->state = connThread_warmup;
    Ns_MutexUnlock(tqueueLockPtr);

    threadsLockPtr = &poolPtr->threads.lock;

    Ns_MutexLock(threadsLockPtr);
    threadId = poolPtr->threads.nextid++;
    if (poolPtr->threads.creating > 0) {
        poolPtr->threads.creating--;
    }
    Ns_MutexUnlock(threadsLockPtr);

    servPtr = poolPtr->servPtr;
    ConnThreadSetName(servPtr->server, poolPtr->pool, threadId, 0);

    Ns_ThreadSelf(&joinThread);

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
        Ns_Log(Notice, "thread initialized (" NS_TIME_FMT " secs)",
               (int64_t)diff.sec, diff.usec);
        Ns_TclDeAllocateInterp(interp);
        argPtr->state = connThread_ready;
    }

    wqueueLockPtr  = &poolPtr->wqueue.lock;

    /*
     * Start handling connections.
     */

    for (;;) {

        /*
         * We are ready to process requests. Pick it either a request
         * from the waiting queue, or go to a waiting state and add
         * yourself to the conn thread queue.
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
            fromQueue = NS_TRUE;
        } else {
            fromQueue = NS_FALSE;
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
             * is waiting for it. Therefore, we lock the connection
             * thread specific lock right here, also the signal sending
             * code uses the same lock.
             */
            Ns_MutexLock(&argPtr->lock);

            argPtr->nextPtr = poolPtr->tqueue.nextPtr;
            poolPtr->tqueue.nextPtr = argPtr;
            Ns_MutexUnlock(tqueueLockPtr);

            while (!servPtr->pools.shutdown) {

                Ns_GetTime(timePtr);
                Ns_IncrTime(timePtr, timeout.sec, timeout.usec);

                /*
                 * Wait until someone wakes us up, or a timeout happens.
                 */
                status = Ns_CondTimedWait(&argPtr->cond, &argPtr->lock, timePtr);

                if (unlikely(status == NS_TIMEOUT)) {
                    Ns_Log(Debug, "TIMEOUT");
                    if (unlikely(argPtr->connPtr != NULL)) {
                        /*
                         * This should not happen: we had a timeout, but there
                         * is a connection to be handled; when a connection
                         * comes in, we get signaled and should see therefore
                         * no timeout.  Maybe the signal was lost?
                         */
                        Ns_Log(Warning, "broadcast signal lost, resuming after timeout");
                        status = NS_OK;

                    } else if (poolPtr->threads.current <= poolPtr->threads.min) {
                        /*
                         * We have a timeout, but we should not reduce the
                         * number of threads below min-threads.
                         */
                        NsIdleCallback(servPtr);
                        continue;

                    } else {
                        /*
                         * We have a timeout, and the thread can exit.
                         */
                        Ns_Log(Debug, "We have a timeout, and the thread can exit");
                        break;
                    }
                }

                if (likely(argPtr->connPtr != NULL)) {
                    /*
                     * We got something to do; therefore, leave this loop.
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
                     aPtr != NULL;
                     prevPtr = &aPtr->nextPtr, aPtr = aPtr->nextPtr) {
                    if (aPtr == argPtr) {
                        /*
                         * This request is for us.
                         */
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

            if (servPtr->pools.shutdown) {
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
         * Run the connection if possible (requires a valid sockPtr and a
         * successful NsGetRequest() operation).
         */
        if (likely(connPtr->sockPtr != NULL)) {
            /*
             * Get the request from the sockPtr (either from read-ahead or via
             * parsing).
             */
            connPtr->reqPtr = NsGetRequest(connPtr->sockPtr, &connPtr->requestDequeueTime);

            /*
             * If there is no request, produce a warning and close the
             * connection.
             */
            if (connPtr->reqPtr == NULL) {
                Ns_Log(Warning, "connPtr %p has no reqPtr, close this connection", (void *)connPtr);
                (void) Ns_ConnClose((Ns_Conn *)connPtr);
            } else {
                /*
                 * Everything is supplied, run the request. ConnRun()
                 * closes finally the connection.
                 */
                ConnThreadSetName(servPtr->server, poolPtr->pool, threadId, connPtr->id);
                ConnRun(connPtr);
            }
        } else {
            /*
             * If we have no sockPtr, we can't do much here.
             */
            Ns_Log(Warning, "connPtr %p has no socket, close this connection", (void *)connPtr);
            (void) Ns_ConnClose((Ns_Conn *)connPtr);
        }

        /*
         * Protect connPtr->headers (and other members) against other threads,
         * since we are deallocating its content. This is especially important
         * for e.g. "ns_server active" since it accesses the header fields.
         */
        Ns_MutexLock(tqueueLockPtr);
        connPtr->flags &= ~NS_CONN_CONFIGURED;

        /*
         * We are done with the headers, reset these for further reuse.
         */
        Ns_SetTrunc(connPtr->headers, 0);

        argPtr->state = connThread_ready;
        Ns_MutexUnlock(tqueueLockPtr);

        /*
         * Push connection to the free list.
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

                Ns_Log(Debug, "[%d] end of job, waiting %d current %d idle %d ncons %d fromQueue %d"
                       " start " NS_TIME_FMT
                       " " NS_TIME_FMT
                       " accept " NS_TIME_FMT
                       " queue " NS_TIME_FMT
                       " filter " NS_TIME_FMT
                       " run " NS_TIME_FMT
                       " netrun " NS_TIME_FMT
                       " total " NS_TIME_FMT,
                       ThreadNr(poolPtr, argPtr),
                       waiting, poolPtr->threads.current, idle, ncons, fromQueue ? 1 : 0,
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
    duringShutdown = servPtr->pools.shutdown;
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
        if (wakeup && connPtr != NULL && !duringShutdown) {
            assert(connPtr->drvPtr != NULL);
            NsWakeupDriver(connPtr->drvPtr);
        }
    }

    /*
     * During shutdown, the main thread waits for signals on the
     * condition variable to check whether all threads have terminated
     * already.
     */
    if (duringShutdown) {
        Ns_CondSignal(&poolPtr->wqueue.cond);
    }

    Ns_MutexLock(&servPtr->pools.lock);
    joinThread = servPtr->pools.joinThread;
    Ns_ThreadSelf(&servPtr->pools.joinThread);
    Ns_MutexUnlock(&servPtr->pools.lock);

    if (joinThread != NULL) {
        Ns_ThreadJoin(&joinThread, NULL);
    }

    Ns_Log(Notice, "exiting: %s", exitMsg);

    Ns_MutexLock(tqueueLockPtr);
    argPtr->state = connThread_free;
    Ns_MutexUnlock(tqueueLockPtr);

    Ns_ThreadExit(argPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * NsHeaderSetGet --
 *
 *      Return an Ns_Set for request headers with some defaults.
 *
 * Results:
 *      Ns_Set *
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Ns_Set *NsHeaderSetGet(size_t size)
{
    Ns_Set *result;

    result = Ns_SetCreateSz(NS_SET_NAME_REQUEST, MAX(10, size));
    result->flags |= NS_SET_OPTION_NOCASE;
#ifdef NS_SET_DSTRING
    NsSetDataPrealloc(result, 4095);
#endif

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ConnRun --
 *
 *      Run the actual non-null request and close it finally the connection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potential side-effects caused by the callbacks.
 *
 *----------------------------------------------------------------------
 */
static void
ConnRun(Conn *connPtr)
{
    Sock           *sockPtr;
    Ns_Conn        *conn;
    NsServer       *servPtr;
    Ns_ReturnCode   status;
    const char     *auth;

    NS_NONNULL_ASSERT(connPtr != NULL);

    conn = (Ns_Conn *)connPtr;
    sockPtr = connPtr->sockPtr;

    assert(sockPtr != NULL);
    assert(sockPtr->reqPtr != NULL);

    /*
     * Make sure we update peer address with actual remote IP address
     */
    (void) Ns_ConnSetPeer(conn,
                          (struct sockaddr *)&(sockPtr->sa),
                          (struct sockaddr *)&(sockPtr->clientsa)
                          );

    /*
     * Get the request data from the reqPtr to ease life-time management in
     * connection threads. It would be probably sufficient to clear just the
     * request line, but we want to play it safe and clear everything.
     */
    connPtr->request = connPtr->reqPtr->request;
    memset(&(connPtr->reqPtr->request), 0, sizeof(struct Ns_Request));

    /*
      Ns_Log(Notice, "ConnRun connPtr %p req %s", (void*)connPtr, connPtr->request.line);
    */

    /*
     * Move connPtr->reqPtr->headers to connPtr->headers (named "req") for the
     * delivery thread and get a fresh or preallocated structure for the next
     * request in this connection thread.
     */
    {
        Ns_Set *preallocedHeaders = connPtr->headers;
        if (preallocedHeaders == NULL) {
            preallocedHeaders = NsHeaderSetGet(connPtr->reqPtr->headers->maxSize);
        } else {
#ifdef NS_SET_DSTRING
            Ns_Log(Ns_LogNsSetDebug, "SSS ConnRun REUSE %p '%s': size %lu/%lu "
                   "buffer %" PRITcl_Size "/%" PRITcl_Size,
                   (void*)preallocedHeaders, preallocedHeaders->name,
                   preallocedHeaders->size, preallocedHeaders->maxSize,
                   preallocedHeaders->data.length, preallocedHeaders->data.spaceAvl);
#endif
        }

        connPtr->headers = connPtr->reqPtr->headers;
        connPtr->reqPtr->headers = preallocedHeaders;
    }

    /*
     * Flag, that the connection is fully configured and we can use its
     * data.
     */
    connPtr->flags |= NS_CONN_CONFIGURED;
    connPtr->contentLength = connPtr->reqPtr->length;

    connPtr->nContentSent = 0u;
    connPtr->responseStatus = 200;
    connPtr->responseLength = -1;  /* -1 == unknown (stream), 0 == zero bytes. */
    connPtr->recursionCount = 0;
    connPtr->auth = NULL;

    /*
     * keep == -1 means: Undecided, the default keep-alive rules are applied.
     */
    connPtr->keep = -1;

    servPtr = connPtr->poolPtr->servPtr;
    Ns_ConnSetCompression(conn, servPtr->compress.enable ? servPtr->compress.level : 0);
    connPtr->compress = -1;

    connPtr->outputEncoding = servPtr->encoding.outputEncoding;
    connPtr->urlEncoding = servPtr->encoding.urlEncoding;

    Tcl_InitHashTable(&connPtr->files, TCL_STRING_KEYS);

    memcpy(connPtr->idstr, "cns", 3u);
    (void)ns_uint64toa(&connPtr->idstr[3], (uint64_t)connPtr->id);

    if (connPtr->outputheaders == NULL) {
        connPtr->outputheaders = Ns_SetCreate(NS_SET_NAME_RESPONSE);
        connPtr->outputheaders->flags |= NS_SET_OPTION_NOCASE;
    }

    if (connPtr->request.version < 1.0) {
        conn->flags |= NS_CONN_SKIPHDRS;
    }
    if (servPtr->opts.hdrcase != Preserve) {
        size_t i;

        for (i = 0u; i < Ns_SetSize(connPtr->headers); ++i) {
            if (servPtr->opts.hdrcase == ToLower) {
                Ns_StrToLower(Ns_SetKey(connPtr->headers, i));
            } else {
                Ns_StrToUpper(Ns_SetKey(connPtr->headers, i));
            }
        }
    }
    //auth = Ns_SetIGet(connPtr->headers, "authorization");
    auth = sockPtr->extractedHeaderFields[NS_EXTRACTED_HEADER_AUTHORIZATION];

    if (auth != NULL) {
        NsParseAuth(connPtr, auth);
    }
    if ((conn->request.method != NULL) && STREQ(conn->request.method, "HEAD")) {
        conn->flags |= NS_CONN_SKIPBODY;
    }

    if (sockPtr->drvPtr->requestProc != NULL) {
        /*
         * Run the driver's private handler
         */
        Ns_GetTime(&connPtr->filterDoneTime);
        status = (*sockPtr->drvPtr->requestProc)(sockPtr->drvPtr->arg, conn);

    } else if (connPtr->request.requestType == NS_REQUEST_TYPE_PROXY
               || connPtr->request.requestType == NS_REQUEST_TYPE_CONNECT
               ) {
        /*
         * Run proxy request
         */
        Ns_GetTime(&connPtr->filterDoneTime);
        status = NsConnRunProxyRequest((Ns_Conn *) connPtr);

    } else {
        /*
         * Run classical HTTP requests
         */

        status = NsRunFilters(conn, NS_FILTER_PRE_AUTH);
        Ns_GetTime(&connPtr->filterDoneTime);

        if (connPtr->sockPtr == NULL) {
            /*
             * If - for what-ever reason - a filter has closed the connection,
             * treat the result as NS_FILTER_RETURN. Other feedback to this
             * connection can not work anymore.
             */
            Ns_Log(Debug, "Preauth filter closed connection; cancel further request processing");
            status = NS_FILTER_RETURN;
        }

        if (status == NS_OK) {
            status = NsAuthorizeRequest(servPtr,
                                        connPtr->request.method,
                                        connPtr->request.url,
                                        Ns_ConnAuthUser(conn),
                                        Ns_ConnAuthPasswd(conn),
                                        Ns_ConnPeerAddr(conn));
            switch (status) {
            case NS_OK:
                status = NsRunFilters(conn, NS_FILTER_POST_AUTH);
                Ns_GetTime(&connPtr->filterDoneTime);
                if (status == NS_OK && (connPtr->sockPtr != NULL)) {
                    /*
                     * Run the actual request
                     */
                    status = Ns_ConnRunRequest(conn);
                }
                break;

            case NS_FORBIDDEN:
                (void) Ns_ConnReturnForbidden(conn);
                break;

            case NS_UNAUTHORIZED:
                (void) Ns_ConnReturnUnauthorized(conn);
                break;

            case NS_CONTINUE:       NS_FALL_THROUGH; /* fall through */
            case NS_ERROR:          NS_FALL_THROUGH; /* fall through */
            case NS_FILTER_BREAK:   NS_FALL_THROUGH; /* fall through */
            case NS_FILTER_RETURN:  NS_FALL_THROUGH; /* fall through */
            case NS_TIMEOUT:
                (void)Ns_ConnTryReturnInternalError(conn, status, "after authorize request");
                break;
            }
        } else if (status != NS_FILTER_RETURN) {
            /*
             * If not ok or filter_return, then the pre-auth filter caught
             * an error.  We are not going to proceed, but also we
             * can't count on the filter to have sent a response
             * back to the client.  So, send an error response.
             */
            (void)Ns_ConnTryReturnInternalError(conn, status, "after pre_auth filter");
            /*
             * Set the status so that NS_FILTER_TRACE can still run.
             */
            status = NS_FILTER_RETURN;
        }
    }

    /*
     * Update run time statistics to make these usable for traces (e.g. access log).
     */
    NsConnTimeStatsUpdate(conn);

    if ((status == NS_OK) || (status == NS_FILTER_RETURN)) {
        status = NsRunFilters(conn, NS_FILTER_TRACE);
        if (status == NS_OK) {
            (void) NsRunFilters(conn, NS_FILTER_VOID_TRACE);
            /*
             * Run Server traces (e.g. writing access log entries)
             */
            NsRunTraces(conn);
        }
    } else {
        NsAddNslogEntry(sockPtr, connPtr->responseStatus, conn, NULL);

        Ns_Log(Notice, "not running NS_FILTER_TRACE status %d http status code %d: %s",
               status, connPtr->responseStatus, connPtr->request.url);
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
     * In case some leftover is in the buffer, signal the driver to
     * process the remaining bytes.
     *
     */
    {
        bool wakeup;

        Ns_MutexLock(&sockPtr->drvPtr->lock);
        wakeup = (sockPtr->keep && (connPtr->reqPtr->leftover > 0u));
        Ns_MutexUnlock(&sockPtr->drvPtr->lock);

        if (wakeup) {
            NsWakeupDriver(sockPtr->drvPtr);
        }
    }

    /*
     * Close the connection. This might free as well the content of
     * connPtr->reqPtr, so set it to NULL to avoid surprises, if someone might
     * want to access these structures.
     */

    (void) Ns_ConnClose(conn);

    Ns_MutexLock(&connPtr->poolPtr->tqueue.lock);
    connPtr->reqPtr = NULL;
    Ns_MutexUnlock(&connPtr->poolPtr->tqueue.lock);

    /*
     * Deactivate stream writer, if defined
     */
    if (connPtr->fd != 0) {
        connPtr->fd = 0;
    }
    if (connPtr->strWriter != NULL) {
        void *wrPtr;

        NsWriterLock();
        /*
         * Avoid potential race conditions, so refetch inside the lock.
         */
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

    Ns_SetTrunc(connPtr->outputheaders, 0);

    if (connPtr->request.line != NULL) {
        /*
         * reqPtr is freed by FreeRequest() in the driver.
         */
        Ns_ResetRequest(&connPtr->request);
        assert(connPtr->request.line == NULL);
    }
    if (connPtr->location != NULL) {
        ns_free((char *)connPtr->location);
        connPtr->location = NULL;
    }

    if (connPtr->clientData != NULL) {
        ns_free(connPtr->clientData);
        connPtr->clientData = NULL;
    }

    NsConnTimeStatsFinalize(conn);

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
    int            i;

#if !defined(NDEBUG)
    { const char *threadName = Ns_ThreadGetName();
      assert(strncmp("-driver:", threadName, 8u) == 0
             || strncmp("-main", threadName, 5u) == 0
             || strncmp("-spooler", threadName, 8u) == 0
             || strncmp("-service-", threadName, 9u) == 0
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
        //argPtr->cond = NULL;

        Ns_ThreadCreate(NsConnThread, argPtr, 0, &thread);
    } else {
        Ns_MutexUnlock(&poolPtr->tqueue.lock);

        Ns_MutexLock(&poolPtr->threads.lock);
        poolPtr->threads.current --;
        poolPtr->threads.creating --;
        Ns_MutexUnlock(&poolPtr->threads.lock);

        Ns_Log(Debug, "Cannot create additional connection thread in pool '%s', "
               "maxthreads (%d) are running", poolPtr->pool, i);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AppendConn --
 *
 *      Append connection data to a Tcl_DString.
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
AppendConn(Tcl_DString *dsPtr, const Conn *connPtr, const char *state, bool checkforproxy)
{
    Ns_Time now, diff;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(state != NULL);

    /*
     * An annoying race condition can be lethal here.
     *
     * In the state "waiting", we have never a connPtr->reqPtr, therefore, we
     * can't even determine the peer address, nor the request method or the
     * request URL. Furthermore, there is no way to honor the "checkforproxy"
     * flag.
     */
    if (connPtr != NULL) {
        Tcl_DStringStartSublist(dsPtr);

        if (connPtr->reqPtr != NULL) {
            const char *p;

            Tcl_DStringAppendElement(dsPtr, connPtr->idstr);

            if (checkforproxy) {
                /*
                 * The user requested explicitly for "checkforproxy", so only
                 * return the proxy value.
                 */
                p = Ns_ConnForwardedPeerAddr((const Ns_Conn *)connPtr);
            } else {
                p = Ns_ConnConfiguredPeerAddr((const Ns_Conn *)connPtr);
            }
            Tcl_DStringAppendElement(dsPtr, p);
        } else {
            /*
             * connPtr->reqPtr == NULL. Having no connPtr->reqPtr is normal
             * for "queued" requests but not for "running" requests. Report
             * this in the system log.
             */
            Tcl_DStringAppendElement(dsPtr, "unknown");
            if (*state == 'r') {
                Ns_Log(Notice,
                       "AppendConn state '%s': request not available, can't determine peer address",
                       state);
            }
        }

        Tcl_DStringAppendElement(dsPtr, state);

        if (connPtr->request.line != NULL) {
            Tcl_DStringAppendElement(dsPtr, (connPtr->request.method != NULL) ? connPtr->request.method : "?");
            Tcl_DStringAppendElement(dsPtr, (connPtr->request.url    != NULL) ? connPtr->request.url : "?");
        } else {
            /* Ns_Log(Notice, "AppendConn: no request in state %s; ignore conn in output", state);*/
            Tcl_DStringAppendElement(dsPtr, "unknown");
            Tcl_DStringAppendElement(dsPtr, "unknown");
        }
        Ns_GetTime(&now);
        Ns_DiffTime(&now, &connPtr->requestQueueTime, &diff);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &diff);
        Ns_DStringPrintf(dsPtr, " %" PRIuz, connPtr->nContentSent);

        Tcl_DStringEndSublist(dsPtr);
    } else {
        Tcl_DStringAppendElement(dsPtr, NS_EMPTY_STRING);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AppendConnList --
 *
 *      Append list of connection data to a Tcl_DString.
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
AppendConnList(Tcl_DString *dsPtr, const Conn *firstPtr, const char *state, bool checkforproxy)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(state != NULL);

    while (firstPtr != NULL) {
        AppendConn(dsPtr, firstPtr, state, checkforproxy);
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
