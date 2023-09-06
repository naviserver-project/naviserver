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
static void FreeRegisteredProc(void *arg) NS_GNUC_NONNULL(1);

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
 *      Register a new procedure to be called to service matching given method
 *      and URL path pattern. Before calling function Ns_RegisterRequest(),
 *      this functions verifies, if the URL path is correct. In particular, in
 *      the OP urlspace, query parameters or fragments are not allowed. For
 *      smooth upgrades, such URLs are fixed, and error message is included in
 *      the log. Future version might raise an exception in this case.
 *
 * Results:
 *      TCL return code.
 *
 * Side effects:
 *      In case of an error, the error message is left in ther result of the
 *      interp.
 *
 *----------------------------------------------------------------------
 */
int Ns_RegisterRequest2(Tcl_Interp *interp, const char *server, const char *method, const char *url,
                        Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg,
                        unsigned int flags)
{
    Ns_URL       parsedUrl;
    int          result = TCL_OK;
    const char  *errorMsg = NULL;
    Tcl_DString  ds;

    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, url, -1);

    if (Ns_ParseUrl(ds.string, NS_FALSE, &parsedUrl, &errorMsg) != NS_OK) {
        Tcl_DString errorDs;
        Tcl_DStringInit(&errorDs);
        Ns_DStringPrintf(&errorDs, "could not parse provided url %s: %s", url, errorMsg);
        if (interp != NULL) {
            Tcl_DStringResult(interp, &errorDs);
        } else {
            Ns_Log(Error, "register request handler: %s", errorDs.string);
            Tcl_DStringFree(&errorDs);
        }
        result = TCL_ERROR;
    } else {
        /*Ns_Log(Notice, "Ns_ParseURL of '%s' returned OK, path '%s' tail '%s', query '%s' fragment %s",
          url, parsedUrl.path, parsedUrl.tail, parsedUrl.query, parsedUrl.fragment);*/

        if (parsedUrl.query != NULL || parsedUrl.fragment != NULL) {
            Ns_Log(Warning, "register request handler: request path '%s'"
                   " contains query and/or fragment, which is not allowed;"
                   " the register operation is ignored'", url);
        } else {
            Ns_RegisterRequest(server, method, url, proc, deleteCallback, arg, flags);
        }
    }
    Tcl_DStringFree(&ds);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterRequest --
 *
 *      Register a new procedure to be called to service matching
 *      given method and URL path pattern.
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
    Ns_UrlSpecificSet(server, method, url, uid, regPtr, flags, FreeRegisteredProc);
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
    const RegisteredProc *regPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(procPtr != NULL);
    NS_NONNULL_ASSERT(argPtr != NULL);
    NS_NONNULL_ASSERT(flagsPtr != NULL);

    Ns_MutexLock(&ulock);
    regPtr = NsUrlSpecificGet(NsGetServer(server), method, url, uid,
                              0u, NS_URLSPACE_DEFAULT, NULL, NULL);
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
            RegisteredProc *regPtr;

            Ns_MutexLock(&ulock);
            regPtr = NsUrlSpecificGet(connPtr->poolPtr->servPtr,
                                      conn->request.method, conn->request.url, uid,
                                      0u, NS_URLSPACE_DEFAULT, NULL, NULL);
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
                status = (*regPtr->proc) (regPtr->arg, conn);

                Ns_MutexLock(&ulock);
                FreeRegisteredProc(regPtr);
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

    Ns_SetRequestUrl(&conn->request, url);

    /*
     * Re-authorize and run the request.
     */

    status = Ns_AuthorizeRequest(Ns_ConnServer(conn),
                                 conn->request.method,
                                 conn->request.url,
                                 Ns_ConnAuthUser(conn),
                                 Ns_ConnAuthPasswd(conn),
                                 Ns_ConnPeerAddr(conn));
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
        Ns_DString      ds;
        int             isNew;
        Tcl_HashEntry  *hPtr;

        Ns_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, method, protocol, (char *)0L);
        regPtr = ns_malloc(sizeof(RegisteredProc));
        regPtr->refcnt = 1;
        regPtr->proc = proc;
        regPtr->deleteCallback = deleteCallback;
        regPtr->arg = arg;
        regPtr->flags = 0u;
        Ns_MutexLock(&servPtr->request.plock);
        hPtr = Tcl_CreateHashEntry(&servPtr->request.proxy, ds.string, &isNew);
        if (isNew == 0) {
            FreeRegisteredProc(Tcl_GetHashValue(hPtr));
        }
        Tcl_SetHashValue(hPtr, regPtr);
        Ns_MutexUnlock(&servPtr->request.plock);
        Ns_DStringFree(&ds);
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
        Ns_DString     ds;

        Ns_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, method, protocol, (char *)0L);
        Ns_MutexLock(&servPtr->request.plock);
        hPtr = Tcl_FindHashEntry(&servPtr->request.proxy, ds.string);
        if (hPtr != NULL) {
            FreeRegisteredProc(Tcl_GetHashValue(hPtr));
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

Ns_ReturnCode
NsConnRunProxyRequest(Ns_Conn *conn)
{
    NsServer            *servPtr;
    RegisteredProc      *regPtr = NULL;
    Ns_ReturnCode        status;
    Ns_DString           ds;
    const Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(conn != NULL);

    servPtr = ((Conn *) conn)->poolPtr->servPtr;

    Ns_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, conn->request.method, conn->request.protocol, (char *)0L);
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
        FreeRegisteredProc(regPtr);
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
 * FreeRegisteredProc --
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
FreeRegisteredProc(void *arg)
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
