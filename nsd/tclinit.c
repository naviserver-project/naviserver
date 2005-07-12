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
 * tclinit.c --
 *
 *      Initialization and resource management routines for Tcl.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


/*
 * The following structure maintains interp trace callbacks.
 */

typedef struct Trace {
    struct Trace       *nextPtr;
    Ns_TclTraceProc    *proc;
    void               *arg;
    int                 when;
} Trace;

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

/*
 * Static functions defined in this file.
 */

static Tcl_Interp *CreateInterp();
static Tcl_InterpDeleteProc FreeInterpData;
static int UpdateInterp(NsInterp *itPtr);
static NsInterp *PopInterp(CONST char *server);
static void PushInterp(NsInterp *itPtr);
static Tcl_HashEntry *GetCacheEntry(NsServer *servPtr);
static Ns_TlsCleanup DeleteInterps;
static void RunTraces(NsInterp *itPtr, int why);
static int RegisterAt(Ns_TclTraceProc *proc, void *arg, int when);

/*
 * Static variables defined in this file.
 */

static Ns_Tls tls;    /* Slot for per-thread Tcl interp cache. */


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
    /*
     * Allocate the thread storage slot for the table of interps
     * per-thread. At thread exit, DeleteInterps will be called
     * to free any interps remaining on the thread cache.
     */

    Ns_TlsAlloc(&tls, DeleteInterps);
}


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
    return Ns_TclInit(interp);
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
    Tcl_Interp *interp;

    interp = CreateInterp();
    (void) NsInitInterp(interp, NULL, NULL);

    return interp;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclInit --
 *
 *      Initialize the given interp with basic commands.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Depends on Tcl library init scripts.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclInit(Tcl_Interp *interp)
{
    return NsInitInterp(interp, NULL, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclEval --
 *
 *      Execute a tcl script in the context of the given server.
 *
 * Results:
 *      Tcl result code. String result or error placed in dsPtr if
 *      not NULL.
 *
 * Side effects:
 *      Tcl interp may be allocated, initialized and cached if none
 *      available.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclEval(Ns_DString *dsPtr, CONST char *server, CONST char *script)
{
    Tcl_Interp *interp;
    CONST char *result;
    int         retcode = NS_ERROR;

    interp = Ns_TclAllocateInterp(server);
    if (interp != NULL) {
        if (Tcl_EvalEx(interp, script, -1, 0) != TCL_OK) {
            result = Ns_TclLogError(interp);
        } else {
            result = Tcl_GetStringResult(interp);
            retcode = NS_OK;
        }
        if (dsPtr != NULL) {
            Ns_DStringAppend(dsPtr, result);
        }
        Ns_TclDeAllocateInterp(interp);
    }
    return retcode;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclAllocateInterp --
 *
 *      Allocate an interpreter from the per-thread list.  Note that a
 *      single thread can have multiple interps for multiple virtual
 *      servers.
 *
 * Results:
 *      Pointer to Tcl_Interp.
 *
 * Side effects:
 *      See PopInterp for details on various traces which may be
 *      called.
 *
 *----------------------------------------------------------------------
 */

Tcl_Interp *
Ns_TclAllocateInterp(CONST char *server)
{
    NsInterp *itPtr;

    itPtr = PopInterp(server);
    return (itPtr ? itPtr->interp : NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclDeAllocateInterp --
 *
 *      Return an interp to the per-thread cache.  If the interp is
 *      associated with a connection, silently do nothing as cleanup
 *      will occur later with connection cleanup.  If the interp is
 *      only a Tcl interp, i.e., missing the NsInterp structure,
 *      simply delete the interp directly (this is suspect).
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

    itPtr = NsGetInterpData(interp);
    if (itPtr == NULL) {
        Tcl_DeleteInterp(interp);
    } else if (itPtr->conn == NULL) {
        PushInterp(itPtr);
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

    if (connPtr->itPtr == NULL) {
        itPtr = PopInterp(connPtr->server);
        itPtr->conn = conn;
        itPtr->nsconn.flags = 0;
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
Ns_FreeConnInterp(Ns_Conn *conn)
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
    NsInterp *itPtr = NsGetInterpData(interp);

    return (itPtr ? itPtr->conn : NULL);
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
    NsInterp *itPtr = NsGetInterpData(interp);

    /*
     * If this is a server interp, invoke the delete traces.
     */

    if (itPtr != NULL) {
        RunTraces(itPtr, NS_TCL_TRACE_DELETE);
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
    NsInterp *itPtr = NsGetInterpData(interp);

    if (itPtr != NULL) {
        itPtr->delete = 1;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclRegisterTrace --
 *
 *      Add an interp trace.  Traces are called in FIFO order.
 *
 * Results:
 *      NS_OK if called with a non-NULL server before startup has
 *      completed, NS_ERROR otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
 
int
Ns_TclRegisterTrace(CONST char *server, Ns_TclTraceProc *proc,
                    void *arg, int when)
{
    Trace    *tracePtr, **firstPtrPtr;
    NsServer *servPtr;

    servPtr = NsGetServer(server);
    if (servPtr == NULL || Ns_InfoStarted()) {
        return NS_ERROR;
    }
    tracePtr = ns_malloc(sizeof(Trace));
    tracePtr->proc = proc;
    tracePtr->arg = arg;
    tracePtr->when = when;
    tracePtr->nextPtr = NULL;
    firstPtrPtr = &servPtr->tcl.firstTracePtr;
    while (*firstPtrPtr != NULL) {
        firstPtrPtr = &((*firstPtrPtr)->nextPtr);
    }
    *firstPtrPtr = tracePtr;

    return NS_OK;
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
 * Results:
 *      See Ns_TclRegisterTrace.
 *
 * Side effects:
 *      See Ns_TclRegisterTrace.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclRegisterAtCreate(Ns_TclTraceProc *proc, void *arg)
{
    return RegisterAt(proc, arg, NS_TCL_TRACE_CREATE);
}

int
Ns_TclRegisterAtCleanup(Ns_TclTraceProc *proc, void *arg)
{
    return RegisterAt(proc, arg, NS_TCL_TRACE_DEALLOCATE);
}

int
Ns_TclRegisterAtDelete(Ns_TclTraceProc *proc, void *arg)
{
    return RegisterAt(proc, arg, NS_TCL_TRACE_DELETE);
}

static int
RegisterAt(Ns_TclTraceProc *proc, void *arg, int when)
{
    NsServer *servPtr;

    servPtr = NsGetInitServer();
    if (servPtr == NULL) {
        return NS_ERROR;
    }
    return Ns_TclRegisterTrace(servPtr->server, proc, arg, when);
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
 * Results:
 *      See Ns_TclRegisterTrace.
 *
 * Side effects:
 *      See Ns_TclRegisterTrace.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclInitInterps(CONST char *server, Ns_TclInterpInitProc *proc, void *arg)
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
    Defer      *deferPtr, **nextPtrPtr;

    if (itPtr == NULL) {
        return;
    }
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

char *
Ns_TclLibrary(CONST char *server)
{
    NsServer *servPtr = NsGetServer(server);

    return (servPtr ? servPtr->tcl.library : nsconf.tcl.sharedlibrary);
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

char *
Ns_TclInterpServer(Tcl_Interp *interp)
{
    NsInterp *itPtr = NsGetInterpData(interp);
    
    if (itPtr != NULL && itPtr->servPtr != NULL) {
        return itPtr->servPtr->server;
    }
    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogError --
 *
 *      Log the global errorInfo variable to the server log. 
 *
 * Results:
 *      Returns a pointer to the errorInfo. 
 *
 * Side effects:
 *      None. 
 *
 *----------------------------------------------------------------------
 */

char *
Ns_TclLogError(Tcl_Interp *interp)
{
    CONST char *errorInfo;

    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    if (errorInfo == NULL) {
        errorInfo = "";
    }
    Ns_Log(Error, "%s\n%s", Tcl_GetStringResult(interp), errorInfo);
 
   return (char *) errorInfo;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclLogErrorRequest --
 *
 *      Log both errorInfo and info about the HTTP request that led 
 *      to it. 
 *
 * Results:
 *      Returns a pointer to the errorInfo. 
 *
 * Side effects:
 *      None. 
 *
 *----------------------------------------------------------------------
 */

char *
Ns_TclLogErrorRequest(Tcl_Interp *interp, Ns_Conn *conn)
{
    char *agent;
    CONST char *errorInfo;

    errorInfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
    if (errorInfo == NULL) {
        errorInfo = Tcl_GetStringResult(interp);
    }
    agent = Ns_SetIGet(conn->headers, "user-agent");
    if (agent == NULL) {
        agent = "?";
    }
    Ns_Log(Error, "error for %s %s, "
           "User-Agent: %s, PeerAddress: %s\n%s", 
           conn->request->method, conn->request->url,
           agent, Ns_ConnPeer(conn), errorInfo);

    return (char*) errorInfo;
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

int
Ns_TclInitModule(CONST char *server, CONST char *module)
{
    NsServer *servPtr;

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        return NS_ERROR;
    }
    (void) Tcl_ListObjAppendElement(NULL, servPtr->tcl.modules,
                                    Tcl_NewStringObj(module, -1));
    return NS_OK;
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
 *      1. Managing the list of "modules" to initialize.
 *      2. Saving the init script for evaluation with new interps.
 *      3. Checking for change of the init script.
 *      4. Register script-level traces.
 *
 *      See init.tcl for details.
 *
 * Results:
 *      Standar Tcl result.
 *
 * Side effects:
 *      May update current saved server Tcl state.
 *
 *----------------------------------------------------------------------
 */

int
NsTclICtlObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsInterp       *itPtr = arg;
    NsServer       *servPtr = itPtr->servPtr;
    Defer          *deferPtr;
    Ns_TclCallback *cbPtr;
    char           *script, *scriptArg = NULL;
    int             opt, length, when = 0, result = TCL_OK;

    static CONST char *opts[] = {
        "addmodule", "cleanup", "epoch", "get", "getmodules", "save",
        "update", "oncreate", "oncleanup", "oninit", "ondelete", "trace",
        NULL
    };
    enum {
        IAddModuleIdx, ICleanupIdx, IEpochIdx, IGetIdx, IGetModulesIdx,
        ISaveIdx, IUpdateIdx, IOnCreateIdx, IOnCleanupIdx, IOnInitIdx,
        IOnDeleteIdx, ITraceIdx
    };
    Ns_ObjvTable traceWhen[] = {
        {"create",     NS_TCL_TRACE_CREATE},
        {"delete",     NS_TCL_TRACE_DELETE},
        {"allocate",   NS_TCL_TRACE_ALLOCATE},
        {"deallocate", NS_TCL_TRACE_DEALLOCATE},
        {"getconn",    NS_TCL_TRACE_GETCONN},
        {"freeconn",   NS_TCL_TRACE_FREECONN},
        {NULL, 0}
    };
    Ns_ObjvSpec traceArgs[] = {
        {"when",       Ns_ObjvFlags,  &when,      traceWhen},
        {"script",     Ns_ObjvString, &script,    NULL},
        {"?scriptArg", Ns_ObjvString, &scriptArg, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case IAddModuleIdx:
        /*
         * Add a Tcl module to the list for later initialization.
         */

        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "module");
            return TCL_ERROR;
        }
        if (servPtr != NsGetInitServer()) {
            Tcl_SetResult(interp, "cannot add module after server startup",
                          TCL_STATIC);
            return TCL_ERROR;
        }
        if (Tcl_ListObjAppendElement(interp, servPtr->tcl.modules,
                                     objv[2]) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, servPtr->tcl.modules);
        break;

    case IGetModulesIdx:
        /*
         * Get the list of modules for initialization.  See inti.tcl
         * for expected use.
         */

        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        Tcl_SetObjResult(interp, servPtr->tcl.modules);
        break;

    case IGetIdx:
        /*
         * Get the current init script to evaluate in new interps.
         */

        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        Ns_RWLockRdLock(&servPtr->tcl.lock);
        Tcl_SetResult(interp, servPtr->tcl.script, TCL_VOLATILE);
        Ns_RWLockUnlock(&servPtr->tcl.lock);
        break;

    case IEpochIdx:
        /*
         * Check the version of this interp against current init script.
         */

        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        Ns_RWLockRdLock(&servPtr->tcl.lock);
        Tcl_SetIntObj(Tcl_GetObjResult(interp), servPtr->tcl.epoch);
        Ns_RWLockUnlock(&servPtr->tcl.lock);
        break;

    case ISaveIdx:
        /*
         * Save the init script.
         */

        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "script");
            return TCL_ERROR;
        }
        script = ns_strdup(Tcl_GetStringFromObj(objv[2], &length));
        Ns_RWLockWrLock(&servPtr->tcl.lock);
        ns_free(servPtr->tcl.script);
        servPtr->tcl.script = script;
        servPtr->tcl.length = length;
        if (++servPtr->tcl.epoch == 0) {
            /* NB: Epoch zero reserved for new interps. */
            ++itPtr->servPtr->tcl.epoch;
        }
        Ns_RWLockUnlock(&servPtr->tcl.lock);
        break;

    case IUpdateIdx:
        /*
         * Check for and process possible change in the init script.
         */

        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        result = UpdateInterp(itPtr);
        break;

    case ICleanupIdx:
        /*
         * Invoke the legacy defer callbacks.
         */

        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        while ((deferPtr = itPtr->firstDeferPtr) != NULL) {
            itPtr->firstDeferPtr = deferPtr->nextPtr;
            (*deferPtr->proc)(interp, deferPtr->arg);
            ns_free(deferPtr);
        }
        break;

    case IOnInitIdx:
    case IOnCreateIdx:
    case IOnCleanupIdx:
    case IOnDeleteIdx:
        /*
         * Register script-level interp traces (deprecated 3-arg form).
         */

        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "script");
            return TCL_ERROR;
        }
        script = Tcl_GetString(objv[objc-1]);

        switch (opt) {
        case IOnInitIdx:
        case IOnCreateIdx:
            when = NS_TCL_TRACE_CREATE;
            break;
        case IOnCleanupIdx:
            when = NS_TCL_TRACE_DEALLOCATE;
            break;
        case IOnDeleteIdx:
            when = NS_TCL_TRACE_DELETE;
            break;
        default:
            /* NB: Silence compiler. */
            break;
        }
        goto trace;
        break;

    case ITraceIdx:
        /*
         * Register script-level interp traces.
         */

        if (Ns_ParseObjv(NULL, traceArgs, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }

    trace:
        if (servPtr != NsGetInitServer()) {
            Tcl_SetResult(interp, "cannot register trace after server startup",
                          TCL_STATIC);
            return TCL_ERROR;
        }
        cbPtr = Ns_TclNewCallback(itPtr->interp, NsTclTraceProc,
                                  script, scriptArg);
        (void) Ns_TclRegisterTrace(servPtr->server, NsTclTraceProc, cbPtr, when);
        break;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclMarkForDeleteObjCmd --
 *
 *      Implements ns_markfordelete.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      See Ns_TclMarkForDelete.
 *
 *----------------------------------------------------------------------
 */

int
NsTclMarkForDeleteObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsInterp *itPtr = arg;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    itPtr->delete = 1;
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclDummyObjCmd --
 *
 *      Dummy command for ns_init and ns_cleanup.  The default
 *      init.tcl script will re-define these commands with propper
 *      startup initialization and deallocation scripts.
 *
 * Results:
 *      TCL_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclDummyObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    return TCL_OK;
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
NsTclAtCloseObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsInterp  *itPtr = arg;
    AtClose   *atPtr;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?args?");
        return TCL_ERROR;
    }
    if (itPtr->conn == NULL) {
        Tcl_SetResult(interp, "no connection", TCL_STATIC);
        return TCL_ERROR;
    }
    atPtr = ns_malloc(sizeof(AtClose));
    atPtr->nextPtr = itPtr->firstAtClosePtr;
    itPtr->firstAtClosePtr = atPtr;
    atPtr->objPtr = Tcl_ConcatObj(objc-1, objv+1);
    Tcl_IncrRefCount(atPtr->objPtr);

    return TCL_OK;
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
    Tcl_Interp  *interp = itPtr->interp;
    AtClose     *atPtr;

    while ((atPtr = itPtr->firstAtClosePtr) != NULL) {
        itPtr->firstAtClosePtr = atPtr->nextPtr;
        if (Tcl_EvalObjEx(interp, atPtr->objPtr, TCL_EVAL_DIRECT) != TCL_OK) {
            Ns_TclLogError(interp);
        }
        Tcl_DecrRefCount(atPtr->objPtr);
        ns_free(atPtr);
    }
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
NsTclInitServer(CONST char *server)
{
    NsServer *servPtr = NsGetServer(server);
    Tcl_Interp *interp;

    if (servPtr != NULL) {
        interp = Ns_TclAllocateInterp(server);
        if (Tcl_EvalFile(interp, servPtr->tcl.initfile) != TCL_OK) {
            Ns_TclLogError(interp);
        }
        Ns_TclDeAllocateInterp(interp);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsInitInterp --
 *
 *      Initialize the given interp with basic commands and if servPtr
 *      is not null, virtual server commands.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Depends on Tcl init script sourced by Tcl_Init.  Some Tcl
 *      object types will be initialized on first call.
 *
 *----------------------------------------------------------------------
 */

int
NsInitInterp(Tcl_Interp *interp, NsServer *servPtr, NsInterp **itPtrPtr)
{
    static volatile int  initialized = 0;
    NsInterp            *itPtr;
    int                  result = TCL_OK;

    /*
     * Core one-time server initialization to add a few Tcl_Obj
     * types.  These calls cannot be in NsTclInit above because
     * Tcl is not fully initialized at libnsd load time.
     */

    if (!initialized) {
        Ns_MasterLock();
        if (!initialized) {
            NsTclInitQueueType();
            NsTclInitAddrType();
            NsTclInitTimeType();
            NsTclInitSpecType();
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
        itPtr = ns_calloc(1, sizeof(NsInterp));
        itPtr->interp = interp;
        Tcl_InitHashTable(&itPtr->sets, TCL_STRING_KEYS);
        Tcl_InitHashTable(&itPtr->chans, TCL_STRING_KEYS);
        Tcl_InitHashTable(&itPtr->https, TCL_STRING_KEYS);

        /*
         * Associate the new NsInterp with this interp.  At interp delete
         * time, Tcl will call FreeInterpData to cleanup the struct.
         */

        Tcl_SetAssocData(interp, "ns:data", FreeInterpData, itPtr);

        /*
         * Add basic commands which function without a virtual server.
         */

        NsTclAddBasicCmds(itPtr);

        /*
         * Add virtual-server commands and update the interp
         * state, run create traces.
         */

        if (servPtr != NULL) {
            itPtr->servPtr = servPtr;
            NsTclAddServerCmds(itPtr);
            RunTraces(itPtr, NS_TCL_TRACE_CREATE);
            if ((result = UpdateInterp(itPtr)) != TCL_OK) {
                Ns_TclLogError(interp);
            }
        } else {
            RunTraces(itPtr, NS_TCL_TRACE_CREATE);
        }
    }
    if (itPtrPtr != NULL) {
        *itPtrPtr = itPtr;
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
    return (interp ? Tcl_GetAssocData(interp, "ns:data", NULL) : NULL);
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
        itPtr->nsconn.flags = 0;
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
 *      Status from script eval.
 *
 * Side effects:
 *      Depends on script.
 *
 *----------------------------------------------------------------------
 */

int
NsTclTraceProc(Tcl_Interp *interp, void *arg)
{
    Ns_TclCallback *cbPtr = arg;

    return Ns_TclEvalCallback(interp, cbPtr, NULL, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * PopInterp --
 *
 *      Pop next avaialble virtual-server interp from the per-thread
 *      cache, allocating a new interp if necessary.
 *
 * Results:
 *      Pointer to next available NsInterp.
 *
 * Side effects:
 *      Will invoke alloc traces and, if the interp is new, create
 *      traces.
 *
 *----------------------------------------------------------------------
 */

static NsInterp *
PopInterp(CONST char *server)
{
    NsServer      *servPtr;
    NsInterp      *itPtr;
    Tcl_Interp    *interp;
    Tcl_HashEntry *hPtr;
    static Ns_Cs   lock;

    /*
     * Verify the server.  NULL (i.e., no server) is valid but
     * a non-null, unknown server is an error.
     */

    if (server == NULL) {
        servPtr = NULL;
    } else {
        servPtr = NsGetServer(server);
        if (servPtr == NULL) {
            return NULL;
        }
    }

    /*
     * Pop the first interp off the list of availabe interps for
     * the given virtual server on this thread.  If none exists,
     * create and initialize a new interp.
     */

    hPtr = GetCacheEntry(servPtr);
    itPtr = Tcl_GetHashValue(hPtr);
    if (itPtr != NULL) {
        Tcl_SetHashValue(hPtr, itPtr->nextPtr);
    } else {
        if (nsconf.tcl.lockoninit) {
            Ns_CsEnter(&lock);
        }
        interp = CreateInterp();
        NsInitInterp(interp, servPtr, &itPtr);
        if (nsconf.tcl.lockoninit) {
            Ns_CsLeave(&lock);
        }
    }
    itPtr->nextPtr = NULL;

    /*
     * Run allocation traces and evaluate the ns_init proc which by
     * default updates the interp state with ns_ictl if necessary.
     */

    RunTraces(itPtr, NS_TCL_TRACE_ALLOCATE);
    if (Tcl_EvalEx(itPtr->interp, "ns_init", -1, 0) != TCL_OK) {
        Ns_TclLogError(itPtr->interp);
    }
    return itPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * PushInterp --
 *
 *      Return a virtual-server interp to the per-thread interp list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will invoke de-alloc traces, interp may be destroyed.
 *
 *----------------------------------------------------------------------
 */

static void
PushInterp(NsInterp *itPtr)
{
    Tcl_Interp *interp = itPtr->interp;
    Tcl_HashEntry *hPtr;

    /*
     * Evaluate the cleanup script to perform various garbage collection
     * and then either delete the interp or push it back on the
     * per-thread list.
     */

    RunTraces(itPtr, NS_TCL_TRACE_DEALLOCATE);
    if (Tcl_EvalEx(interp, "ns_cleanup", -1, 0) != TCL_OK) {
        Ns_TclLogError(interp);
    }
    if (itPtr->delete) {
        Ns_TclDestroyInterp(interp);
    } else {
        Tcl_ResetResult(interp);
        hPtr = GetCacheEntry(itPtr->servPtr);
        itPtr->nextPtr = Tcl_GetHashValue(hPtr);
        Tcl_SetHashValue(hPtr, itPtr);
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
GetCacheEntry(NsServer *servPtr)
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
 * CreateInterp --
 *
 *      Create a fresh new Tcl interp.
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
CreateInterp(NsInterp **itPtrPtr)
{
    Tcl_Interp *interp;

    /*
     * Create and initialize a basic Tcl interp.
     */

    interp = Tcl_CreateInterp();
    Tcl_InitMemory(interp);
    if (Tcl_Init(interp) != TCL_OK) {
        Ns_TclLogError(interp);
    }

    return interp;
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
FreeInterpData(ClientData arg, Tcl_Interp *interp)
{
    NsInterp *itPtr = arg;

    NsFreeAdp(itPtr);
    Tcl_DeleteHashTable(&itPtr->sets);
    Tcl_DeleteHashTable(&itPtr->chans);
    Tcl_DeleteHashTable(&itPtr->https);
    ns_free(itPtr);
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
    NsServer *servPtr = itPtr->servPtr;
    int       result = TCL_OK;

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
 * DeleteInterps --
 *
 *      Delete all per-thread interps at thread exit time.
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
    Tcl_HashTable  *tablePtr = arg;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    NsInterp       *itPtr;

    hPtr = Tcl_FirstHashEntry(tablePtr, &search);
    while (hPtr != NULL) {
        while ((itPtr = Tcl_GetHashValue(hPtr)) != NULL) {
            Tcl_SetHashValue(hPtr, itPtr->nextPtr);
            Ns_TclDestroyInterp(itPtr->interp);
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(tablePtr);
    ns_free(tablePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * RunTraces --
 *
 *      Execute interp trace callbacks in FIFO order.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depeneds on callbacks.
 *
 *----------------------------------------------------------------------
 */
 
static void
RunTraces(NsInterp *itPtr, int why)
{
    Trace *tracePtr;

    if (itPtr->servPtr != NULL) {
        tracePtr = itPtr->servPtr->tcl.firstTracePtr;
        while (tracePtr != NULL) {
            if ((tracePtr->when & why)) {
                if ((*tracePtr->proc)(itPtr->interp, tracePtr->arg) != TCL_OK) {
                    Ns_TclLogError(itPtr->interp);
                }
            }
            tracePtr = tracePtr->nextPtr;
        }
    }
}
