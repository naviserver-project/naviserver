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
 * op.c --
 *
 *      Routines to register, unregister, and run connection request
 *      routines (previously known as "op procs").
 */

#include "nsd.h"

/*
 * The following structure defines a request procedure including user
 * routine and client data.
 */

typedef struct {
    int             refcnt;
    Ns_OpProc      *proc;
    Ns_Callback    *deleteCallback;
    void           *arg;
    unsigned int    flags;
} Req;

/*
 * Static functions defined in this file.
 */

static Ns_ServerInitProc ConfigServerProxy;
static void WalkCallback(Tcl_DString *dsPtr, const void *arg) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void FreeReq(void *arg) NS_GNUC_NONNULL(1);

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

static int
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
 * Ns_RegisterRequest --
 *
 *      Register a new procedure to be called to service matching
 *      given method and url pattern.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Delete procedure of previously registered request, if any,
 *      will be called unless NS_OP_NODELETE flag is set.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RegisterRequest(const char *server, const char *method, const char *url,
                   Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg, 
		   unsigned int flags)
{
    Req *reqPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    reqPtr = ns_malloc(sizeof(Req));
    reqPtr->proc = proc;
    reqPtr->deleteCallback = deleteCallback;
    reqPtr->arg = arg;
    reqPtr->flags = flags;
    reqPtr->refcnt = 1;
    Ns_MutexLock(&ulock);
    Ns_UrlSpecificSet(server, method, url, uid, reqPtr, flags, FreeReq);
    Ns_MutexUnlock(&ulock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetRequest --
 *
 *      Return the procedures and context for a given method and url
 *      pattern.
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
Ns_GetRequest(const char *server, const char *method, const char *url,
              Ns_OpProc **procPtr, Ns_Callback **deletePtr, void **argPtr, 
	      unsigned int *flagsPtr)
{
    Req *reqPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(procPtr != NULL);
    NS_NONNULL_ASSERT(argPtr != NULL);
    NS_NONNULL_ASSERT(flagsPtr != NULL);

    Ns_MutexLock(&ulock);
    reqPtr = NsUrlSpecificGet(NsGetServer(server), method, url, uid,
                              0u, NS_URLSPACE_DEFAULT);
    if (reqPtr != NULL) {
        *procPtr = reqPtr->proc;
        *deletePtr = reqPtr->deleteCallback;
        *argPtr = reqPtr->arg;
        *flagsPtr = reqPtr->flags;
    } else {
        *procPtr = NULL;
        *deletePtr = NULL;
        *argPtr = NULL;
        *flagsPtr = 0u;
    }
    Ns_MutexUnlock(&ulock);
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
                     int inherit)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Ns_UnRegisterRequestEx(server, method, url, (inherit != 0) ? 0u : NS_OP_NOINHERIT);
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

int
Ns_ConnRunRequest(Ns_Conn *conn)
{
    int         status = NS_OK;
    Conn       *connPtr;
    Ns_Request *requestPtr;

    NS_NONNULL_ASSERT(conn != NULL);

    connPtr = (Conn *) conn;

    /*
     * Return error messages for invalid headers and 
     * the entity too large.
     */
    if (connPtr->flags & NS_CONN_ENTITYTOOLARGE) {
        connPtr->flags &= ~NS_CONN_ENTITYTOOLARGE;
        return Ns_ConnReturnEntityTooLarge(conn);
    } else if (connPtr->flags & NS_CONN_REQUESTURITOOLONG) {
        connPtr->flags &= ~NS_CONN_REQUESTURITOOLONG;
        return Ns_ConnReturnRequestURITooLong(conn);
    } else if (connPtr->flags & NS_CONN_LINETOOLONG) {
        connPtr->flags &= ~NS_CONN_LINETOOLONG;
        return Ns_ConnReturnHeaderLineTooLong(conn);
    }

    /*
     * True requests.
     */
    requestPtr = conn->request;
    assert(requestPtr != NULL);
    
    if ((requestPtr->method != NULL) && (requestPtr->url != NULL)) {
        Req        *reqPtr;

        Ns_Log(Debug, "Ns_ConnRunRequest: method <%s> url <%s>", requestPtr->method, requestPtr->url);
        
        Ns_MutexLock(&ulock);
        reqPtr = NsUrlSpecificGet(connPtr->poolPtr->servPtr,
                                  requestPtr->method, requestPtr->url, uid,
                                  0u, NS_URLSPACE_DEFAULT);
        if (reqPtr == NULL) {
            Ns_MutexUnlock(&ulock);
            if (STREQ(requestPtr->method, "BAD")) {
                return Ns_ConnReturnBadRequest(conn, NULL);
            } else {
                return Ns_ConnReturnInvalidMethod(conn);
            }
        }
        ++reqPtr->refcnt;
        Ns_MutexUnlock(&ulock);
        status = (*reqPtr->proc) (reqPtr->arg, conn);
        Ns_MutexLock(&ulock);
        FreeReq(reqPtr);
        Ns_MutexUnlock(&ulock);
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

int
Ns_ConnRedirect(Ns_Conn *conn, const char *url)
{
    int status;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    /*
     * Update the request URL.
     */

    Ns_SetRequestUrl(conn->request, url);

    /*
     * Re-authorize and run the request.
     */

    status = Ns_AuthorizeRequest(Ns_ConnServer(conn),
                                 conn->request->method,
                                 conn->request->url,
                                 Ns_ConnAuthUser(conn),
                                 Ns_ConnAuthPasswd(conn),
                                 Ns_ConnPeer(conn));
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
    case NS_ERROR:
    default:
        status = Ns_ConnReturnInternalError(conn);
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
    NsServer      *servPtr;
    Req           *reqPtr;
    Ns_DString     ds;
    int            isNew;
    Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(protocol != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        Ns_Log(Error, "Ns_RegisterProxyRequest: no such server: %s", server);
        return;
    }
    Ns_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, method, protocol, NULL);
    reqPtr = ns_malloc(sizeof(Req));
    reqPtr->refcnt = 1;
    reqPtr->proc = proc;
    reqPtr->deleteCallback = deleteCallback;
    reqPtr->arg = arg;
    reqPtr->flags = 0u;
    Ns_MutexLock(&servPtr->request.plock);
    hPtr = Tcl_CreateHashEntry(&servPtr->request.proxy, ds.string, &isNew);
    if (isNew == 0) {
        FreeReq(Tcl_GetHashValue(hPtr));
    }
    Tcl_SetHashValue(hPtr, reqPtr);
    Ns_MutexUnlock(&servPtr->request.plock);
    Ns_DStringFree(&ds);
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
        Ns_DString     ds;

        Ns_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, method, protocol, NULL);
        Ns_MutexLock(&servPtr->request.plock);
        hPtr = Tcl_FindHashEntry(&servPtr->request.proxy, ds.string);
        if (hPtr != NULL) {
            FreeReq(Tcl_GetHashValue(hPtr));
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&servPtr->request.plock);
        Ns_DStringFree(&ds);
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

int
NsConnRunProxyRequest(Ns_Conn *conn)
{
    Conn          *connPtr = (Conn *) conn;
    NsServer      *servPtr;
    Ns_Request    *request;
    Req           *reqPtr = NULL;
    int            status;
    Ns_DString     ds;
    Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(conn != NULL);
    
    servPtr = connPtr->poolPtr->servPtr;
    request = conn->request;

    Ns_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, request->method, request->protocol, NULL);
    Ns_MutexLock(&servPtr->request.plock);
    hPtr = Tcl_FindHashEntry(&servPtr->request.proxy, ds.string);
    if (hPtr != NULL) {
        reqPtr = Tcl_GetHashValue(hPtr);
        ++reqPtr->refcnt;
    }
    Ns_MutexUnlock(&servPtr->request.plock);
    if (reqPtr == NULL) {
        status = Ns_ConnReturnNotFound(conn);
    } else {
        status = (*reqPtr->proc) (reqPtr->arg, conn);
        Ns_MutexLock(&servPtr->request.plock);
        FreeReq(reqPtr);
        Ns_MutexUnlock(&servPtr->request.plock);
    }
    Ns_DStringFree(&ds);

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
    NsServer *servPtr;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);
    
    servPtr = NsGetServer(server);
    assert(servPtr != NULL);
    
    Ns_MutexLock(&ulock);
    Ns_UrlSpecificWalk(uid, servPtr->server, WalkCallback, dsPtr);
    Ns_MutexUnlock(&ulock);
}

static void
WalkCallback(Tcl_DString *dsPtr, const void *arg)
{
     const Req *reqPtr = arg;

     NS_NONNULL_ASSERT(dsPtr != NULL);
     NS_NONNULL_ASSERT(arg != NULL);
     
     Ns_GetProcInfo(dsPtr, (Ns_Callback *)reqPtr->proc, reqPtr->arg);
}


/*
 *----------------------------------------------------------------------
 *
 * FreeReq --
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
FreeReq(void *arg)
{
    Req *reqPtr = (Req *) arg;

    NS_NONNULL_ASSERT(arg != NULL);

    if (--reqPtr->refcnt == 0) {
        if (reqPtr->deleteCallback != NULL) {
            (*reqPtr->deleteCallback) (reqPtr->arg);
        }
        ns_free(reqPtr);
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
