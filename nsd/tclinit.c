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
 * tclinit.c --
 *
 *      Initialization and resource management routines for Tcl.
 */

#include "nsd.h"

/*
 * The following structure maintains interp trace callbacks.
 */

typedef struct TclTrace {
    struct TclTrace    *nextPtr;
    struct TclTrace    *prevPtr;
    Ns_TclTraceProc    *proc;
    const void         *arg;
    Ns_TclTraceType     when;
} TclTrace;

/*
 * The following structure maintains procs to call during interp garbage
 * collection.  Unlike traces, these callbacks are one-shot events
 * registered during normal Tcl script evaluation. The callbacks are
 * invoked in FIFO order (LIFO would probably have been better). In
 * practice this API is rarely used. Instead, more specific garbage
 * collection schemes are used; see the "ns_cleanup" script in init.tcl
 * for examples.
 */

typedef struct Defer {
    struct Defer    *nextPtr;
    Ns_TclDeferProc *proc;
    void            *arg;
} Defer;

/*
 * The following structure maintains scripts to execute when the
 * connection is closed.  The scripts are invoked in LIFO order.
 */

typedef struct AtClose {
    struct AtClose *nextPtr;
    Tcl_Obj        *objPtr;
} AtClose;

static Ns_ObjvTable traceWhen[] = {
    {"allocate",   (unsigned int)NS_TCL_TRACE_ALLOCATE},
    {"create",     (unsigned int)NS_TCL_TRACE_CREATE},
    {"deallocate", (unsigned int)NS_TCL_TRACE_DEALLOCATE},
    {"delete",     (unsigned int)NS_TCL_TRACE_DELETE},
    {"freeconn",   (unsigned int)NS_TCL_TRACE_FREECONN},
    {"getconn",    (unsigned int)NS_TCL_TRACE_GETCONN},
    {NULL,         (unsigned int)0}
};


/*
 * Static functions defined in this file.
 */

static NsInterp *PopInterp(NsServer *servPtr, Tcl_Interp *interp)
    NS_GNUC_RETURNS_NONNULL;

static void PushInterp(NsInterp *itPtr)
    NS_GNUC_NONNULL(1);

static Tcl_HashEntry *GetCacheEntry(const NsServer *servPtr)
    NS_GNUC_RETURNS_NONNULL;

static Tcl_Interp *CreateInterp(NsInterp **itPtrPtr, NsServer *servPtr)
    NS_GNUC_NONNULL(1)
    NS_GNUC_RETURNS_NONNULL;

static NsInterp *NewInterpData(Tcl_Interp *interp, NsServer *servPtr)
    NS_GNUC_NONNULL(1);

static int UpdateInterp(NsInterp *itPtr)
    NS_GNUC_NONNULL(1);

static void RunTraces(const NsInterp *itPtr, Ns_TclTraceType why)
    NS_GNUC_NONNULL(1);

static void LogTrace(const NsInterp *itPtr, const TclTrace *tracePtr, Ns_TclTraceType why)
    NS_GNUC_NONNULL(1);

static Ns_ReturnCode RegisterAt(Ns_TclTraceProc *proc, const void *arg, Ns_TclTraceType when)
    NS_GNUC_NONNULL(1);

static Tcl_InterpDeleteProc FreeInterpData;
static Ns_TlsCleanup DeleteInterps;
static Ns_ServerInitProc ConfigServerTcl;

static int ICtlAddTrace(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv,  Ns_TclTraceType when);

static Tcl_ObjCmdProc ICtlAddModuleObjCmd;
static Tcl_ObjCmdProc ICtlCleanupObjCmd;
static Tcl_ObjCmdProc ICtlEpochObjCmd;
static Tcl_ObjCmdProc ICtlGetModulesObjCmd;
static Tcl_ObjCmdProc ICtlGetObjCmd;
static Tcl_ObjCmdProc ICtlGetTracesObjCmd;
static Tcl_ObjCmdProc ICtlMarkForDeleteObjCmd;
static Tcl_ObjCmdProc ICtlOnCleanupObjCmd;
static Tcl_ObjCmdProc ICtlOnCreateObjCmd;
static Tcl_ObjCmdProc ICtlOnDeleteObjCmd;
static Tcl_ObjCmdProc ICtlRunTracesObjCmd;
static Tcl_ObjCmdProc ICtlSaveObjCmd;
static Tcl_ObjCmdProc ICtlTraceObjCmd;
static Tcl_ObjCmdProc ICtlUpdateObjCmd;

/*
 * Static variables defined in this file.
 */

static Ns_Tls tls;  /* Slot for per-thread Tcl interp cache. */
static Ns_Mutex interpLock = NULL; 
static bool concurrent_interp_create = NS_FALSE;


/*
 *----------------------------------------------------------------------
 *
 * Nsd_Init --
 *
 *      Init routine called when libnsd is loaded via the Tcl
 *      load command.
 *
 * Results:
 *      Always TCL_OK.
 *
 * Side effects:
 *      See Ns_TclInit.
 *
 *----------------------------------------------------------------------
 */

int
Nsd_Init(Tcl_Interp *interp)
{
    NS_NONNULL_ASSERT(interp != NULL);

    return Ns_TclInit(interp);
}


/*
 *----------------------------------------------------------------------
 *
 * NsConfigTcl --
 *
 *      Allow configuration of Tcl-specific parameters via the config file.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Setting static configuration variable.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigTcl(void)
{
    concurrent_interp_create = Ns_ConfigBool(NS_CONFIG_PARAMETERS, "concurrentinterpcreate", NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitTcl --
 *
 *      Initialize the Tcl interp interface.
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
NsInitTcl(void)
{
    Ns_MutexInit(&interpLock);
    Ns_MutexSetName(&interpLock, "interp");
    /*
     * Allocate the thread storage slot for the table of interps
     * per-thread. At thread exit, DeleteInterps will be called
     * to free any interps remaining on the thread cache.
     */

    Ns_TlsAlloc(&tls, DeleteInterps);

    NsRegisterServerInit(ConfigServerTcl);
}

