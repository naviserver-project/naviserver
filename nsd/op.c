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
 * op.c --
 *
 *      Routines to register, unregister, and run connection request
 *      routines (previously known as "op procs").
 */

#include "nsd.h"

/*
 * The following structure defines a structure for registered procs including
 * client data and a deletion callback.
 */

typedef struct {
    int             refcnt;
    Ns_OpProc      *proc;
    Ns_Callback    *deleteCallback;
    void           *arg;
    unsigned int    flags;
} RegisteredProc;

/*
 * Static functions defined in this file.
 */

static Ns_ServerInitProc ConfigServerProxy;
static void WalkCallback(Tcl_DString *dsPtr, const void *arg) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void RegisteredProcDecrRef(void *arg) NS_GNUC_NONNULL(1);
static void RegisterRequest(const char *server, const char *method, const char *url,
                            Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg,
                            unsigned int flags, void *contextSpec)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4);

/*
 * Static variables defined in this file.
 */

static Ns_Mutex       ulock = NULL;
static int            uid = 0;


/*
 *----------------------------------------------------------------------
 *
 * NsInitRequests --
 *
 *      Initialize the request API.
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
NsInitRequests(void)
{
    uid = Ns_UrlSpecificAlloc();
    Ns_MutexInit(&ulock);
    Ns_MutexSetName(&ulock, "nsd:requests");

    NsRegisterServerInit(ConfigServerProxy);
}

static Ns_ReturnCode
ConfigServerProxy(const char *server)
{
    NsServer *servPtr = NsGetServer(server);

    Tcl_InitHashTable(&servPtr->request.proxy, TCL_STRING_KEYS);
    Ns_MutexInit(&servPtr->request.plock);
    Ns_MutexSetName2(&servPtr->request.plock, "nsd:proxy", server);

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterRequest2 --
 *
 *      Register a new request handler callback for a given server,
 *      HTTP method, and URL path, after validating that the URL is a
 *      plain path (no illegal characters).  If URL validation fails,
 *      logs an error (or optionally returns a Tcl error) and does not
 *      register the handler.
 *
 * Results:
 *      TCL_OK if the handler was successfully registered or if URL
 *      validation failed but errors are only logged.  TCL_ERROR if URL
 *      validation fails and error raising is enabled on the interpreter.
 *
 * Side Effects:
 *      On success, invokes RegisterRequest() to allocate and insert
 *      the handler into the URL dispatch table.  May emit a log entry
 *      for invalid URL paths.
 *
 *----------------------------------------------------------------------
 */
