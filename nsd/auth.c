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
 * auth.c --
 *
 *      URL level HTTP authorization support.
 */

#include "nsd.h"

/*
 *  Structures for Authentication Chains
 */
 typedef struct RequestAuth {
    struct RequestAuth      *nextPtr;   /* Next entry in the request‐auth callback chain */
    Ns_AuthorizeRequestProc *proc;      /* Function to invoke for request‐level authorization */
    void                    *arg;       /* Registration data to pass to proc() */
    const char              *authority; /* Identifier of the registration authority that installed this callback */
} RequestAuth;

typedef struct UserAuth {
    struct UserAuth         *nextPtr;   /* Next entry in the user‐auth callback chain */
    Ns_AuthorizeUserProc    *proc;      /* Function to invoke for user‐level authorization */
    void                    *arg;       /* Registration data to pass to proc() */
    const char              *authority; /* Identifier of the registration authority that installed this callback */
} UserAuth;

typedef struct AuthNode { /* common “base” for both types */
    void *nextPtr;
} AuthNode;


/*
 * Static functions defined in this file.
 */
static int
HandleAuthorizationResult(Tcl_Interp *interp, Ns_ReturnCode status, const char *cmdName,
                          const char *authority, bool asDict,
                          const char *arg1, const char *arg2, int *result)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static void AuthLock(NsServer *servPtr, NS_RW rw) NS_GNUC_NONNULL(1);
static void AuthUnlock(NsServer *servPtr) NS_GNUC_NONNULL(1);

static void *RegisterAuth(NsServer *servPtr, void *authPtr, void **firstAuthPtr, bool first)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static TCL_OBJCMDPROC_T UserAuthorizeObjCmd;

/*
 *----------------------------------------------------------------------
 *
 * AuthLock --
 *
 *      Acquire the appropriate read or write lock on the server’s
 *      request‐authorization list. Used for request and user authentications.
 *      A NS_READ request obtains a shared read lock, allowing concurrent
 *      readers; NS_WRITE obtains an exclusive write lock, blocking other
 *      readers and writers.
 *
 * Side Effects:
 *      Blocks until the lock is successfully acquired.
 *
 *----------------------------------------------------------------------
 */

static void
AuthLock(NsServer *servPtr, NS_RW rw) {
    if (rw == NS_READ) {
        Ns_RWLockRdLock(&servPtr->request.rwlock);
    } else {
        Ns_RWLockWrLock(&servPtr->request.rwlock);
    }
}
/*
 *----------------------------------------------------------------------
 *
 * AuthUnlock --
 *
 *      Release the previously acquired read or write lock on the server’s
 *      request‐authorization list. Used for request and user authentications.
 *
 * Side Effects:
 *      Unlocks the rwlock, potentially waking blocked threads.
 *
 *----------------------------------------------------------------------
 */