static Ns_ReturnCode
ConfigServerTcl(const char *server)
{
    NsServer   *servPtr;
    Ns_DString  ds;
    const char *path, *p, *initFileString;
    int         n;
    Ns_Set     *set;

    NS_NONNULL_ASSERT(server != NULL);

    servPtr = NsGetServer(server);
    assert(servPtr != NULL);

    path = Ns_ConfigGetPath(server, NULL, "tcl", (char *)0);
    set = Ns_ConfigCreateSection(path);

    Ns_DStringInit(&ds);

    servPtr->tcl.library = Ns_ConfigString(path, "library", "modules/tcl");
    if (Ns_PathIsAbsolute(servPtr->tcl.library) == NS_FALSE) {
        Ns_HomePath(&ds, servPtr->tcl.library, (char *)0);
        servPtr->tcl.library = Ns_DStringExport(&ds);
	Ns_SetUpdate(set, "library", servPtr->tcl.library);
    }

    initFileString = Ns_ConfigString(path, "initfile", "bin/init.tcl");
    if (Ns_PathIsAbsolute(initFileString) == NS_FALSE) {
        Ns_HomePath(&ds, initFileString, (char *)0);
        initFileString = Ns_DStringExport(&ds);
	Ns_SetUpdate(set, "initfile", initFileString);
    }
    servPtr->tcl.initfile = Tcl_NewStringObj(initFileString, -1);
    Tcl_IncrRefCount(servPtr->tcl.initfile);
    
    servPtr->tcl.modules = Tcl_NewObj();
    Tcl_IncrRefCount(servPtr->tcl.modules);

    Ns_RWLockInit(&servPtr->tcl.lock);
    Ns_MutexInit(&servPtr->tcl.cachelock);
    Ns_MutexSetName2(&servPtr->tcl.cachelock, "ns:tcl.cache", server);
    Tcl_InitHashTable(&servPtr->tcl.caches, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.runTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.synch.mutexTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.synch.csTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.synch.semaTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.synch.condTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.synch.rwTable, TCL_STRING_KEYS);

    servPtr->nsv.nbuckets = Ns_ConfigIntRange(path, "nsvbuckets", 8, 1, INT_MAX);
    servPtr->nsv.buckets = NsTclCreateBuckets(server, servPtr->nsv.nbuckets);

    /*
     * Initialize the list of connection headers to log for Tcl errors.
     */

    p = Ns_ConfigGetValue(path, "errorlogheaders");
    if (p != NULL 
	&& Tcl_SplitList(NULL, p, &n, &servPtr->tcl.errorLogHeaders) != TCL_OK) {
        Ns_Log(Error, "config: errorlogheaders is not a list: %s", p);
    }

    /*
     * Initialize the Tcl detached channel support.
     */

    Tcl_InitHashTable(&servPtr->chans.table, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->chans.lock, "nstcl:chans", server);

    Tcl_InitHashTable(&servPtr->connchans.table, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->connchans.lock, "nstcl:connchans", server);
    
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclCreateInterp --
 *
 *      Create a new interp with basic commands.
 *
 * Results:
 *      Pointer to new interp.
 *
 * Side effects:
 *      Depends on Tcl library init scripts.
 *
 *----------------------------------------------------------------------
 */

Tcl_Interp *
Ns_TclCreateInterp(void)
{
    return NsTclAllocateInterp(NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclInit --
 *
 *      Initialize the given interp with basic commands.
 *
 * Results:
 *      Always TCL_OK.
 *
 * Side effects:
 *      Depends on Tcl library init scripts.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclInit(Tcl_Interp *interp)
{
    NsServer *servPtr = NsGetServer(NULL);

    NS_NONNULL_ASSERT(interp != NULL);

    /* 
     * Associate the the interp data with the current interpreter.
     */
    (void)NewInterpData(interp, servPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclEval --
 *
 *      Execute a tcl script in the context of the given server.
 *
 * Results:
 *      NaviServer result code. String result or error placed in dsPtr if
 *      dsPtr is not NULL.
 *
 * Side effects:
 *      Tcl interp may be allocated, initialized and cached if none
 *      available.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclEval(Ns_DString *dsPtr, const char *server, const char *script)
{
    Tcl_Interp   *interp;
    Ns_ReturnCode status = NS_ERROR;

    NS_NONNULL_ASSERT(script != NULL);

    interp = Ns_TclAllocateInterp(server);
    if (interp != NULL) {
        const char *result;

        if (Tcl_EvalEx(interp, script, -1, 0) != TCL_OK) {
            result = Ns_TclLogErrorInfo(interp, NULL);
        } else {
            result = Tcl_GetStringResult(interp);
            status = NS_OK;
        }
        if (dsPtr != NULL) {
            Ns_DStringAppend(dsPtr, result);
        }
        Ns_TclDeAllocateInterp(interp);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclAllocateInterp, NsTclAllocateInterp --
 *
 *      Return a pre-initialized interp for the given server or create
 *      a new one and cache it for the current thread.
 *
 * Results:
 *      Pointer to Tcl_Interp or NULL if invalid server.
 *
 * Side effects:
 *      May invoke alloc and create traces.
 *
 *----------------------------------------------------------------------
 */

Tcl_Interp *
Ns_TclAllocateInterp(const char *server)
{
    Tcl_Interp     *result = NULL;

    /*
     * Verify the server.  NULL (i.e., no server) is valid but
     * a non-null, unknown server is an error.
     */
    if (server == NULL) {
        result = PopInterp(NULL, NULL)->interp;

    } else {
        NsServer  *servPtr = NsGetServer(server);
        if (likely( servPtr != NULL) ) {
            result = PopInterp(servPtr, NULL)->interp;
        }
    }

    return result;
}

Tcl_Interp *
NsTclAllocateInterp(NsServer *servPtr)
{
    const NsInterp *itPtr = PopInterp(servPtr, NULL);

    return itPtr->interp;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclDeAllocateInterp --
 *
 *      Return an interp to the per-thread cache.  If the interp is
 *      associated with a connection, simply adjust the refcnt as
 *      cleanup will occur later when the connection closes.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See notes on garbage collection in PushInterp.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclDeAllocateInterp(Tcl_Interp *interp)
{
    NsInterp *itPtr;

    NS_NONNULL_ASSERT(interp != NULL);

    itPtr = NsGetInterpData(interp);
    if (itPtr == NULL) {
        Ns_Log(Bug, "Ns_TclDeAllocateInterp: no interp data");
        Tcl_DeleteInterp(interp);
    } else if (itPtr->conn == NULL) {
        PushInterp(itPtr);
    } else {
        itPtr->refcnt--;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetConnInterp --
 *
 *      Get an interp for the given connection.  The interp will be
 *      automatically cleaned up at the end of the connection via a
 *      call to NsFreeConnInterp().
 *
 * Results:
 *      Pointer to Tcl_interp.
 *
 * Side effects:
 *      Interp may be allocated, initialized and cached. Interp traces
 *      may run.
 *
 *----------------------------------------------------------------------
 */

Tcl_Interp *
Ns_GetConnInterp(Ns_Conn *conn)
{
    Conn     *connPtr = (Conn *) conn;
    NsInterp *itPtr;

    NS_NONNULL_ASSERT(conn != NULL);

    if (connPtr->itPtr == NULL) {
        itPtr = PopInterp(connPtr->poolPtr->servPtr, NULL);
        itPtr->conn = conn;
        itPtr->nsconn.flags = 0u;
        connPtr->itPtr = itPtr;
        RunTraces(itPtr, NS_TCL_TRACE_GETCONN);
    }
    return connPtr->itPtr->interp;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_FreeConnInterp --
 *
 *      Deprecated.  See: NsFreeConnInterp.
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
Ns_FreeConnInterp(Ns_Conn *UNUSED(conn))
{
    return;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetConn --
 *
 *      Get the Ns_Conn structure associated with an interp.
 *
 * Results:
 *      Pointer to Ns_Conn or NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Conn *
Ns_TclGetConn(Tcl_Interp *interp)
{
    const NsInterp *itPtr;

    NS_NONNULL_ASSERT(interp != NULL);

    itPtr = NsGetInterpData(interp);
    return ((itPtr != NULL) ? itPtr->conn : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclDestroyInterp --
 *
 *      Delete an interp.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on delete traces, if any.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclDestroyInterp(Tcl_Interp *interp)
{
    const NsInterp *itPtr;

    NS_NONNULL_ASSERT(interp != NULL);

    itPtr = NsGetInterpData(interp);
    /*
     * If this is an naviserver interp, clean it up
     */

    if (itPtr != NULL) {
        Tcl_HashTable *tablePtr = Ns_TlsGet(&tls);

        /*
         * Run traces (behaves gracefully, if there is no server
         * associated).
         */
        RunTraces(itPtr, NS_TCL_TRACE_DELETE);

        /*
         * During shutdown, don't fetch entries via GetCacheEntry(),
         * since this function might create new cache entries. Note,
         * that the thread local cache table might contain as well
         * entries with itPtr->servPtr == NULL.
         */
        if (tablePtr != NULL) {
            int ignored;
            Tcl_HashEntry *hPtr;

            /*
             * Make sure to delete the entry in the thread local cache to
             * avoid double frees in DeleteInterps()
             */

            hPtr = Tcl_CreateHashEntry(tablePtr, (char *)itPtr->servPtr, &ignored);
            Tcl_SetHashValue(hPtr, NULL);
        }
    }
    
    /*
     * All other cleanup, including the NsInterp data, if any, will
     * be handled by Tcl's normal delete mechanisms.
     */

    Tcl_DeleteInterp(interp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclMarkForDelete --
 *
 *      Mark the interp to be deleted after next cleanup.  This routine
 *      is useful for destory interps after they've been modified in
 *      weird ways, e.g., by the TclPro debugger.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Interp will be deleted on next de-allocate.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclMarkForDelete(Tcl_Interp *interp)
{
    NsInterp *itPtr;

    NS_NONNULL_ASSERT(interp != NULL);

    itPtr = NsGetInterpData(interp);
    if (itPtr != NULL) {
        itPtr->deleteInterp = NS_TRUE;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclRegisterTrace --
 *
 *      Add an interp trace.  Traces are called in FIFO order.  Valid
 *      traces are: NS_TCL_TRACE... CREATE, DELETE, ALLOCATE,
 *      DEALLOCATE, GETCONN, and FREECONN.
 *
 * Results:
 *      NS_OK if called with a non-NULL server before startup has
 *      completed, NS_ERROR otherwise.
 *
 * Side effects:
 *      CREATE and ALLOCATE traces are run immediately in the current
 *      interp (the initial bootstrap interp).
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclRegisterTrace(const char *server, Ns_TclTraceProc *proc,
                    const void *arg, Ns_TclTraceType when)
{

    NsServer      *servPtr;
    Ns_ReturnCode  status = NS_OK;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        Ns_Log(Error, "Ns_TclRegisterTrace: Invalid server: %s", server);
        status = NS_ERROR;
        
    } else if (Ns_InfoStarted()) {
        Ns_Log(Error, "Can not register Tcl trace, server already started.");
        status = NS_ERROR;

    } else {
        TclTrace  *tracePtr = ns_malloc(sizeof(TclTrace));
        
        tracePtr->proc = proc;
        tracePtr->arg = arg;
        tracePtr->when = when;
        tracePtr->nextPtr = NULL;

        tracePtr->prevPtr = servPtr->tcl.lastTracePtr;
        servPtr->tcl.lastTracePtr = tracePtr;
        if (tracePtr->prevPtr != NULL) {
            tracePtr->prevPtr->nextPtr = tracePtr;
        } else {
            servPtr->tcl.firstTracePtr = tracePtr;
        }

        /*
         * Run CREATE and ALLOCATE traces immediately so that commands registered
         * by binary modules can be called by Tcl init scripts sourced by the
         * already initialised interp which loads the modules.
         */

        if ((when == NS_TCL_TRACE_CREATE) || (when == NS_TCL_TRACE_ALLOCATE)) {
            Tcl_Interp *interp = NsTclAllocateInterp(servPtr);

            if ((*proc)(interp, arg) != TCL_OK) {
                (void) Ns_TclLogErrorInfo(interp, "\n(context: register trace)");
            }
            Ns_TclDeAllocateInterp(interp);
        }
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclRegisterAtCreate, Ns_TclRegisterAtCleanup,
 * Ns_TclRegisterAtDelete --
 *
 *      Register callbacks for interp create, cleanup, and delete at
 *      startup.  These routines are deprecated in favor of the more
 *      general Ns_TclRegisterTrace. In particular, they do not take a
 *      virtual server argument so must assume the currently
 *      initializing server is the intended server.
 *
 *      Deprecated.
 *
 * Results:
 *      See Ns_TclRegisterTrace.
 *
 * Side effects:
 *      See Ns_TclRegisterTrace.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclRegisterAtCreate(Ns_TclTraceProc *proc, const void *arg)
{
    return RegisterAt(proc, arg, NS_TCL_TRACE_CREATE);
}

Ns_ReturnCode
Ns_TclRegisterAtCleanup(Ns_TclTraceProc *proc, const void *arg)
{
    return RegisterAt(proc, arg, NS_TCL_TRACE_DEALLOCATE);
}

Ns_ReturnCode
Ns_TclRegisterAtDelete(Ns_TclTraceProc *proc, const void *arg)
{
    return RegisterAt(proc, arg, NS_TCL_TRACE_DELETE);
}

static Ns_ReturnCode
RegisterAt(Ns_TclTraceProc *proc, const void *arg, Ns_TclTraceType when)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status;

    NS_NONNULL_ASSERT(proc != NULL);

    servPtr = NsGetInitServer();
    if (servPtr == NULL) {
        status = NS_ERROR;
    } else {
        status = Ns_TclRegisterTrace(servPtr->server, proc, arg, when);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclInitInterps --
 *
 *      Arrange for the given proc to be called on newly created
 *      interps.  This routine now simply uses the more general Tcl
 *      interp tracing facility.  Earlier versions would invoke the
 *      given proc immediately on each interp in a shared pool which
 *      explains this otherwise misnamed API.
 *
 *      Deprecated.
 *
 * Results:
 *      See Ns_TclRegisterTrace.
 *
 * Side effects:
 *      See Ns_TclRegisterTrace.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclInitInterps(const char *server, Ns_TclInterpInitProc *proc, const void *arg)
{
    return Ns_TclRegisterTrace(server, proc, arg, NS_TCL_TRACE_CREATE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclRegisterDeferred --
 *
 *      Register a procedure to be called when the interp is deallocated.
 *      This is a one-shot FIFO order callback mechanism which is seldom
 *      used.
 *
 *      Deprecated.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Procedure will be called later.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclRegisterDeferred(Tcl_Interp *interp, Ns_TclDeferProc *proc, void *arg)
{
    NsInterp   *itPtr = NsGetInterpData(interp);

    if (itPtr != NULL) {
        Defer *deferPtr, **nextPtrPtr;

        deferPtr = ns_malloc(sizeof(Defer));
        deferPtr->proc = proc;
        deferPtr->arg = arg;
        deferPtr->nextPtr = NULL;
        nextPtrPtr = &itPtr->firstDeferPtr;
        while (*nextPtrPtr != NULL) {
            nextPtrPtr = &((*nextPtrPtr)->nextPtr);
        }
        *nextPtrPtr = deferPtr;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLibrary --
 *
 *      Return the name of the private tcl lib if configured, or the
 *      global shared library otherwise.
 *
 * Results:
 *      Tcl lib name.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_TclLibrary(const char *server)
{
    const NsServer *servPtr = NsGetServer(server);

    return ((servPtr != NULL) ? servPtr->tcl.library : nsconf.tcl.sharedlibrary);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclInterpServer --
 *
 *      Return the name of the server.
 *
 * Results:
 *      Server name, or NULL if not a server interp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_TclInterpServer(Tcl_Interp *interp)
{
    const NsInterp *itPtr;
    const char     *result = NULL;

    NS_NONNULL_ASSERT(interp != NULL);

    itPtr = NsGetInterpData(interp);
    if (itPtr != NULL && itPtr->servPtr != NULL) {
        result = itPtr->servPtr->server;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclInitModule --
 *
 *      Add a module name to the init list.
 *
 * Results:
 *      NS_ERROR if no such server, NS_OK otherwise.
 *
 * Side effects:
 *      Module will be initialized by the init script later.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclInitModule(const char *server, const char *module)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(module != NULL);

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        status = NS_ERROR;
    } else {
        (void) Tcl_ListObjAppendElement(NULL, servPtr->tcl.modules,
                                        Tcl_NewStringObj(module, -1));
        status = NS_OK;
    }
    return status;
}



/*
 *----------------------------------------------------------------------
 *
 * ICtlAddTrace 
 *
 *      Helper functin for various trace commands
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Adding a trace on success.
 *
 *----------------------------------------------------------------------
 */

static int
ICtlAddTrace(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv,  Ns_TclTraceType when)
{
    unsigned int    flags = 0u;
    Tcl_Obj        *scriptObj;
    int             remain = 0, result = TCL_OK;
    Ns_ReturnCode   status;
    Ns_ObjvSpec     addTraceArgs[] = {
        {"when",       Ns_ObjvFlags,  &flags,     traceWhen},
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?args",      Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     legacyAddTraceArgs[] = {
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?args",      Ns_ObjvArgs,   &remain,    NULL},
    };

    if (when == NS_TCL_TRACE_NONE) {
        status = Ns_ParseObjv(NULL, addTraceArgs, interp, 2, objc, objv);
    } else {
        status = Ns_ParseObjv(NULL, legacyAddTraceArgs, interp, 2, objc, objv);        
    }
    if (status != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp  *itPtr = clientData;
        const NsServer  *servPtr = itPtr->servPtr;

        if (servPtr != NsGetInitServer()) {
            Ns_TclPrintfResult(interp, "cannot add module after server startup");
            result = TCL_ERROR;
            
        } else {
            const Ns_TclCallback *cbPtr;

            /*
             * When NS_TCL_TRACE_NONE was provide, get the value from the
             * parsed flags.
             */
            if (when == NS_TCL_TRACE_NONE) {
                when  = (Ns_TclTraceType)flags;
            }
            cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *)NsTclTraceProc, 
                                      scriptObj, remain, objv + (objc - remain));
            if (Ns_TclRegisterTrace(servPtr->server, NsTclTraceProc, cbPtr, when) != NS_OK) {
                result = TCL_ERROR;
            }
        }
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * ICtlAddModuleObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl addmodule" command.
 *      Add a Tcl module to the list for later initialization.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Add module.
 *
 *----------------------------------------------------------------------
 */
static int
ICtlAddModuleObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    const NsServer *servPtr = itPtr->servPtr;
    Tcl_Obj        *moduleObj;
    int             result = TCL_OK;
    Ns_ObjvSpec     args[] = {
        {"module",     Ns_ObjvObj,  &moduleObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else if (servPtr != NsGetInitServer()) {
        Ns_TclPrintfResult(interp, "cannot add module after server startup");
        result = TCL_ERROR;
        
    } else {
        result = Tcl_ListObjAppendElement(interp, servPtr->tcl.modules, moduleObj);
        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, servPtr->tcl.modules);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ICtlGetObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl get" command.
 *      Get the current init script to evaluate in new interps.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ICtlGetObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        Ns_RWLockRdLock(&servPtr->tcl.lock);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(servPtr->tcl.script, -1));
        Ns_RWLockUnlock(&servPtr->tcl.lock);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ICtlGetModulesObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl getmodules" command.
 *      Return the list of registered modules.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ICtlGetModulesObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    const NsServer *servPtr = itPtr->servPtr;
    int             result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        Tcl_SetObjResult(interp, servPtr->tcl.modules);
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * ICtlEpochObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl epoch" command.
 *      Check the version of this interp against current init script.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ICtlEpochObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int             result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        Ns_RWLockRdLock(&servPtr->tcl.lock);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(servPtr->tcl.epoch));
        Ns_RWLockUnlock(&servPtr->tcl.lock);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ICtlMarkForDeleteObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl markfordelete" command.
 *      The interp will be deleted on next deallocation.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Adding flag to itPtr.
 *
 *----------------------------------------------------------------------
 */

static int
ICtlMarkForDeleteObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp  *itPtr = clientData;
    int        result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        itPtr->deleteInterp = NS_TRUE;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ICtlSaveObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl save" command.
 *      Save the init script.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Save bluprint.
 *
 *----------------------------------------------------------------------
 */
static int
ICtlSaveObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          result = TCL_OK;
    Tcl_Obj     *scriptObj;
    Ns_ObjvSpec  args[] = {
        {"script",     Ns_ObjvObj,  &scriptObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        int             length;
        const char     *script = ns_strdup(Tcl_GetStringFromObj(scriptObj, &length));
        
        Ns_RWLockWrLock(&servPtr->tcl.lock);
        ns_free((char *)servPtr->tcl.script);
        servPtr->tcl.script = script;
        servPtr->tcl.length = length;
        if (++servPtr->tcl.epoch == 0) {
            /* NB: Epoch zero reserved for new interps. */
            ++itPtr->servPtr->tcl.epoch;
        }
        Ns_RWLockUnlock(&servPtr->tcl.lock);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ICtlUpdateObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl update" command.
 *      Check for and process possible change in the init script.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Update blueprint.
 *
 *----------------------------------------------------------------------
 */

static int
ICtlUpdateObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp    *itPtr = clientData;
    int          result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        result = UpdateInterp(itPtr);
    }
    return result;
}    


/*
 *----------------------------------------------------------------------
 *
 * ICtlCleanupObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl cleanup" command.
 *      Invoke the legacy defer callbacks.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Free memory.
 *
 *----------------------------------------------------------------------
 */

static int
ICtlCleanupObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp    *itPtr = clientData;
    int          result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else if (itPtr->firstDeferPtr != NULL) {
        Defer  *deferPtr;

        for (deferPtr = itPtr->firstDeferPtr; deferPtr != NULL; deferPtr = deferPtr->nextPtr) {
            (*deferPtr->proc)(interp, deferPtr->arg);
            ns_free(deferPtr);
        }
	itPtr->firstDeferPtr = NULL;
        
        result = UpdateInterp(itPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ICtlOnInitObjCmd 
 * ICtlOnCreateObjCmd 
 * ICtlOnCleanupObjCmd 
 * ICtlOnDeleteObjCmd 
 * ICtlTraceObjCmd 
 *        - subcommands of NsTclICtlObjCmd --
 *
 *      Implements various trace commands
 *
 *          ns_ictl trace|oninit|oncreate|oncleanup|ondelete
 *
 *      Register script-level interp traces. "ns_ictl trace" is the new
 *      version, the other ones are deprecated 3-argument variants.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Adding a trace on success.
 *
 *----------------------------------------------------------------------
 */

static int
ICtlOnCreateObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_LogDeprecated(objv, 2, "ns_ictl trace create ...", NULL);
    return ICtlAddTrace(clientData, interp, objc, objv, NS_TCL_TRACE_CREATE);
}
static int
ICtlOnCleanupObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_LogDeprecated(objv, 2, "ns_ictl trace deallocate ...", NULL);
    return ICtlAddTrace(clientData, interp, objc, objv, NS_TCL_TRACE_DEALLOCATE);
}
static int
ICtlOnDeleteObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_LogDeprecated(objv, 2, "ns_ictl trace delete ...", NULL);
    return ICtlAddTrace(clientData, interp, objc, objv, NS_TCL_TRACE_DELETE);
}
static int
ICtlTraceObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    /* 
     * Passing NS_TCL_TRACE_NONE as last argument means to get the trace type
     * from the passed-in value 
     */
    return ICtlAddTrace(clientData, interp, objc, objv, NS_TCL_TRACE_NONE);
}


/*
 *----------------------------------------------------------------------
 *
 * ICtlGetTracesObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl gettraces" command.
 *      Return the script of the specified trace.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ICtlGetTracesObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int             result = TCL_OK;
    unsigned int    flags = 0u;
    Ns_ObjvSpec     args[] = {
        {"when", Ns_ObjvFlags,  &flags, traceWhen},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp  *itPtr = clientData;
        const NsServer  *servPtr = itPtr->servPtr;
        Ns_DString       ds;
        const TclTrace  *tracePtr;
        Ns_TclTraceType  when = (Ns_TclTraceType)flags;
        
        Ns_DStringInit(&ds);
        for (tracePtr = servPtr->tcl.firstTracePtr;
             (tracePtr != NULL);
             tracePtr = tracePtr->nextPtr) {
            if (tracePtr->when == when) {
                Ns_GetProcInfo(&ds, (Ns_Callback *)tracePtr->proc, tracePtr->arg);
            }
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ICtlRunTracesObjCmd - subcommand of NsTclICtlObjCmd --
 *
 *      Implements "ns_ictl runtraces" command.
 *      Run the specified trace.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ICtlRunTracesObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int             result = TCL_OK;
    unsigned int    flags = 0u;
    Ns_ObjvSpec     args[] = {
        {"when", Ns_ObjvFlags,  &flags, traceWhen},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        
        RunTraces(itPtr, (Ns_TclTraceType)flags);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclICtlObjCmd --
 *
 *      Implements ns_ictl command to control interp state for
 *      virtual server interps.  This command provide internal control
 *      functions required by the init.tcl script and is not intended
 *      to be called by a user directly.  It supports four activities:
 *
 *      1. Managing the list of "modules" to initialize.
 *      2. Saving the init script for evaluation with new interps.
 *      3. Checking for change of the init script.
 *      4. Register script-level traces.
 *
 *      See init.tcl for details.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Depends on the subcommand.
 *
 *----------------------------------------------------------------------
 */

int
NsTclICtlObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"addmodule",     ICtlAddModuleObjCmd},
        {"cleanup",       ICtlCleanupObjCmd},
        {"epoch",         ICtlEpochObjCmd},
        {"get",           ICtlGetObjCmd},
        {"getmodules",    ICtlGetModulesObjCmd},
        {"gettraces",     ICtlGetTracesObjCmd},
        {"markfordelete", ICtlMarkForDeleteObjCmd},
        {"oncleanup",     ICtlOnCleanupObjCmd},
        {"oncreate",      ICtlOnCreateObjCmd},
        {"ondelete",      ICtlOnDeleteObjCmd},
        {"oninit",        ICtlOnCreateObjCmd},
        {"runtraces",     ICtlRunTracesObjCmd},
        {"save",          ICtlSaveObjCmd},
        {"trace",         ICtlTraceObjCmd},
        {"update",        ICtlUpdateObjCmd},
        {NULL, NULL}
    };
    
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAtCloseObjCmd --
 *
 *      Implements ns_atclose.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Script will be invoked when the connection is closed.  Note
 *      the connection may continue execution, e.g., with continued
 *      ADP code, traces, etc.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAtCloseObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp  *itPtr = clientData;
    AtClose   *atPtr;
    int        result = TCL_OK;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?args?");
        result = TCL_ERROR;

    } else if (itPtr->conn == NULL) {
        Ns_TclPrintfResult(interp, "no connection");

    } else {
    
        atPtr = ns_malloc(sizeof(AtClose));
        atPtr->nextPtr = itPtr->firstAtClosePtr;
        itPtr->firstAtClosePtr = atPtr;
        atPtr->objPtr = Tcl_ConcatObj(objc-1, objv+1);
        Tcl_IncrRefCount(atPtr->objPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRunAtClose --
 *
 *      Run and then free any registered connection at-close scripts.
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
NsTclRunAtClose(NsInterp *itPtr)
{
    Tcl_Interp  *interp;
    AtClose     *atPtr, *nextPtr;

    NS_NONNULL_ASSERT(itPtr != NULL);

    interp = itPtr->interp;

    for (atPtr = itPtr->firstAtClosePtr; atPtr != NULL; atPtr = nextPtr) {
        assert(atPtr->objPtr != NULL);
        if (Tcl_EvalObjEx(interp, atPtr->objPtr, TCL_EVAL_DIRECT) != TCL_OK) {
            (void) Ns_TclLogErrorInfo(interp, "\n(context: at close)");
        }
        Tcl_DecrRefCount(atPtr->objPtr);
        nextPtr = atPtr->nextPtr;
        ns_free(atPtr);
    }
    itPtr->firstAtClosePtr = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclInitServer --
 *
 *      Evaluate server initialization script at startup.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on init script (normally init.tcl).
 *
 *----------------------------------------------------------------------
 */

void
NsTclInitServer(const char *server)
{
    NsServer *servPtr;
    
    NS_NONNULL_ASSERT(server != NULL); 

    servPtr = NsGetServer(server);
    if (servPtr != NULL) {
	Tcl_Interp *interp = NsTclAllocateInterp(servPtr);

        if ( Tcl_FSEvalFile(interp, servPtr->tcl.initfile) != TCL_OK) {
            (void) Ns_TclLogErrorInfo(interp, "\n(context: init server)");
        }
        Ns_TclDeAllocateInterp(interp);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclAppInit --
 *
 *      Initialize an interactive command interp with basic and
 *      server commands using the default virtual server.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Override Tcl exit command so that propper server shutdown
 *      takes place.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAppInit(Tcl_Interp *interp)
{
    NsServer  *servPtr;
    int        result = TCL_OK;

    servPtr = NsGetServer(nsconf.defaultServer);
    if (servPtr == NULL) {
        Ns_Log(Bug, "NsTclAppInit: invalid default server: %s",
               nsconf.defaultServer);
        result = TCL_ERROR;

    } else if (Tcl_Init(interp) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        (void) Tcl_SetVar(interp, "tcl_rcFileName", "~/.nsdrc", TCL_GLOBAL_ONLY);
        (void) Tcl_Eval(interp, "proc exit {} ns_shutdown");
        (void) PopInterp(servPtr, interp);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetInterpData --
 *
 *      Return the interp's NsInterp structure from assoc data.
 *      This routine is used when the NsInterp is needed and
 *      not available as command ClientData.
 *
 * Results:
 *      Pointer to NsInterp or NULL if none.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NsInterp *
NsGetInterpData(Tcl_Interp *interp)
{
    NS_NONNULL_ASSERT(interp != NULL);
    return Tcl_GetAssocData(interp, "ns:data", NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsFreeConnInterp --
 *
 *      Free the interp data, if any, for given connection.  This
 *      routine is called at the end of connection processing.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See PushInterp.
 *
 *----------------------------------------------------------------------
 */

void
NsFreeConnInterp(Conn *connPtr)
{
    NsInterp *itPtr = connPtr->itPtr;

    if (itPtr != NULL) {
        RunTraces(itPtr, NS_TCL_TRACE_FREECONN);
        itPtr->conn = NULL;
        itPtr->nsconn.flags = 0u;
        PushInterp(itPtr);
        connPtr->itPtr = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclTraceProc --
 *
 *      Eval a registered Tcl interp trace callback.
 *
 * Results:
 *      Tcl result code from script eval.
 *
 * Side effects:
 *      Depends on script.
 *
 *----------------------------------------------------------------------
 */

int
NsTclTraceProc(Tcl_Interp *interp, const void *arg)
{
    const Ns_TclCallback *cbPtr = arg;
    int                   result;

    result = Ns_TclEvalCallback(interp, cbPtr, NULL, (char *)0);
    if (unlikely(result != TCL_OK)) {
        (void) Ns_TclLogErrorInfo(interp, "\n(context: trace proc)");
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * PopInterp --
 *
 *      Get virtual-server interp from the per-thread cache and
 *      increment the reference count.  Allocate a new interp if
 *      necessary.
 *
 * Results:
 *      NsInterp.
 *
 * Side effects:
 *      Will invoke alloc traces if not recursively allocated and, if
 *      the interp is new, create traces.
 *
 *----------------------------------------------------------------------
 */

static NsInterp *
PopInterp(NsServer *servPtr, Tcl_Interp *interp)
{
    NsInterp      *itPtr;
    Tcl_HashEntry *hPtr;
    static Ns_Cs   lock;

    /*
     * Get an already initialized interp for the given virtual server
     * on this thread.  If it doesn't yet exist, create and
     * initialize one.
     */
    hPtr = GetCacheEntry(servPtr);
    itPtr = Tcl_GetHashValue(hPtr);
    if (itPtr == NULL) {
        if (nsconf.tcl.lockoninit) {
            Ns_CsEnter(&lock);
        }
        if (interp != NULL) {
            itPtr = NewInterpData(interp, servPtr);
        } else {
            interp = CreateInterp(&itPtr, servPtr);
        }
        if (servPtr != NULL) {
            itPtr->servPtr = servPtr;
            NsTclAddServerCmds(itPtr);
            RunTraces(itPtr, NS_TCL_TRACE_CREATE);
            if (UpdateInterp(itPtr) != TCL_OK) {
                (void) Ns_TclLogErrorInfo(interp, "\n(context: update interpreter)");
            }
        } else {
            RunTraces(itPtr, NS_TCL_TRACE_CREATE);
        }
        if (nsconf.tcl.lockoninit) {
            Ns_CsLeave(&lock);
        }
        Tcl_SetHashValue(hPtr, itPtr);
    }

    /*
     * Run allocation traces once.
     */

    if (++itPtr->refcnt == 1) {
        RunTraces(itPtr, NS_TCL_TRACE_ALLOCATE);
    }

    return itPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * PushInterp --
 *
 *      Return a virtual-server interp to the thread cache.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May invoke de-alloc traces, destroy interp if no longer
 *      being used.
 *
 *----------------------------------------------------------------------
 */

static void
PushInterp(NsInterp *itPtr)
{
    Tcl_Interp *interp;
    bool        ok = NS_TRUE;

    NS_NONNULL_ASSERT(itPtr != NULL);
    
    interp = itPtr->interp;

    /*
     * Evaluate the dellocation traces once to perform various garbage
     * collection and then either delete the interp or push it back on the
     * per-thread list.
     */
    if (itPtr->refcnt == 1) {
        RunTraces(itPtr, NS_TCL_TRACE_DEALLOCATE);
        if (itPtr->deleteInterp) {
            Ns_Log(Debug, "ns_markfordelete: true");
            Ns_TclDestroyInterp(interp);
            ok = NS_FALSE;
        }
    }
    if (ok) {
        Tcl_ResetResult(interp);
        itPtr->refcnt--;

        assert(itPtr->refcnt >= 0);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * GetCacheEntry --
 *
 *      Get hash entry in per-thread interp cache for given virtual
 *      server.
 *
 * Results:
 *      Pointer to hash entry.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_HashEntry *
GetCacheEntry(const NsServer *servPtr)
{
    Tcl_HashTable *tablePtr;
    int ignored;

    tablePtr = Ns_TlsGet(&tls);
    if (tablePtr == NULL) {
        tablePtr = ns_malloc(sizeof(Tcl_HashTable));
        Tcl_InitHashTable(tablePtr, TCL_ONE_WORD_KEYS);
        Ns_TlsSet(&tls, tablePtr);
    }
    return Tcl_CreateHashEntry(tablePtr, (char *) servPtr, &ignored);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCreateInterp --
 *
 *      Create a fresh new Tcl interp. The creation is serialized to
 *      prevent concurrent interp creations.
 *
 * Results:
 *      Tcl_Interp pointer.
 *
 * Side effects:
 *      Depends on Tcl library init scripts, errors will be logged.
 *
 *----------------------------------------------------------------------
 */

Tcl_Interp *
NsTclCreateInterp(void) {
    Tcl_Interp *interp;

    if (concurrent_interp_create) {
        interp = Tcl_CreateInterp();
    } else {
        Ns_MutexLock(&interpLock);
        interp = Tcl_CreateInterp();
        Ns_MutexUnlock(&interpLock);
    }
    return interp;
}


/*
 *----------------------------------------------------------------------
 *
 * CreateInterp --
 *
 *      Create a fresh new Tcl interp configured for NaviServer
 *
 * Results:
 *      Tcl_Interp pointer.
 *
 * Side effects:
 *      Depends on Tcl library init scripts, errors will be logged.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Interp *
CreateInterp(NsInterp **itPtrPtr, NsServer *servPtr)
{
    NsInterp   *itPtr;
    Tcl_Interp *interp;

    NS_NONNULL_ASSERT(itPtrPtr != NULL);
    
    /*
     * Create and initialize a basic Tcl interp.
     */

    interp = NsTclCreateInterp();

    Tcl_InitMemory(interp);
    if (Tcl_Init(interp) != TCL_OK) {
        (void) Ns_TclLogErrorInfo(interp, "\n(context: create interpreter)");
    }

    /*
     * Make sure, the system encoding is UTF-8. Changing the system
     * encoding at runtime is a potentially dangerous operation, since
     * tcl might be loading already files based on a previous
     * enconding in another thread. So, we want to perform this
     * operation only once for all threads.
     */
    if (strcmp("utf-8", Tcl_GetEncodingName(Tcl_GetEncoding(interp, NULL))) != 0) {
	int result = Tcl_SetSystemEncoding(interp, "utf-8");
	if (result != TCL_OK) {
	    (void) Ns_TclLogErrorInfo(interp, "\n(context: set system encoding to utf-8)");
	}
    }

    /*
     * Allocate and associate a new NsInterp struct for the interp.
     */

    itPtr = NewInterpData(interp, servPtr);
    *itPtrPtr = itPtr;

    return interp;
}


/*
 *----------------------------------------------------------------------
 *
 * NewInterpData --
 *
 *      Create a new NsInterp struct for the given interp, adding
 *      basic commands and associating it with the interp.
 *
 * Results:
 *      Pointer to new NsInterp struct.
 *
 * Side effects:
 *      Depends on Tcl init script sourced by Tcl_Init.  Some Tcl
 *      object types will be initialized on first call.
 *
 *----------------------------------------------------------------------
 */

static NsInterp *
NewInterpData(Tcl_Interp *interp, NsServer *servPtr)
{
    static volatile int initialized = 0;
    NsInterp *itPtr;

    NS_NONNULL_ASSERT(interp != NULL);

    /*
     * Core one-time server initialization to add a few Tcl_Obj
     * types.  These calls cannot be in NsTclInit above because
     * Tcl is not fully initialized at libnsd load time.
     */

    if (initialized == 0) {
        Ns_MasterLock();
        if (initialized == 0) {
            NsTclInitQueueType();
            NsTclInitAddrType();
            NsTclInitTimeType();
            NsTclInitKeylistType();
            initialized = 1;
        }
        Ns_MasterUnlock();
    }

    /*
     * Allocate and initialize a new NsInterp struct.
     */

    itPtr = NsGetInterpData(interp);
    if (itPtr == NULL) {
        itPtr = ns_calloc(1u, sizeof(NsInterp));
        itPtr->interp = interp;
        itPtr->servPtr = servPtr;
        Tcl_InitHashTable(&itPtr->sets, TCL_STRING_KEYS);
        Tcl_InitHashTable(&itPtr->chans, TCL_STRING_KEYS);
        Tcl_InitHashTable(&itPtr->httpRequests, TCL_STRING_KEYS);
        NsAdpInit(itPtr);

        /*
         * Associate the new NsInterp with this interp.  At interp delete
         * time, Tcl will call FreeInterpData to cleanup the struct.
         */

        Tcl_SetAssocData(interp, "ns:data", FreeInterpData, itPtr);

        /*
         * Add basic commands which function without a virtual server.
         */

        NsTclAddBasicCmds(itPtr);
    }

    return itPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * UpdateInterp --
 *
 *      Update the state of an interp by evaluating the saved script
 *      whenever the epoch changes.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
UpdateInterp(NsInterp *itPtr)
{
    NsServer *servPtr;
    int       result = TCL_OK;

    NS_NONNULL_ASSERT(itPtr != NULL);
    servPtr = itPtr->servPtr;

    /*
     * A reader-writer lock is used on the assumption updates are
     * rare and likley expensive to evaluate if the virtual server
     * contains significant state.
     */

    Ns_RWLockRdLock(&servPtr->tcl.lock);
    if (itPtr->epoch != servPtr->tcl.epoch) {
        result = Tcl_EvalEx(itPtr->interp, servPtr->tcl.script,
                            servPtr->tcl.length, TCL_EVAL_GLOBAL);
        itPtr->epoch = servPtr->tcl.epoch;
    }
    Ns_RWLockUnlock(&servPtr->tcl.lock);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * RunTraces, LogTrace --
 *
 *      Execute interp trace callbacks.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depeneds on callbacks. Event may be logged.
 *
 *----------------------------------------------------------------------
 */

static void
RunTraces(const NsInterp *itPtr, Ns_TclTraceType why)
{
    const TclTrace *tracePtr;
    const NsServer *servPtr;

    NS_NONNULL_ASSERT(itPtr != NULL);
    
    servPtr = itPtr->servPtr;
    if (servPtr != NULL) {

        switch (why) {
        case NS_TCL_TRACE_FREECONN:
        case NS_TCL_TRACE_DEALLOCATE:
        case NS_TCL_TRACE_DELETE:
            /* Run finalization traces in LIFO order. */

            tracePtr = servPtr->tcl.lastTracePtr;
            while (tracePtr != NULL) {
                if (tracePtr->when == why) {
                    LogTrace(itPtr, tracePtr, why);
                    if ((*tracePtr->proc)(itPtr->interp, tracePtr->arg) != TCL_OK) {
                        (void) Ns_TclLogErrorInfo(itPtr->interp, "\n(context: run trace)");
                    }
                }
                tracePtr = tracePtr->prevPtr;
            }
            break;

        case NS_TCL_TRACE_ALLOCATE:
        case NS_TCL_TRACE_CREATE:
        case NS_TCL_TRACE_GETCONN:
            /* Run initialization traces in FIFO order. */

            tracePtr = servPtr->tcl.firstTracePtr;
            while (tracePtr != NULL) {
                if (tracePtr->when == why) {
                    LogTrace(itPtr, tracePtr, why);
                    if ((*tracePtr->proc)(itPtr->interp, tracePtr->arg) != TCL_OK) {
                        (void) Ns_TclLogErrorInfo(itPtr->interp, "\n(context: run trace)");
                    }
                }
                tracePtr = tracePtr->nextPtr;
            }
            break;
            
        case NS_TCL_TRACE_NONE:
            break;
        }
    }
}

static void
LogTrace(const NsInterp *itPtr, const TclTrace *tracePtr, Ns_TclTraceType why)
{
    Ns_DString  ds;

    NS_NONNULL_ASSERT(itPtr != NULL);

    if (Ns_LogSeverityEnabled(Debug)) {
        Ns_DStringInit(&ds);
        switch (why) {
        case NS_TCL_TRACE_CREATE:
            Tcl_DStringAppendElement(&ds, "create");
            break;
        case NS_TCL_TRACE_DELETE:
            Tcl_DStringAppendElement(&ds, "delete");
            break;
        case NS_TCL_TRACE_ALLOCATE:
            Tcl_DStringAppendElement(&ds, "allocate");
            break;
        case NS_TCL_TRACE_DEALLOCATE:
            Tcl_DStringAppendElement(&ds, "deallocate");
            break;
        case NS_TCL_TRACE_GETCONN:
            Tcl_DStringAppendElement(&ds, "getconn");
            break;
        case NS_TCL_TRACE_FREECONN:
            Tcl_DStringAppendElement(&ds, "freeconn");
            break;
        case NS_TCL_TRACE_NONE:
        default:
            /* unexpected value */
            assert(why && 0);
            break;
        }
        Ns_GetProcInfo(&ds, (Ns_Callback *)tracePtr->proc, tracePtr->arg);
        Ns_Log(Debug, "ns:interptrace[%s]: %s",
               itPtr->servPtr->server, Ns_DStringValue(&ds));
        Ns_DStringFree(&ds);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * FreeInterpData --
 *
 *      Tcl assoc data callback to destroy the per-interp NsInterp
 *      structure at interp delete time.
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
FreeInterpData(ClientData clientData, Tcl_Interp *UNUSED(interp))
{
    NsInterp *itPtr = clientData;

    NsAdpFree(itPtr);
    Tcl_DeleteHashTable(&itPtr->sets);
    Tcl_DeleteHashTable(&itPtr->chans);
    Tcl_DeleteHashTable(&itPtr->httpRequests);

    ns_free(itPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * DeleteInterps --
 *
 *      Tls callback to delete all cache virtual-server interps at
 *      thread exit time.
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
DeleteInterps(void *arg)
{
    Tcl_HashTable       *tablePtr = arg;
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;

    hPtr = Tcl_FirstHashEntry(tablePtr, &search);
    while (hPtr != NULL) {
        const NsInterp *itPtr;

        itPtr = Tcl_GetHashValue(hPtr);
        if ((itPtr != NULL) && (itPtr->interp != NULL)) {
	    Ns_TclDestroyInterp(itPtr->interp);
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(tablePtr);
    ns_free(tablePtr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
