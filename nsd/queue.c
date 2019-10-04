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

static int ServerMaxThreadsObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                                  ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static int ServerMinThreadsObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                                  ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);


static int ServerConnectionRateLimitObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                                           ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);


static int ServerPoolRateLimitObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                                     ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);


static int ServerMapObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                           NsServer  *servPtr, ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

static int ServerMappedObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                              NsServer *servPtr, int nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static int ServerUnmapObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                             NsServer *servPtr, int nargs)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static void ConnThreadSetName(const char *server, const char *pool, uintptr_t threadId, uintptr_t connId)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int ServerListActiveCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                               ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
static int ServerListAllCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                            ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
static int ServerListQueuedCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                               ConnPool *poolPtr, int nargs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static void ServerListActive(Tcl_DString *dsPtr, ConnPool *poolPtr, bool checkforproxy)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void ServerListQueued(Tcl_DString *dsPtr, ConnPool *poolPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int SetPoolAttribute(Tcl_Interp *interp, int nargs, ConnPool *poolPtr, int *valuePtr, int value)
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

    mapspecObj = Tcl_NewStringObj(mapString, -1);
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

    return (dlPtr->size -1u);
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

    dlPtr = &(poolPtr->rate.writerRates);
    dlPtr->data[slot] = (void*)(uintptr_t)rate;

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

    Ns_MutexUnlock(&poolPtr->threads.lock);
    Ns_MutexUnlock(&poolPtr->wqueue.lock);

    if (create) {
        Ns_Log(Notice, "NsEnsureRunningConnectionThreads wantCreate %d waiting %d idle %d current %d",
               (int)create,
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
 *      NS_TRUE if queued, NS_FALSE otherwise.
 *
 * Side effects:
 *      Connection will run shortly.
 *
 *----------------------------------------------------------------------
 */

bool
NsQueueConn(Sock *sockPtr, const Ns_Time *nowPtr)
{
    ConnThreadArg *argPtr = NULL;
    NsServer      *servPtr;
    ConnPool      *poolPtr = NULL;
    Conn          *connPtr = NULL;
    bool           create = NS_FALSE, queued = NS_TRUE;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(nowPtr != NULL);
    assert(sockPtr->drvPtr != NULL);

    sockPtr->drvPtr->stats.received++;
    servPtr = sockPtr->servPtr;

    /*
     * Select server connection pool. For non-HTTP drivers, the request.method
     * won't be provided.
     */

    if ((sockPtr->reqPtr != NULL) && (sockPtr->reqPtr->request.method != NULL)) {
        NsUrlSpaceContext ctx;

        ctx.headers = sockPtr->reqPtr->headers;
        ctx.saPtr = (struct sockaddr *)&(sockPtr->sa);
        poolPtr = NsUrlSpecificGet(servPtr,
                                   sockPtr->reqPtr->request.method,
                                   sockPtr->reqPtr->request.url,
                                   poolid, 0u, NS_URLSPACE_DEFAULT,
                                   NsUrlSpaceContextFilter, &ctx);
    }
    if (poolPtr == NULL) {
        poolPtr = servPtr->pools.defaultPtr;
    }

   /*
    * Queue connection if possible (e.g. no shutdown, a free Conn is
    * available, ...)
    */

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
            connPtr->location             = sockPtr->location;
            connPtr->flags                = sockPtr->flags;
            if ((sockPtr->drvPtr->opts & NS_DRIVER_ASYNC) == 0u) {
                connPtr->acceptTime       = *nowPtr;
            } else {
                connPtr->acceptTime       = sockPtr->acceptTime;
            }
            sockPtr->acceptTime.sec       = 0; /* invalidate time */
            connPtr->rateLimit            = poolPtr->rate.defaultConnectionLimit;

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
    }

    if (connPtr == NULL) {
        Ns_Log(Notice, "[%s pool %s] All available connections are used, waiting %d idle %d current %d",
               poolPtr->servPtr->server,
               poolPtr->pool,
               poolPtr->wqueue.wait.num,
               poolPtr->threads.idle,
               poolPtr->threads.current);
        queued = NS_FALSE;
        create = NS_FALSE;

    } else if (argPtr != NULL) {
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
WalkCallback(Ns_DString *dsPtr, const void *arg)
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
SetPoolAttribute(Tcl_Interp *interp, int nargs, ConnPool *poolPtr, int *valuePtr, int value) {

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
 *    Implements the "ns_server ... maxthreads ..." command.
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
ServerMaxThreadsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                       ConnPool *poolPtr, int nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {poolPtr->threads.min, poolPtr->wqueue.maxconns};
    Ns_ObjvSpec       args[] = {
        {"?maxthreads",   Ns_ObjvInt, &value, &range},
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
 *    Implements the "ns_server ... minthreads ..." command.
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
ServerMinThreadsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                       ConnPool *poolPtr, int nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {1, poolPtr->threads.max};
    Ns_ObjvSpec       args[] = {
        {"?minthreads", Ns_ObjvInt, &value, &range},
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
ServerPoolRateLimitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                       ConnPool *poolPtr, int nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {-1, INT_MAX};
    Ns_ObjvSpec       args[] = {
        {"?poolratelimit", Ns_ObjvInt, &value, &range},
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
ServerConnectionRateLimitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                                ConnPool *poolPtr, int nargs)
{
    int               result = TCL_OK, value = 0;
    Ns_ObjvValueRange range = {-1, INT_MAX};
    Ns_ObjvSpec       args[] = {
        {"?connectionratelimit", Ns_ObjvInt, &value, &range},
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
    int           oc;
    Tcl_Obj     **ov;

    NS_NONNULL_ASSERT(mapspecObj != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(specPtr != NULL);

    if (Tcl_ListObjGetElements(NULL, mapspecObj, &oc, &ov) == TCL_OK) {
        if (oc == 2 || oc == 3) {
            status = NS_OK;
            *method = Tcl_GetString(ov[0]);
            *url = Tcl_GetString(ov[1]);
            if (oc == 3) {
                int        oc2;
                Tcl_Obj  **ov2;

                if (Tcl_ListObjGetElements(NULL, ov[2], &oc2, &ov2) == TCL_OK && oc2 == 2) {
                    *specPtr = NsUrlSpaceContextSpecNew(Tcl_GetString(ov2[0]),
                                                        Tcl_GetString(ov2[1]));

                } else {
                    status = NS_ERROR;
                }
            } else {
                *specPtr = NULL;
            }
        }
    }
    if (unlikely(status == NS_ERROR) && interp != NULL) {
        Ns_TclPrintfResult(interp,
                           "invalid mapspec '%s'; must be 2- or 3-element list "
                           "containing HTTP method, URL, and optionally a filtercontext",
                           Tcl_GetString(mapspecObj));
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerMapObjCmd, subcommand of NsTclServerObjCmd --
 *
 *    Implements the "ns_server ... map ..." command.
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
ServerMapObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                NsServer  *servPtr, ConnPool *poolPtr, int nargs)
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
        Tcl_Obj     **ov, *fullListObj;
        int          oc;

        /*
         * Return the current mappings just in the case, when the map
         * operation was called without the optional argument.
         */
        Ns_DStringInit(dsPtr);

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
            int i;

            /*
             * The result should be always a proper list, so the potential
             * error should not occur.
             */
            resultObj = Tcl_NewListObj(0, NULL);

            for (i = 0; i < oc; i++) {
                Tcl_Obj *elemObj = ov[i];
                int      length;

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
 *    Implements the "ns_server ... mapped " command.
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
ServerMappedObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                  NsServer *servPtr, int nargs)
{
    int          result = TCL_OK, noinherit = 0, exact = 0;
    Tcl_Obj     *mapspecObj = NULL;
    char        *method, *url;
    NsUrlSpaceContextSpec *specPtr;
    Ns_ObjvSpec  lopts[] = {
        {"-exact",     Ns_ObjvBool,   &exact, INT2PTR(NS_TRUE)},
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
        NsUrlSpaceOp    op;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }

        if (exact == (int)NS_TRUE) {
            op = NS_URLSPACE_EXACT;
        } else {
            op = NS_URLSPACE_DEFAULT;
        }

        Ns_MutexLock(&servPtr->urlspace.lock);
        mappedPoolPtr = (ConnPool *)NsUrlSpecificGet(servPtr,  method, url, poolid, flags, op,
                                                     NULL, NULL);
        Ns_MutexUnlock(&servPtr->urlspace.lock);

        if (mappedPoolPtr != NULL) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(mappedPoolPtr->pool, -1));
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ServerUnmapObjCmd, subcommand of NsTclServerObjCmd --
 *
 *    Implements the "ns_server ... unmap ..." command.
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
ServerUnmapObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                  NsServer *servPtr, int nargs)
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
        flags |= NS_OP_ALLFILTERS;

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
ServerListActiveCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                 ConnPool *poolPtr, int nargs)
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
ServerListQueuedCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                 ConnPool *poolPtr, int nargs)
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
ServerListAllCmd(Tcl_DString *dsPtr, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv,
                 ConnPool *poolPtr, int nargs)
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
NsTclServerObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    int             subcmd = 0, result = TCL_OK, nargs = 0;
    NsServer       *servPtr = NULL;
    ConnPool       *poolPtr;
    char           *pool = NULL, *optArg = NULL;
    Tcl_DString     ds, *dsPtr = &ds;

    enum {
        SActiveIdx, SAllIdx,
        SConnectionsIdx, SConnectionRateLimitIdx,
        SFiltersIdx,
        SKeepaliveIdx,
        SMapIdx, SMappedIdx,
        SMaxthreadsIdx, SMinthreadsIdx,
        SPagedirIdx, SPoolRateLimitIdx, SPoolsIdx,
        SQueuedIdx,
        SRequestprocsIdx,
        SServerdirIdx, SStatsIdx,
        STcllibIdx, SThreadsIdx, STracesIdx,
        SUnmapIdx,
        SUrl2fileIdx, SWaitingIdx
    };

    static Ns_ObjvTable subcmds[] = {
        {"active",              (unsigned int)SActiveIdx},
        {"all",                 (unsigned int)SAllIdx},
        {"connectionratelimit", (unsigned int)SConnectionRateLimitIdx},
        {"connections",         (unsigned int)SConnectionsIdx},
        {"filters",             (unsigned int)SFiltersIdx},
        {"keepalive",           (unsigned int)SKeepaliveIdx},
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
        {"waiting",             (unsigned int)SWaitingIdx},
        {NULL,                  0u}
    };
    Ns_ObjvSpec opts[] = {
        {"-server", Ns_ObjvServer,  &servPtr, NULL},
        {"-pool",   Ns_ObjvString,  &pool,    NULL},
        {"--",      Ns_ObjvBreak,   NULL,     NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"subcmd",  Ns_ObjvIndex,  &subcmd,   subcmds},
        {"?args",   Ns_ObjvArgs,   &nargs,    NULL},
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
            Ns_TclPrintfResult(interp, "option -pool is not allowed for this subcommand");
            return TCL_ERROR;
    }

    if (subcmd != SMinthreadsIdx
        && subcmd != SMaxthreadsIdx
        && subcmd != SMapIdx
        && subcmd != SMappedIdx
        && subcmd != SUnmapIdx
        && subcmd != SActiveIdx
        && subcmd != SQueuedIdx
        && subcmd != SAllIdx
        && subcmd != SPoolRateLimitIdx
        && subcmd != SConnectionRateLimitIdx
        ) {
        /*
         * Just for backwards compatibility
         */
        if (nargs > 0) {
            Ns_LogDeprecated(objv, objc, "ns_server ?-pool p? ...",
                             "Passing pool as second argument is deprecated.");
            optArg = Tcl_GetString(objv[objc-1]);
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

    switch (subcmd) {
        /*
         * These subcommands are server specific (do not allow -pool option)
         */
    case SPoolsIdx:
        {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            for (poolPtr = servPtr->pools.firstPtr; poolPtr != NULL; poolPtr = poolPtr->nextPtr) {
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(poolPtr->pool, -1));
            }
            Tcl_SetObjResult(interp, listObj);
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

    case SMapIdx:
        result = ServerMapObjCmd(clientData, interp, objc, objv, servPtr, poolPtr, nargs);
        break;

    case SMappedIdx:
        result = ServerMappedObjCmd(clientData, interp, objc, objv, servPtr,nargs);
        break;

    case SUnmapIdx:
        result = ServerUnmapObjCmd(clientData, interp, objc, objv, servPtr, nargs);
        break;

    case SMaxthreadsIdx:
        result = ServerMaxThreadsObjCmd(clientData, interp, objc, objv, poolPtr, nargs);
        break;

    case SPoolRateLimitIdx:
        result = ServerPoolRateLimitObjCmd(clientData, interp, objc, objv, poolPtr, nargs);
        break;

    case SConnectionRateLimitIdx:
        result = ServerConnectionRateLimitObjCmd(clientData, interp, objc, objv, poolPtr, nargs);
        break;

    case SMinthreadsIdx:
        result = ServerMinThreadsObjCmd(clientData, interp, objc, objv, poolPtr, nargs);
        break;

    case SConnectionsIdx:
        Tcl_SetObjResult(interp, Tcl_NewLongObj((long)poolPtr->stats.processed));
        break;

    case SStatsIdx:
        Tcl_DStringInit(dsPtr);

        Ns_DStringPrintf(dsPtr, "requests %lu ", poolPtr->stats.processed);
        Ns_DStringPrintf(dsPtr, "spools %lu ", poolPtr->stats.spool);
        Ns_DStringPrintf(dsPtr, "queued %lu ", poolPtr->stats.queued);
        Ns_DStringPrintf(dsPtr, "sent %" TCL_LL_MODIFIER "d ", poolPtr->rate.bytesSent);
        Ns_DStringPrintf(dsPtr,  "connthreads %lu", poolPtr->stats.connthreads);

        Ns_DStringAppend(dsPtr, " accepttime ");
        Ns_DStringAppendTime(dsPtr, &poolPtr->stats.acceptTime);

        Ns_DStringAppend(dsPtr, " queuetime ");
        Ns_DStringAppendTime(dsPtr, &poolPtr->stats.queueTime);

        Ns_DStringAppend(dsPtr, " filtertime ");
        Ns_DStringAppendTime(dsPtr, &poolPtr->stats.filterTime);

        Ns_DStringAppend(dsPtr, " runtime ");
        Ns_DStringAppendTime(dsPtr, &poolPtr->stats.runTime);

        Ns_DStringAppend(dsPtr, " tracetime ");
        Ns_DStringAppendTime(dsPtr, &poolPtr->stats.traceTime);

        Tcl_DStringResult(interp, dsPtr);
        break;

    case SThreadsIdx:
        Ns_TclPrintfResult(interp,
                           "min %d max %d current %d idle %d stopping 0",
                           poolPtr->threads.min, poolPtr->threads.max,
                           poolPtr->threads.current, poolPtr->threads.idle);
        break;

    case SActiveIdx:
        Tcl_DStringInit(dsPtr);
        result = ServerListActiveCmd(dsPtr, interp, objc, objv, poolPtr, nargs);
        if (likely(result == NS_OK)) {
            Tcl_DStringResult(interp, dsPtr);
        } else {
            Tcl_DStringFree(dsPtr);
        }
        break;

    case SQueuedIdx:
        Tcl_DStringInit(dsPtr);
        result = ServerListQueuedCmd(dsPtr, interp, objc, objv, poolPtr, nargs);
        if (likely(result == NS_OK)) {
            Tcl_DStringResult(interp, dsPtr);
        } else {
            Tcl_DStringFree(dsPtr);
        }
        break;

    case SAllIdx:
        Tcl_DStringInit(dsPtr);
        result = ServerListAllCmd(dsPtr, interp, objc, objv, poolPtr, nargs);
        if (likely(result == NS_OK)) {
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

    if (*pool != '\0') {
        /*
         * Non-Empty pool name.
         */
        Ns_ThreadSetName("-conn:%s:%s:%" PRIuPTR ":%" PRIuPTR "-",
                         server, pool, threadId, connId);
    } else {
        /*
         * Empty pool name.
         */
        Ns_ThreadSetName("-conn:%s:default:%" PRIuPTR ":%" PRIuPTR "-",
                         server, threadId, connId);
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
    ConnThreadArg *argPtr;
    ConnPool      *poolPtr;
    NsServer      *servPtr;
    Conn          *connPtr = NULL;
    Ns_Time        wait, *timePtr = &wait;
    uintptr_t      threadId;
    bool           duringShutdown, fromQueue;
    int            cpt, ncons, current;
    Ns_ReturnCode  status = NS_OK;
    long           timeout;
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
        Ns_Log(Notice, "thread initialized (%" PRId64 ".%06ld secs)",
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
            while (!servPtr->pools.shutdown) {

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
                     aPtr != NULL;
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
                       " start %" PRId64 ".%06ld"
                       " %" PRId64 ".%06ld"
                       " accept %" PRId64 ".%06ld"
                       " queue %" PRId64 ".%06ld"
                       " filter %" PRId64 ".%06ld"
                       " run %" PRId64 ".%06ld"
                       " netrun %" PRId64 ".%06ld"
                       " total %" PRId64 ".%06ld",
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
    const NsServer *servPtr;
    Ns_ReturnCode   status;
    char           *auth;

    NS_NONNULL_ASSERT(connPtr != NULL);

    conn = (Ns_Conn *)connPtr;
    sockPtr = connPtr->sockPtr;

    assert(sockPtr != NULL);
    assert(sockPtr->reqPtr != NULL);

    /*
     * Make sure we update peer address with actual remote IP address
     */
    (void) Ns_ConnSetPeer(conn, (struct sockaddr *)&(sockPtr->sa));

    /*
     * Get the request data from the reqPtr to ease life-time management in
     * connection threads. It would be probably sufficient to clear just the
     * request line, but we want to play it safe and clear everything.
     */
    connPtr->request = connPtr->reqPtr->request;
    memset(&(connPtr->reqPtr->request), 0, sizeof(struct Ns_Request));

    /*
      Ns_Log(Notice, "ConnRun connPtr %p req %p %s", connPtr, connPtr->request, connPtr->request.line);
    */
    (void) Ns_SetRecreate2(&connPtr->headers, connPtr->reqPtr->headers);

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
    ns_uint64toa(&connPtr->idstr[3], (uint64_t)connPtr->id);

    connPtr->outputheaders = Ns_SetCreate(NULL);
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
    auth = Ns_SetIGet(connPtr->headers, "authorization");
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
    } else if ((connPtr->request.protocol != NULL) && (connPtr->request.host != NULL)) {
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

        if (status == NS_OK) {
            status = Ns_AuthorizeRequest(servPtr->server,
                                         connPtr->request.method,
                                         connPtr->request.url,
                                         Ns_ConnAuthUser(conn),
                                         Ns_ConnAuthPasswd(conn),
                                         Ns_ConnPeerAddr(conn));
            switch (status) {
            case NS_OK:
                status = NsRunFilters(conn, NS_FILTER_POST_AUTH);
                Ns_GetTime(&connPtr->filterDoneTime);
                if (status == NS_OK) {
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

            case NS_ERROR:          NS_FALL_THROUGH; /* fall through */
            case NS_FILTER_BREAK:   NS_FALL_THROUGH; /* fall through */
            case NS_FILTER_RETURN:  NS_FALL_THROUGH; /* fall through */
            case NS_TIMEOUT:
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

    /*
     * Update runtime statistics to make these usable for traces (e.g. access log).
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
    if ((sockPtr->keep) && (connPtr->reqPtr->leftover > 0u)) {
        NsWakeupDriver(sockPtr->drvPtr);
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

    if (connPtr->request.line != NULL) {
        /*
         * reqPtr is freed by FreeRequest() in the driver.
         */
        Ns_ResetRequest(&connPtr->request);
        assert(connPtr->request.line == NULL);
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
    int i;

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
        argPtr->cond = NULL;

        Ns_ThreadCreate(NsConnThread, argPtr, 0, &thread);
    } else {
        Ns_MutexUnlock(&poolPtr->tqueue.lock);

        Ns_MutexLock(&poolPtr->threads.lock);
        poolPtr->threads.current --;
        poolPtr->threads.creating --;
        Ns_MutexUnlock(&poolPtr->threads.lock);

        Ns_Log(Notice, "Cannot create connection thread, all available slots (%d) are used\n", i);
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
     * In the state "waiting", we have never a connPtr->reqPtr, therefore we
     * can't even determine the peer address, nor the request method or the
     * request URL. Furthermore, there is no way to honor the "checkforproxy"
     * flag.
     */
    if (connPtr != NULL) {
        Tcl_DStringStartSublist(dsPtr);

        if (connPtr->reqPtr != NULL) {
            Tcl_DStringAppendElement(dsPtr, connPtr->idstr);

            /*
             * The settings of (connPtr->flags & NS_CONN_CONFIGURED) is
             * protected via the mutex connPtr->poolPtr->tqueue.lock from the
             * caller, so the protected members can't be changed from another
             * thread.
             */
            if ((connPtr->flags & NS_CONN_CONFIGURED) != 0u) {
                const char *p;

                if ( checkforproxy ) {
                    /*
                     * When the connection is NS_CONN_CONFIGURED, the headers
                     * have to be always set.
                     */
                    assert(connPtr->headers != NULL);
                    p = Ns_SetIGet(connPtr->headers, "X-Forwarded-For");

                    if (p == NULL || (*p == '\0') || strcasecmp(p, "unknown") == 0) {
                        /*
                         * Lookup of header field failed, use upstream peer
                         * address.
                         */
                        p = Ns_ConnPeerAddr((const Ns_Conn *) connPtr);
                    }
                } else {
                    p = Ns_ConnPeerAddr((const Ns_Conn *) connPtr);
                }
                Tcl_DStringAppendElement(dsPtr, p);
            } else {
                /*
                 * The request is not configured, the headers might not be
                 * fully processed. In this situation we can determine the
                 * peer address, but not the header fields.
                 */
                if (checkforproxy ) {
                    /*
                     * The user requested "checkforproxy", but we can't. Since
                     * we assume that the user uses this option typically when
                     * running behind a proxy, we do not want to return here
                     * the peer address, which might be incorrect. So we
                     * append "unknown" as in other semi-processed cases.
                     */
                    Ns_Log(Notice, "Connection is not configured, we can't check for the proxy yet");
                    Tcl_DStringAppendElement(dsPtr, "unknown");
                } else {
                    /*
                     * Append the peer address, which is part of the reqPtr
                     * and unrelated with the configured state.
                     */
                    Tcl_DStringAppendElement(dsPtr, Ns_ConnPeerAddr((const Ns_Conn *) connPtr));
                }
            }
        } else {
            /*
             * connPtr->reqPtr == NULL. Having no connPtr->reqPtr is normal
             * for "queued" requests but not for "running" requests. Report this in the error log.
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
        Ns_DStringNAppend(dsPtr, " ", 1);
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