static void
AuthUnlock(NsServer *servPtr) {
    Ns_RWLockUnlock(&servPtr->request.rwlock);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AuthorizeRequest --
 *
 *      Executes the chain of registered request-authorization callbacks
 *      for a single HTTP request. Each callback is invoked in the order
 *      of registration and uses the following signature:
 *
 *          Ns_ReturnCode proc(arg, conn, &continuation);
 *
 *      Callbacks return an `Ns_ReturnCode` and may set *continuation to
 *      a Tcl code (e.g., TCL_BREAK, TCL_RETURN) to control the flow of
 *      further callback execution. After each callback, *authorityPtr is
 *      updated to reflect the authority label of the last invoked callback,
 *      so callers can know which module handled the request.
 *
 *      If all callbacks have been invoked or one has set *continuation to
 *      something other than TCL_OK, and *continuation is TCL_RETURN, the
 *      status is set to NS_FILTER_RETURN to indicate that further processing
 *      is not desired.
 *
 * Parameters:
 *      conn         - The connection object for the HTTP request.
 *      authorityPtr - Output parameter that is set to the authority label
 *                    of the last callback invoked, or NULL if no callbacks
 *                    were registered.
 *
 * Results:
 *      Returns the `Ns_ReturnCode` from the last callback invoked, or NS_OK
 *      if no callbacks were registered. The possible return codes from the
 *      callbacks are:
 *
 *        NS_OK            - access granted
 *        NS_FORBIDDEN     - access denied (e.g., IP-based deny rule)
 *        NS_UNAUTHORIZED  - authentication required or failed (retry possible)
 *        NS_ERROR         - internal error (e.g., server or module issue)
 *        NS_FILTER_RETURN - mapped from Tcl continuation TCL_RETURN
 *
 * Side Effects:
 *      Acquires a read lock on the server’s request authorization list
 *      and invokes each authorization procedure in turn under that lock.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_AuthorizeRequest(Ns_Conn *conn, const char **authorityPtr)
{
    Ns_ReturnCode status = NS_OK;
    NsServer     *servPtr;
    int           continuation = TCL_OK;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(authorityPtr != NULL);

    *authorityPtr = NULL;
    servPtr = (NsServer *)Ns_ConnServPtr(conn);

    AuthLock(servPtr, NS_READ);
    if (servPtr->request.firstRequestAuthPtr != NULL) {
        const RequestAuth *authPtr = servPtr->request.firstRequestAuthPtr;

        while (authPtr != NULL && continuation == TCL_OK) {
            status = (*authPtr->proc)(authPtr->arg, conn, &continuation);
            *authorityPtr = authPtr->authority;
            authPtr = authPtr->nextPtr;
        }
    }
    AuthUnlock(servPtr);

    if (continuation == TCL_RETURN) {
        status = NS_FILTER_RETURN;
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AuthorizeUser --
 *
 *      Executes the chain of registered user-authorization callbacks to verify
 *      the given username/password pair. Each UserAuth callback is invoked
 *      in registration order under a shared read-lock. Iteration stops when a
 *      callback sets the continuation code to non-TCL_OK or the list is exhausted.
 *      If all callbacks have been executed (or one sets continuation to non-TCL_OK),
 *      and continuation is TCL_RETURN, the function returns NS_FILTER_RETURN to signal
 *      that further processing is not desired.
 *
 * Parameters:
 *      server       - The NsServer whose user authorization callbacks should be invoked.
 *      user         - Username string to verify.
 *      passwd       - Password string to check.
 *      authorityPtr - Out parameter; set to the authority label of the callback
 *                     that determined the final status, or NULL if no callbacks ran.
 *
 * Results:
 *      Returns the `Ns_ReturnCode` from the last callback invoked, or NS_UNAUTHORIZED
 *      if no callbacks are registered. Possible return codes are:
 *
 *        NS_OK            - Credentials accepted.
 *        NS_FORBIDDEN     - User exists, but password is incorrect.
 *        NS_UNAUTHORIZED  - User does not exist, or no callback registered.
 *        NS_FILTER_RETURN - Mapped from continuation TCL_RETURN.
 *        NS_ERROR         - Internal error during checks.
 *
 * Side Effects:
 *      Acquires the server’s user authorization list read-lock and invokes each
 *      authorization procedure under that lock.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_AuthorizeUser(Ns_Server *server, const char *user, const char *passwd,
                 const char ** authorityPtr)
{
    Ns_ReturnCode status = NS_UNAUTHORIZED;
    NsServer     *servPtr = (NsServer *)server;
    int           continuation = TCL_OK;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(user != NULL);
    NS_NONNULL_ASSERT(passwd != NULL);
    NS_NONNULL_ASSERT(authorityPtr != NULL);

    *authorityPtr = NULL;

    AuthLock(servPtr, NS_READ);
    if (servPtr->request.firstUserAuthPtr != NULL) {
        const UserAuth *authPtr = servPtr->request.firstUserAuthPtr;

        while (authPtr != NULL && continuation == TCL_OK) {
            status = (*authPtr->proc)(authPtr->arg, (Ns_Server *)servPtr, user, passwd, &continuation);
            Ns_Log(Notice, "Ns_AuthorizeUser: authority '%s'"
                   " for user '%s' passwd '%s' -> %d (%s) continuation %s",
                   authPtr->authority, user, passwd,
                   status , Ns_ReturnCodeString(status), Ns_TclReturnCodeString(continuation));
            *authorityPtr = authPtr->authority;
            authPtr = authPtr->nextPtr;
        }
    }
    AuthUnlock(servPtr);

    if (continuation == TCL_RETURN) {
        status = NS_FILTER_RETURN;
    }

    /*fprintf(stderr, "Ns_AuthorizeUser returns %d (%s) for user %s passwd %s\n",
            status , Ns_ReturnCodeString(status),
            user, passwd);*/
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * NsGetAuthprocs --
 *
 *      Enumerate the registered user-level and request-level authorization
 *      callbacks for a server, and append their metadata into a Tcl_DString
 *      as a Tcl list of dict-style entries.
 *
 *      Each entry appended to provide Tcl_DString
 *      is itself a Tcl list containing key/value pairs:
 *          type      – "user" or "request"
 *          authority – the authority label supplied at registration
 *          proc      – a string describing the C function or Tcl callback
 *
 * Parameters:
 *      dsPtr   – initialized Tcl_DString; on return it will contain a Tcl list
 *                of dict entries, one per registered auth proc.
 *      servPtr – the NsServer whose authorization callback chains are inspected.
 *
 * Results:
 *      None.  After this call, dsPtr holds a Tcl list of dict entries as above.
 *
 * Side Effects:
 *      Acquires servPtr’s AuthLock in read mode while walking the lists,
 *      then releases the lock.  Allocates temporary Tcl_Obj and Tcl_DString
 *      structures for formatting each entry.
 *
 *----------------------------------------------------------------------
 */
void
NsGetAuthprocs(Tcl_DString *dsPtr, NsServer *servPtr)
{
    AuthLock(servPtr, NS_READ);

    /*
     * User Auth procs
     */
    if (servPtr->request.firstUserAuthPtr != NULL) {
        const UserAuth *authPtr = servPtr->request.firstUserAuthPtr;

        while (authPtr != NULL) {
            Tcl_Obj *innerListObj = Tcl_NewListObj(0, NULL);
            Tcl_DString procInfo;

            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("type", 4));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("user", 4));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("authority", 9));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj(authPtr->authority, TCL_INDEX_NONE));

            Tcl_DStringInit(&procInfo);
            Ns_GetProcInfo(&procInfo, (ns_funcptr_t) authPtr->proc, authPtr->arg);
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("proc", 4));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj(procInfo.string, procInfo.length));
            Tcl_DStringFree(&procInfo);

            Tcl_DStringAppendElement(dsPtr, Tcl_GetString(innerListObj));
            Tcl_DecrRefCount(innerListObj);
            authPtr = authPtr->nextPtr;
        }
    }
    /*
     * Request Auth procs
     */
    if (servPtr->request.firstRequestAuthPtr != NULL) {
        const RequestAuth *authPtr = servPtr->request.firstRequestAuthPtr;

        while (authPtr != NULL) {
            Tcl_Obj *innerListObj = Tcl_NewListObj(0, NULL);
            Tcl_DString procInfo;

            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("type", 4));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("request", 7));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("authority", 9));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj(authPtr->authority, TCL_INDEX_NONE));

            Tcl_DStringInit(&procInfo);
            Ns_GetProcInfo(&procInfo, (ns_funcptr_t) authPtr->proc, authPtr->arg);
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj("proc", 4));
            Tcl_ListObjAppendElement(NULL, innerListObj, Tcl_NewStringObj(procInfo.string, procInfo.length));
            Tcl_DStringFree(&procInfo);

            Tcl_DStringAppendElement(dsPtr, Tcl_GetString(innerListObj));
            Tcl_DecrRefCount(innerListObj);
            authPtr = authPtr->nextPtr;
        }
    }
    AuthUnlock(servPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * RegisterAuth --
 *
 *      Generic helper to insert an authentication callback object into a
 *      server’s linked list, either at the head or the tail.
 *
 *      This works for both RequestAuth and UserAuth, since they both
 *      begin with a `nextPtr` field of type void *.
 *
 * Parameters:
 *      servPtr      - pointer to the NsServer whose list is being modified.
 *      authPtr      - newly allocated authentication object
 *                     (RequestAuth * or UserAuth *).
 *      firstAuthPtr - address of the head pointer for this list
 *      first        - if true, insert authPtr at the front of the list;
 *                     otherwise append.
 *
 * Results:
 *      Returns authPtr (the pointer you passed in), for deregistration
 *      or later reference.
 *
 * Side Effects:
 *      Acquires the global AuthLock(servPtr) in write mode, updates the
 *      singly-linked list, then releases the lock.
 *----------------------------------------------------------------------
 */
static void *
RegisterAuth(NsServer *servPtr, void *authPtr, void **firstAuthPtr, bool first)
{
    AuthNode *node;

    NS_NONNULL_ASSERT(servPtr    != NULL);
    NS_NONNULL_ASSERT(authPtr    != NULL);
    NS_NONNULL_ASSERT(firstAuthPtr != NULL);

    node = (AuthNode *)authPtr;
    node->nextPtr = NULL;

    AuthLock(servPtr, NS_WRITE);

    if (first) {
        /* Prepend at head */
        node->nextPtr = *firstAuthPtr;
        *firstAuthPtr = authPtr;
    } else {
        /* Append at tail */
        if (*firstAuthPtr == NULL) {
            /* empty list */
            *firstAuthPtr = authPtr;
        } else {
            AuthNode *cursor = (AuthNode *)*firstAuthPtr;
            while (cursor->nextPtr != NULL) {
                cursor = (AuthNode *)cursor->nextPtr;
            }
            cursor->nextPtr = authPtr;
        }
    }

    AuthUnlock(servPtr);
    return authPtr;
}

/*
 * Ns_RegisterAuthorizeRequest --
 *
 *      Register a request‐level authorization callback with the given server.
 *      The callback will be invoked for every incoming request in the order
 *      determined by the 'first' flag.
 *
 * Parameters:
 *      server    - name of the server on which to register the callback (must not be NULL)
 *      proc      - pointer to the authorization function (Ns_AuthorizeRequestProc)
 *      arg       - user data to pass as the first argument to proc on each call
 *      authority - authority label for this callback (used in logging and Tcl results)
 *      first     - if true, prepend this callback to the list; otherwise append
 *
 * Returns:
 *      An opaque handle (RequestAuth *) that can later be passed to
 *      Ns_UnregisterAuthorizeRequest to remove this callback.
 *
 * Side Effects:
 *      Allocates a RequestAuth structure, fills it, and links it into
 *      servPtr->request.firstRequestAuthPtr under AuthLock.
 */

void *
Ns_RegisterAuthorizeRequest(const char *server, Ns_AuthorizeRequestProc *proc, void *arg,
                            const char *authority, bool first)
{
    NsServer    *servPtr = NsGetServer(server);
    RequestAuth *authPtr = ns_malloc(sizeof(RequestAuth));

    authPtr->proc      = proc;
    authPtr->arg       = arg;
    authPtr->authority = ns_strdup(authority);
    authPtr->nextPtr   = NULL;

    return RegisterAuth(servPtr,
                        authPtr,
                        (void **)&servPtr->request.firstRequestAuthPtr,
                        first);
}

/*
 * Ns_RegisterAuthorizeUser --
 *
 *      Register a user‐level authorization callback with the given server.
 *      The callback will be invoked to validate username/password pairs
 *      in the order determined by the 'first' flag.
 *
 * Parameters:
 *      server    - name of the server on which to register the callback (must not be NULL)
 *      proc      - pointer to the user authorization function (Ns_AuthorizeUserProc)
 *      arg       - user data to pass as the first argument to proc on each call
 *      authority - authority label for this callback (used in logging and Tcl results)
 *      first     - if true, prepend this callback to the list; otherwise append
 *
 * Returns:
 *      An opaque handle (UserAuth *) that can later be passed to
 *      Ns_UnregisterAuthorizeUser to remove this callback.
 *
 * Side Effects:
 *      Allocates a UserAuth structure, fills it, and links it into
 *      servPtr->request.firstUserAuthPtr under AuthLock.
 */
void *
Ns_RegisterAuthorizeUser(const char *server, Ns_AuthorizeUserProc *proc, void *arg,
                         const char *authority, bool first)
{
    NsServer *servPtr = NsGetServer(server);
    UserAuth *authPtr = ns_malloc(sizeof(UserAuth));

    authPtr->proc      = proc;
    authPtr->arg       = arg;
    authPtr->authority = ns_strdup(authority);
    authPtr->nextPtr   = NULL;

    return RegisterAuth(servPtr,
                        authPtr,
                        (void **)&servPtr->request.firstUserAuthPtr,
                        first);
}


#if 0
/*
 *----------------------------------------------------------------------
 *
 * Ns_SetRequestAuthorizeProc --
 *
 *      Set the proc to call when authorizing requests.
 *      Replaced by Ns_RegisterAuthorizeRequest() and Ns_RegisterAuthorizeUser()
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
Ns_SetRequestAuthorizeProc(const char *server, Ns_AuthorizeRequestProc *procPtr)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(procPtr != NULL);

    Ns_RegisterAuthorizeRequest(server, procPtr, "setproc", NS_TRUE);
}

void
Ns_SetUserAuthorizeProc(const char *server, Ns_AuthorizeUserProc *procPtr)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(procPtr != NULL);

    Ns_RegisterAuthorizeUser(server, procPtr, "setproc", NS_TRUE);
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * HandleAuthorizationResult --
 *
 *      Take an Ns_ReturnCode from an authorization callback and set the
 *      Tcl interpreter result accordingly.  Recognizes NS_OK, NS_ERROR,
 *      NS_FORBIDDEN and NS_UNAUTHORIZED, mapping them to the strings
 *      "OK", "ERROR", "FORBIDDEN" and "UNAUTHORIZED".  If asDict is true,
 *      wraps the authority label and status into a Tcl dict:
 *
 *          { authority <authority> code <status> }
 *
 *      For other status codes it returns a formatted error message and
 *      forces *result to TCL_ERROR.
 *
 * Returns:
 *      The Tcl return code (*result), updated to TCL_ERROR for unexpected
 *      status values.
 *
 * Side Effects:
 *      Sets interp's result object to the appropriate string or dict.  May
 *      modify *result.
 *
 *----------------------------------------------------------------------
 */
static int
HandleAuthorizationResult(Tcl_Interp *interp, Ns_ReturnCode status, const char *cmdName,
                          const char *authority, bool asDict,
                          const char *arg1, const char *arg2, int *result)
{
    Tcl_Obj *resultObj = NULL;

    /*Ns_Log(Notice, "HandleAuthorizationResult authority %s status %s",
      authority, Ns_ReturnCodeString(status));*/

    switch (status) {
    case NS_OK:
        resultObj = Tcl_NewStringObj("OK", 2);
        break;

    case NS_ERROR:
        resultObj = Tcl_NewStringObj("ERROR", 5);
        break;

    case NS_FORBIDDEN:
        resultObj = Tcl_NewStringObj("FORBIDDEN", 9);
        break;

    case NS_UNAUTHORIZED:
        resultObj = Tcl_NewStringObj("UNAUTHORIZED", 12);
        break;

    case NS_FILTER_RETURN:
        resultObj = Tcl_NewStringObj("FILTER_RETURN", 13);
        break;

    case NS_FILTER_BREAK:  NS_FALL_THROUGH; /* fall through */
    case NS_TIMEOUT:
        Ns_TclPrintfResult(interp, "%s '%s' '%s' returned unexpected result %s from authority %s",
                           cmdName, arg1, arg2, Ns_ReturnCodeString(status), authority);
        *result = TCL_ERROR;
    }

    if (resultObj != NULL) {
        if (asDict) {
            Tcl_Obj *dictObj = Tcl_NewDictObj();

            Tcl_DictObjPut(interp, dictObj,Tcl_NewStringObj("authority", 9),
                           Tcl_NewStringObj(authority, TCL_INDEX_NONE));
            Tcl_DictObjPut(interp, dictObj, Tcl_NewStringObj("code", 4), resultObj);
            resultObj = dictObj;
        }
        /*Ns_Log(Notice, "HandleAuthorizationResult authority %s status %s sets result %s",
          authority, Ns_ReturnCodeString(status), Tcl_GetString(resultObj));*/
        Tcl_SetObjResult(interp, resultObj);
    }

    return *result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclRequestAuthorizeObjCmd --
 *
 *      Implements the "ns_auth request" subcommand (and legacy
 *      "ns_requestauthorize" and "ns_checkurl") for request‐level authorization.
 *      It invokes Ns_AuthorizeRequest(), and returns one of "OK", "ERROR",
 *      "FORBIDDEN" or "UNAUTHORIZED" or a dict of the form
 *
 *                { authority <authority> code <status> }
 *
 *      when the option "-dict" was specified. Unexpected return codes
 *      generate an error.
 *
 * Syntax:
 *      ns_auth request ?-dict? method url user passwd ?peer?
 *
 * Returns:
 *      TCL_OK on success (with result set to one of the four status
 *      codes), or TCL_ERROR if argument parsing fails or an
 *      unexpected Ns_ReturnCode is encountered.
 *
 * Side Effects:
 *      May log deprecation notices and sets the Tcl interpreter result via
 *      HandleAuthorizationResult().
 *
 *----------------------------------------------------------------------
 */
int
NsTclRequestAuthorizeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    int             result = TCL_OK, asDict = 0;
    char           *method, *url, *authuser, *authpasswd, *ipaddr = NULL;
    TCL_SIZE_T      startArg = 2;
    Ns_ObjvSpec opts[] = {
        {"-dict", Ns_ObjvBool, &asDict, INT2PTR(NS_TRUE)},
        {"--",    Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     args[] = {
        {"method",    Ns_ObjvString, &method,     NULL},
        {"url",       Ns_ObjvString, &url,        NULL},
        {"username",  Ns_ObjvString, &authuser,   NULL},
        {"password",  Ns_ObjvString, &authpasswd, NULL},
        {"?ipaddr",   Ns_ObjvString, &ipaddr,     NULL},
        {NULL, NULL, NULL, NULL}
    };

#ifdef NS_WITH_DEPRECATED
    if (strcmp(Tcl_GetString(objv[0]), "ns_checkurl") == 0
        || strcmp(Tcl_GetString(objv[0]), "ns_requestauthorize") == 0
        ) {
        Ns_LogDeprecated(objv, 1, "ns_auth request ...", NULL);
    }
#endif
    if (!STREQ(Tcl_GetString(objv[0]), "ns_auth")) {
        startArg = 1;
    }

    if (Ns_ParseObjv(opts, args, interp, startArg, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_ReturnCode status = NS_OK;
        const char   *authority;
        Conn  conn;
        Ns_Set *auth;

        memcpy(&conn, itPtr->conn, sizeof(Conn));
        conn.request.method = method;
        conn.request.url = url;

        auth = Ns_SetCopy(itPtr->conn->auth);
        Ns_SetUpdateSz(auth, "username", 8, authuser, (TCL_SIZE_T)strlen(authuser));
        Ns_SetUpdateSz(auth, "password", 8, authpasswd, (TCL_SIZE_T)strlen(authpasswd));

        if (ipaddr != NULL) {
            size_t ipaddrLen = strlen(ipaddr);
            if (ipaddrLen+1 >= NS_IPADDR_SIZE) {
                Ns_Log(Warning, "%s: ignore invalid values of provided IP address", ipaddr);
            } else {
                memcpy(conn.peer, ipaddr, ipaddrLen+1);
                memcpy(conn. proxypeer, ipaddr, ipaddrLen+1);
            }
        }

        status = Ns_AuthorizeRequest((Ns_Conn *)&conn, &authority);
        result = HandleAuthorizationResult(interp, status, "ns_auth request",
                                           authority != NULL ? authority : "unknown",
                                           asDict, method, url, &result);
        /*fprintf(stderr, "NsTclRequestAuthorizeObjCmd for %s %s user %s passwd %s peer %s -> %d (%s)\n",
                method, url, authuser, authpasswd, ipaddr == NULL ? "NULL" : ipaddr,
                result , Ns_ReturnCodeString(result));*/
        Ns_SetFree(auth);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclUserAuthorizeObjCmd --
 *
 *      Implements the "ns_auth user" subcommand for script‐level
 *      authentication.  If the optional "-dict" flag was set, it returns a
 *      dict of the form:
 *
 *          { authority <authority> code <status> }
 *
 *      otherwise returns the status code as a plain string. Unexpected return
 *      codes generate an error.
 *
 * Syntax:
 *      ns_auth user ?-dict? username password
 *
 * Returns:
 *      TCL_OK if the command syntax is valid and an expected status
 *      ("OK", "ERROR", "UNAUTHORIZED", or "FORBIDDEN") was produced;
 *      TCL_ERROR if argument parsing fails or an unexpected status
 *      code is encountered.
 *
 * Side Effects:
 *      Sets the Tcl interpreter result via HandleAuthorizationResult().
 *
 *----------------------------------------------------------------------
 */
static int
UserAuthorizeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    int             result = TCL_OK, asDict = 0;
    char           *authuser, *authpasswd;
    Ns_ObjvSpec opts[] = {
        {"-dict", Ns_ObjvBool, &asDict, INT2PTR(NS_TRUE)},
        {"--",    Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec     args[] = {
        {"username",   Ns_ObjvString, &authuser,   NULL},
        {"password", Ns_ObjvString, &authpasswd, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_ReturnCode status = NS_UNAUTHORIZED;
        const char   *authority;

        status = Ns_AuthorizeUser((Ns_Server *)(itPtr->servPtr), authuser, authpasswd, &authority);
        result = HandleAuthorizationResult(interp, status, "ns_auth user",
                                           authority != NULL ? authority : "unknown",
                                           asDict, authuser, authpasswd, &result);
        /*fprintf(stderr, "NsTclUserAuthorizeObjCmd for user %s passwd %s -> %d (%s)\n",
                authuser, authpasswd,
                result , Ns_ReturnCodeString(result));*/
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclAuthObjCmd --
 *
 *      Implements the "ns_auth" Tcl command to its subcommands:
 *      - "request" for request‐level auth, and
 *      - "user" for user‐level auth.
 *
 * Returns:
 *      Standard Tcl result.
 *----------------------------------------------------------------------
 */
int
NsTclAuthObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"request",    NsTclRequestAuthorizeObjCmd},
        {"user",       UserAuthorizeObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsParseAuth --
 *
 *      Parse an HTTP authorization string.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May set the auth Passwd and User connection pointers.
 *
 *----------------------------------------------------------------------
 */

void
NsParseAuth(Conn *connPtr, const char *auth)
{
    register char *p;
    Tcl_DString    authDs;

    NS_NONNULL_ASSERT(connPtr != NULL);
    NS_NONNULL_ASSERT(auth != NULL);

    if (connPtr->auth == NULL) {
        connPtr->auth = Ns_SetCreate(NS_SET_NAME_AUTH);
        connPtr->auth->flags |= NS_SET_OPTION_NOCASE;
    }

    Tcl_DStringInit(&authDs);
    Tcl_DStringAppend(&authDs, auth, TCL_INDEX_NONE);

    p = authDs.string;
    while (*p != '\0' && CHARTYPE(space, *p) == 0) {
        ++p;
    }
    if (*p != '\0') {
        register char *q, *v;
        char           save;

        save = *p;
        *p = '\0';

        if (STRIEQ(authDs.string, "Basic")) {
            size_t     size;
            TCL_SIZE_T userLength;

            (void)Ns_SetPutSz(connPtr->auth, "AuthMethod", 10, "Basic", 5);

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }

            size = strlen(q) + 3u;
            v = ns_malloc(size);
            size = Ns_HtuuDecode(q, (unsigned char *) v, size);
            v[size] = '\0';

            q = strchr(v, INTCHAR(':'));
            if (q != NULL) {
                TCL_SIZE_T pwLength;

                *q++ = '\0';
                pwLength = (TCL_SIZE_T)((v+size) - q);
                (void)Ns_SetPutSz(connPtr->auth, "Password", 8, q, pwLength);
                userLength = (TCL_SIZE_T)size - (pwLength + 1);
            } else {
                userLength = (TCL_SIZE_T)size;
            }
            (void)Ns_SetPutSz(connPtr->auth, "Username", 8, v, userLength);
            ns_free(v);

        } else if (STRIEQ(authDs.string, "Digest")) {
            (void)Ns_SetPutSz(connPtr->auth, "AuthMethod", 10, "Digest", 6);

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }

            while (*q != '\0') {
                size_t idx;
                char   save2;

                p = strchr(q, INTCHAR('='));
                if (p == NULL) {
                    break;
                }
                v = p - 1;
                /* Trim trailing spaces */
                while (v > q && CHARTYPE(space, *v) != 0) {
                    v--;
                }
                /* Remember position */
                save2 = *(++v);
                *v = '\0';
                idx = Ns_SetPutSz(connPtr->auth, q, (TCL_SIZE_T)(v-q), NULL, 0);
                /* Restore character */
                *v = save2;
                /* Skip = and optional spaces */
                p++;
                while (*p != '\0' && CHARTYPE(space, *p) != 0) {
                    p++;
                }
                if (*p == '\0') {
                    break;
                }
                /* Find end of the value, deal with quotes strings */
                if (*p == '"') {
                    for (q = ++p; *q != '\0' && *q != '"'; q++) {
                        ;
                    }
                } else {
                    for (q = p; *q != '\0' && *q != ',' && CHARTYPE(space, *q) == 0; q++) {
                        ;
                    }
                }
                save2 = *q;
                *q = '\0';
                /* Update with current value */
                Ns_SetPutValueSz(connPtr->auth, idx, p, TCL_INDEX_NONE);
                *q = save2;
                /* Advance to the end of the param value, can be end or next name*/
                while (*q != '\0' && (*q == ',' || *q == '"' || CHARTYPE(space, *q) != 0)) {
                    q++;
                }
            }
        } else if (STRIEQ(authDs.string, "Bearer")) {

            (void)Ns_SetPutSz(connPtr->auth, "AuthMethod", 10, "Bearer", 6);

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }
            (void)Ns_SetPutSz(connPtr->auth, "Token", 5, q,
                              (authDs.length - (TCL_SIZE_T)(q - authDs.string)));
        }
        if (p != NULL) {
            *p = save;
        }
    }
    Tcl_DStringFree(&authDs);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_AuthDigestValidate --
 *
 *      Validate an authentication response from digest authentication.  This
 *      function is currently just a dummy placeholder. The function requires
 *      as input the stored (plain text) password, which is not available in
 *      many security systems.
 *
 * Results:
 *      Returns always NS_UNAUTHORIZED.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_AuthDigestValidate(const Ns_Set *UNUSED(auth), const char *UNUSED(storedPwd)) {
    /*
     * Authorization: Digest username="username", realm="realm", nonce="nonce",
     *                uri="uri", response="response", opaque="opaque",
     *                qop="qop", nc="nc", cnonce="cnonce", algorithm="algorithm"
     *
     * response = MD5(MD5(username:realm:password) : nonce : MD5(method:uri)
     *                : [optional qop] : [optional nc] : [optional cnonce])
     *
     * Digest validation needs the following parameters, passed in via the
     * "auth" set.
     *
     *  realm:
     *    The authentication realm used to identify the scope of
     *    the protected resource.
     *
     *  nonce:
     *    A unique value generated by the server for each
     *    authentication request to prevent replay attacks.
     *
     *  uri:
     *    The URI of the requested resource.
     *
     *  method:
     *    The HTTP method (e.g., GET, POST) used in the request.

     *  qop (optional, Quality of Protection):
     *    Specifies what protections are applied (e.g.,
     *    authentication, integrity protection).
     *
     *  nc (optional, Nonce Count):
     *    The count of how many times the
     *    nonce has been used (typically used for replay protection).
     *
     *  cnonce (optional, Client Nonce):
     *    A random value generated  by the client to prevent replay attacks.
     */
#if 0
    /*
     * Sketched Digest Authentication handling (just MD5 here)
     */
#include <openssl/md5.h>
    if (qop != NULL) {
        // Digest Authentication using MD5 hashing
        char hash1[MD5_DIGEST_LENGTH];
        char hash2[MD5_DIGEST_LENGTH];
        char response[MD5_DIGEST_LENGTH * 2];

        // Step 1: Hash1 = MD5(username:realm:password)
        snprintf(buf, NS_ENCRYPT_BUFSIZE, "%s:%s:%s", inputPwd, realm, storedPwd);
        MD5_CTX ctx;
        MD5_Init(&ctx);
        MD5_Update(&ctx, buf, strlen(buf));
        MD5_Final((unsigned char *)hash1, &ctx);

        // Step 2: Hash2 = MD5(method:uri)
        snprintf(buf, NS_ENCRYPT_BUFSIZE, "%s:%s", method, uri);
        MD5_Init(&ctx);
        MD5_Update(&ctx, buf, strlen(buf));
        MD5_Final((unsigned char *)hash2, &ctx);

        // Step 3: Create Digest response
        if (STREQ(qop, "auth")) {
            // qop = 'auth', include nonce, nc, cnonce in response calculation
            snprintf(buf, NS_ENCRYPT_BUFSIZE, "%s:%s:%s:%s:%s:%s",
                     hash1, nonce, nc, cnonce, hash2);
        } else {
            // qop is null or another value, just use the basic hash
            snprintf(buf, NS_ENCRYPT_BUFSIZE, "%s:%s:%s", hash1, nonce, hash2);
        }

        // Final MD5 hash of the response string
        MD5_Init(&ctx);
        MD5_Update(&ctx, buf, strlen(buf));
        MD5_Final((unsigned char *)response, &ctx);

        // Compare the calculated response to the expected response (passed by the server)
        if (STREQ(response, storedPwd)) {
            return NS_OK;  // Password match
        } else {
            // Password mismatch
            goto deny;
        }
    }
#endif
    return NS_UNAUTHORIZED;
}



/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