int Ns_RegisterRequest2(Tcl_Interp *interp, const char *server, const char *method, const char *url,
                        Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg,
                        unsigned int flags, void *contextSpec)
{
    int          result = TCL_OK;
    const char  *errorMsg = NULL;

    if (!Ns_PlainUrlPath(url, &errorMsg)) {
        Tcl_DString errorDs;
        bool raiseError;

        /*
         * Raising errors is deactivated for the time being to improve
         * backwards compatibility.
         */
        raiseError = NS_FALSE;

        Tcl_DStringInit(&errorDs);
        Ns_DStringPrintf(&errorDs, "invalid URL path %s: %s", url, errorMsg);
        if (raiseError && interp != NULL) {
            Tcl_DStringResult(interp, &errorDs);
            result = TCL_ERROR;
        } else {
            Ns_Log(Error, "register request handler: %s", errorDs.string);
            Tcl_DStringFree(&errorDs);
        }

    } else {
        RegisterRequest(server, method, url, proc, deleteCallback, arg, flags, contextSpec);
    }

    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * RegisterRequest --
 *
 *      Internal helper to allocate and register a request handler
 *      in the URL dispatch trie for a given server, method, and URL.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Allocates a RegisteredProc structure, initializes its fields,
 *      then locks the URL space mutex and calls Ns_UrlSpecificSet2()
 *      to insert the handler into the server’s dispatch table.
 *
 *----------------------------------------------------------------------
 */
static void
RegisterRequest(const char *server, const char *method, const char *url,
                   Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg,
                   unsigned int flags, void *contextSpec)
{
    RegisteredProc *regPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    regPtr = ns_malloc(sizeof(RegisteredProc));
    regPtr->proc = proc;
    regPtr->deleteCallback = deleteCallback;
    regPtr->arg = arg;
    regPtr->flags = flags;
    regPtr->refcnt = 1;
    Ns_MutexLock(&ulock);
    Ns_UrlSpecificSet2(server, method, url, uid, regPtr, flags,
                       RegisteredProcDecrRef, contextSpec);
    Ns_MutexUnlock(&ulock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterRequest --
 *
 *      Public API to register a request handler callback for the
 *      specified server, method, and URL.  This is a thin wrapper
 *      around RegisterRequest() that omits a context specification.
 *
 * Results:
 *      None.
 *
 * Side Effects:
 *      Allocates and registers a handler identically to
 *      RegisterRequest(), with contextSpec == NULL.
 *
 *----------------------------------------------------------------------
 */
void
Ns_RegisterRequest(const char *server, const char *method, const char *url,
                   Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg,
                   unsigned int flags)
{
    RegisterRequest(server, method, url, proc, deleteCallback, arg, flags, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetRequest, NsGetRequest2 --
 *
 *      Return the procedures and context for a given method and url
 *      pattern. While Ns_GetRequest() provides the legacy interface,
 *      NsGetRequest2() gives more fine granular input to without exposing
 *      static definitions.
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
NsGetRequest2(NsServer *servPtr, const char *method, const char *url,
              unsigned int flags, Ns_UrlSpaceOp op,
              Ns_UrlSpaceContextFilterEvalProc proc, void *context,
              Ns_OpProc **procPtr, Ns_Callback **deletePtr, void **argPtr,
              unsigned int *flagsPtr)
{
    const RegisteredProc *regPtr;
    Ns_UrlSpaceMatchInfo  matchInfo;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(procPtr != NULL);
    NS_NONNULL_ASSERT(argPtr != NULL);
    NS_NONNULL_ASSERT(flagsPtr != NULL);

    Ns_MutexLock(&ulock);
    regPtr = NsUrlSpecificGet((Ns_Server*)servPtr, method, url,
                              uid, flags, op, &matchInfo, proc, context);
    Ns_Log(Debug, "NsGetRequest2 %s %s -> %p",  method, url, (void*)regPtr);

    if (regPtr != NULL) {
        *procPtr = regPtr->proc;
        *deletePtr = regPtr->deleteCallback;
        *argPtr = regPtr->arg;
        *flagsPtr = regPtr->flags;
    } else {
        *procPtr = NULL;
        *deletePtr = NULL;
        *argPtr = NULL;
        *flagsPtr = 0u;
    }
    Ns_MutexUnlock(&ulock);
}


void
Ns_GetRequest(const char *server, const char *method, const char *url,
              Ns_OpProc **procPtr, Ns_Callback **deletePtr, void **argPtr,
              unsigned int *flagsPtr)
{

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(procPtr != NULL);
    NS_NONNULL_ASSERT(argPtr != NULL);
    NS_NONNULL_ASSERT(flagsPtr != NULL);

    NsGetRequest2(NsGetServer(server), method, url,
                  0u, NS_URLSPACE_DEFAULT,
                  NULL, NULL,
                  procPtr, deletePtr, argPtr, flagsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UnRegisterRequest --
 *
 *      Remove the procedure which would run for the given method and
 *      url pattern.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Requests deleteProc may run.
 *
 *----------------------------------------------------------------------
 */

void
Ns_UnRegisterRequest(const char *server, const char *method, const char *url,
                     bool inherit)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Ns_UnRegisterRequestEx(server, method, url, (inherit ? 0u : NS_OP_NOINHERIT));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UnRegisterRequestEx --
 *
 *      Remove the procedure which would run for the given method and
 *      url pattern, passing along any flags.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Requests deleteProc may run.
 *
 *----------------------------------------------------------------------
 */

void
Ns_UnRegisterRequestEx(const char *server, const char *method, const char *url,
                       unsigned int flags)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Ns_MutexLock(&ulock);
    (void)Ns_UrlSpecificDestroy(server, method, url, uid, flags);
    Ns_MutexUnlock(&ulock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnRunRequest --
 *
 *      Locate and execute the procedure for the given method and
 *      url pattern.
 *
 * Results:
 *      Standard request procedure result, normally NS_OK.
 *
 * Side effects:
 *      Depends on request procedure.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnRunRequest(Ns_Conn *conn)
{
    Ns_ReturnCode  status = NS_OK;
    Conn          *connPtr;

    NS_NONNULL_ASSERT(conn != NULL);

    connPtr = (Conn *) conn;

    /*
     * Return error messages for invalid headers and
     * the entity too large.
     */
    if ((connPtr->flags & NS_CONN_ENTITYTOOLARGE) != 0u) {
        connPtr->flags &= ~NS_CONN_ENTITYTOOLARGE;
        status = Ns_ConnReturnEntityTooLarge(conn);

    } else if ((connPtr->flags & NS_CONN_REQUESTURITOOLONG) != 0u) {
        connPtr->flags &= ~NS_CONN_REQUESTURITOOLONG;
        status = Ns_ConnReturnRequestURITooLong(conn);

    } else if ((connPtr->flags & NS_CONN_LINETOOLONG) != 0u) {
        connPtr->flags &= ~NS_CONN_LINETOOLONG;
        status = Ns_ConnReturnHeaderLineTooLong(conn);

    } else {
        /*
         * True requests.
         */

        if ((conn->request.method != NULL) && (conn->request.url != NULL)) {
            RegisteredProc       *regPtr;
            Ns_UrlSpaceMatchInfo  matchInfo;
            NsUrlSpaceContext     ctx;

            NsUrlSpaceContextInit(&ctx, connPtr->sockPtr, connPtr->headers);

            Ns_MutexLock(&ulock);
            regPtr = NsUrlSpecificGet((Ns_Server *)(connPtr->poolPtr->servPtr),
                                      conn->request.method, conn->request.url, uid,
                                      0u, NS_URLSPACE_DEFAULT, &matchInfo,
                                      NsUrlSpaceContextFilterEval, &ctx);
            /*Ns_Log(Notice, "Ns_ConnRunRequest %s %s -> %p (isSegmentMatch %d, offset %ld)",
                   conn->request.method, conn->request.url, (void*)regPtr,
                   matchInfo.isSegmentMatch, matchInfo.offset);*/

            if (regPtr == NULL) {
                Ns_MutexUnlock(&ulock);
                if (STREQ(conn->request.method, "BAD")) {
                    status = Ns_ConnReturnBadRequest(conn, NULL);
                } else {
                    status = Ns_ConnReturnInvalidMethod(conn);
                }
            } else {
                ++regPtr->refcnt;
                Ns_MutexUnlock(&ulock);
                connPtr->matchInfo = matchInfo;
                status = (*regPtr->proc) (regPtr->arg, conn);

                Ns_MutexLock(&ulock);
                RegisteredProcDecrRef(regPtr);
                Ns_MutexUnlock(&ulock);
            }
        }
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnRedirect --
 *
 *      Perform an internal redirect by updating the connection's
 *      request URL and re-authorizing and running the request.  This
 *      Routine is used in FastPath to redirect to directory files
 *      (e.g., index.html) and in return.c to redirect by HTTP result
 *      code (e.g., custom not-found handler).
 *
 * Results:
 *      Standard request procedure result, normally NS_OK.
 *
 * Side effects:
 *      Depends on request procedure.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnRedirect(Ns_Conn *conn, const char *url)
{
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    /*
     * Update the request URL.
     */
    status = Ns_SetRequestUrl(&conn->request, url);

    if (status == NS_OK) {
        /*
         * Re-authorize and run the request.
         */
        status = NsAuthorizeRequest((NsServer*)NsConnServPtr(conn),
                                    conn->request.method,
                                    conn->request.url,
                                    Ns_ConnAuthUser(conn),
                                    Ns_ConnAuthPasswd(conn),
                                    Ns_ConnPeerAddr(conn));
    }

    switch (status) {
    case NS_OK:
        status = Ns_ConnRunRequest(conn);
        break;
    case NS_FORBIDDEN:
        status = Ns_ConnReturnForbidden(conn);
        break;
    case NS_UNAUTHORIZED:
        status = Ns_ConnReturnUnauthorized(conn);
        break;
    case NS_ERROR:          NS_FALL_THROUGH; /* fall through */
    case NS_FILTER_BREAK:   NS_FALL_THROUGH; /* fall through */
    case NS_FILTER_RETURN:  NS_FALL_THROUGH; /* fall through */
    case NS_TIMEOUT:
        status = Ns_ConnTryReturnInternalError(conn, status, "redirect, after authorize request");
        break;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterProxyRequest --
 *
 *      Register a new procedure to be called to proxy matching
 *      given method and protocol pattern.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Delete procedure of previously registered request, if any.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RegisterProxyRequest(const char *server, const char *method, const char *protocol,
                        Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(protocol != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        Ns_Log(Error, "Ns_RegisterProxyRequest: no such server: %s", server);
    } else {
        RegisteredProc *regPtr;
        Tcl_DString     ds;
        int             isNew;
        Tcl_HashEntry  *hPtr;

        Tcl_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, method, protocol, NS_SENTINEL);
        regPtr = ns_malloc(sizeof(RegisteredProc));
        regPtr->refcnt = 1;
        regPtr->proc = proc;
        regPtr->deleteCallback = deleteCallback;
        regPtr->arg = arg;
        regPtr->flags = 0u;
        Ns_MutexLock(&servPtr->request.plock);
        hPtr = Tcl_CreateHashEntry(&servPtr->request.proxy, ds.string, &isNew);
        if (isNew == 0) {
            RegisteredProcDecrRef(Tcl_GetHashValue(hPtr));
        }
        Tcl_SetHashValue(hPtr, regPtr);
        Ns_MutexUnlock(&servPtr->request.plock);
        Tcl_DStringFree(&ds);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UnRegisterProxyRequest --
 *
 *      Remove the procedure which would run for the given method and
 *      protocol.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Request's deleteProc may run.
 *
 *----------------------------------------------------------------------
 */

void
Ns_UnRegisterProxyRequest(const char *server, const char *method,
                          const char *protocol)
{
    NsServer      *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(protocol != NULL);

    servPtr = NsGetServer(server);
    if (servPtr != NULL) {
        Tcl_HashEntry *hPtr;
        Tcl_DString    ds;

        Tcl_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, method, protocol, NS_SENTINEL);
        Ns_MutexLock(&servPtr->request.plock);
        hPtr = Tcl_FindHashEntry(&servPtr->request.proxy, ds.string);
        if (hPtr != NULL) {
            RegisteredProcDecrRef(Tcl_GetHashValue(hPtr));
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&servPtr->request.plock);
        Tcl_DStringFree(&ds);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsConnRunProxyRequest --
 *
 *      Locate and execute the procedure for the given method and
 *      protocol pattern.
 *
 * Results:
 *      Standard request procedure result, normally NS_OK.
 *
 * Side effects:
 *      Depends on request procedure.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsConnRunProxyRequest(Ns_Conn *conn)
{
    NsServer            *servPtr;
    RegisteredProc      *regPtr = NULL;
    Ns_ReturnCode        status;
    Tcl_DString          ds;
    const Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(conn != NULL);

    servPtr = ((Conn *) conn)->poolPtr->servPtr;

    Tcl_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, conn->request.method, conn->request.protocol, NS_SENTINEL);
    Ns_MutexLock(&servPtr->request.plock);
    hPtr = Tcl_FindHashEntry(&servPtr->request.proxy, ds.string);
    if (hPtr != NULL) {
        regPtr = Tcl_GetHashValue(hPtr);
        ++regPtr->refcnt;
    }
    Ns_MutexUnlock(&servPtr->request.plock);
    if (regPtr == NULL) {
        status = Ns_ConnReturnNotFound(conn);
    } else {
        status = (*regPtr->proc) (regPtr->arg, conn);
        Ns_MutexLock(&servPtr->request.plock);
        RegisteredProcDecrRef(regPtr);
        Ns_MutexUnlock(&servPtr->request.plock);
    }
    Tcl_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 * NsGetRequestProcs --
 *
 *      Get a description of each registered request for the given
 *      server.
 *
 * Results:
 *      DString with info in Tcl list form.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsGetRequestProcs(Tcl_DString *dsPtr, const char *server)
{
    const NsServer *servPtr;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Ns_MutexLock(&ulock);
        Ns_UrlSpecificWalk(uid, servPtr->server, WalkCallback, dsPtr);
        Ns_MutexUnlock(&ulock);
    }
}

static void
WalkCallback(Tcl_DString *dsPtr, const void *arg)
{
     const RegisteredProc *regPtr;

     NS_NONNULL_ASSERT(dsPtr != NULL);
     NS_NONNULL_ASSERT(arg != NULL);
     regPtr = arg;

     Ns_GetProcInfo(dsPtr, (ns_funcptr_t)regPtr->proc, regPtr->arg);
}


/*
 *----------------------------------------------------------------------
 *
 * RegisteredProcDecrRef --
 *
 *      URL space callback to delete a request structure.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on request delete procedure.
 *
 *----------------------------------------------------------------------
 */

static void
RegisteredProcDecrRef(void *arg)
{
    RegisteredProc *regPtr = (RegisteredProc *) arg;

    NS_NONNULL_ASSERT(arg != NULL);

    if (--regPtr->refcnt == 0) {
        if (regPtr->deleteCallback != NULL) {
            (*regPtr->deleteCallback) (regPtr->arg);
        }
        ns_free(regPtr);
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
