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
 * tclrequest.c --
 *
 *      Routines for Tcl proc, filter and ADP registered requests.
 */

#include "nsd.h"

/*
 * Static variables defined in this file.
 */

static Ns_ObjvTable filters[] = {
    {"preauth",  (unsigned int)NS_FILTER_PRE_AUTH},
    {"postauth", (unsigned int)NS_FILTER_POST_AUTH},
    {"trace",    (unsigned int)NS_FILTER_TRACE},
    {NULL, 0u}
};


/*
 * Authorization types
 */
typedef enum {
    AUTH_TYPE_REQUEST,  /* Request-level authorization */
    AUTH_TYPE_USER      /* User-level authorization */
} AuthType;


static Ns_ObjvTable authprocs[] = {
    {"request",  (unsigned int)AUTH_TYPE_REQUEST},
    {"user",     (unsigned int)AUTH_TYPE_USER},
    {NULL, 0u}
};

static Ns_ReturnCode EvalTclAuthCallback(const Ns_TclCallback *cbPtr, AuthType authType, void *arg,
                                         const char *username, const char *password, int *continuation)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(6);


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclRequest --
 *
 *      Dummy up a direct call to NsTclRequestProc for a connection.
 *
 * Results:
 *      See NsTclRequest.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclRequest(Ns_Conn *conn, const char *name)
{
    Ns_TclCallback cb;

    cb.cbProc = (ns_funcptr_t) NsTclRequestProc;
    cb.server = Ns_ConnServer(conn);
    cb.script = name;
    cb.argc   = 0;
    cb.argv   = NULL;

    return NsTclRequestProc(&cb, conn);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterProcObjCmd --
 *
 *      Implements "ns_register_proc".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterProcObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj      *scriptObj;
    char         *method, *url;
    TCL_SIZE_T    remain = 0;
    int           noinherit = 0, result = TCL_OK;
    NsUrlSpaceContextSpec *specPtr = NULL;
    Ns_ObjvSpec   opts[] = {
        {"-constraints", Ns_ObjvUrlspaceSpec, &specPtr, NULL},
        {"-noinherit",     Ns_ObjvBool,        &noinherit,    INT2PTR(NS_TRUE)},
        {"--",             Ns_ObjvBreak,       NULL,          NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   args[] = {
        {"method",     Ns_ObjvString, &method,    NULL},
        {"url",        Ns_ObjvString, &url,       NULL},
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?arg",       Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp  *itPtr = clientData;
        Ns_TclCallback  *cbPtr;
        unsigned int     flags = 0u;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }
        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclRequestProc, scriptObj,
                                  remain, objv + ((TCL_SIZE_T)objc - remain));
        result = Ns_RegisterRequest2(interp, itPtr->servPtr->server, method, url,
                                     NsTclRequestProc, Ns_TclFreeCallback, cbPtr, flags, specPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterProxyObjCmd --
 *
 *      Implements "ns_register_proxy".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterProxyObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    Tcl_Obj        *scriptObj;
    char           *method, *protocol;
    TCL_SIZE_T      remain = 0;
    int             result = TCL_OK;

    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method,    NULL},
        {"protocol",   Ns_ObjvString, &protocol,  NULL},
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?arg",       Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_TclCallback *cbPtr;

        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclRequestProc,
                                  scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
        Ns_RegisterProxyRequest(itPtr->servPtr->server, method, protocol,
                                NsTclRequestProc, Ns_TclFreeCallback, cbPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterFastPathObjCmd --
 *
 *      Implements "ns_register_fastpath".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterFastPathObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *method, *url;
    int         noinherit = 0, result = TCL_OK;
    NsUrlSpaceContextSpec *specPtr = NULL;
    Ns_ObjvSpec opts[] = {
        {"-constraints", Ns_ObjvUrlspaceSpec, &specPtr, NULL},
        {"-noinherit",     Ns_ObjvBool,        &noinherit,    INT2PTR(NS_OP_NOINHERIT)},
        {"--",             Ns_ObjvBreak,       NULL,          NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method, NULL},
        {"url",        Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        unsigned int    flags = 0u;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }

        result = Ns_RegisterRequest2(interp, itPtr->servPtr->server, method, url,
                                     Ns_FastPathProc, NULL, NULL, flags, specPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUnRegisterObjCmd --
 *
 *      Implements "ns_unregister_op".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Depends on any delete callbacks.
 *
 *----------------------------------------------------------------------
 */

int
NsTclUnRegisterOpObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char           *method = NULL, *url = NULL;
    int             noinherit = 0, recurse = 0, allconstraints = 0, result = TCL_OK;
    const NsInterp *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    Ns_ObjvSpec opts[] = {
        {"-allconstraints",    Ns_ObjvBool,   &allconstraints, INT2PTR(NS_OP_ALLCONSTRAINTS)},
        {"-noinherit",         Ns_ObjvBool,   &noinherit,      INT2PTR(NS_OP_NOINHERIT)},
        {"-recurse",           Ns_ObjvBool,   &recurse,        INT2PTR(NS_OP_RECURSE)},
        {"-server",            Ns_ObjvServer, &servPtr,        NULL},
        {"--",                 Ns_ObjvBreak,  NULL,            NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",   Ns_ObjvString, &method, NULL},
        {"url",      Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_UnRegisterRequestEx(servPtr->server, method, url,
                               ((unsigned int)noinherit | (unsigned int)recurse | (unsigned int) allconstraints));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterFilterObjCmd --
 *
 *      Implements "ns_register_filter".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterFilterObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char         *method, *urlPattern;
    Tcl_Obj      *scriptObj;
    TCL_SIZE_T    remain = 0;
    int           first = (int)NS_FALSE, result = TCL_OK;
    unsigned int  when = 0u;
    NsUrlSpaceContextSpec *specPtr = NULL;
    Ns_ObjvSpec opts[] = {
        {"-constraints", Ns_ObjvUrlspaceSpec, &specPtr, NULL},
        {"-first", Ns_ObjvBool,  &first, INT2PTR(NS_TRUE)},
        {"--",     Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   args[] = {
        {"when",       Ns_ObjvIndex,  &when,       filters},
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlpattern", Ns_ObjvString, &urlPattern, NULL},
        {"script",     Ns_ObjvObj,    &scriptObj,  NULL},
        {"?arg",       Ns_ObjvArgs,   &remain,     NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp  *itPtr = clientData;
        Ns_TclCallback  *cbPtr;

        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclFilterProc,
                                  scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
        (void)Ns_RegisterFilter2(itPtr->servPtr->server, method, urlPattern,
                                 NsTclFilterProc, (Ns_FilterType)when, cbPtr, (bool)first,
                                 specPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclShortcutFilterObjCmd --
 *
 *      Implements "ns_shortcut_filter".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Other filters that also match when+method+urlPattern will not run.
 *
 *----------------------------------------------------------------------
 */

int
NsTclShortcutFilterObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char           *method, *urlPattern;
    unsigned int    when = 0u;
    int             result = TCL_OK;
    NsUrlSpaceContextSpec *specPtr = NULL;
    Ns_ObjvSpec opts[] = {
        {"-constraints", Ns_ObjvUrlspaceSpec, &specPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"when",       Ns_ObjvIndex,  &when,       filters},
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlpattern", Ns_ObjvString, &urlPattern, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        const char     *server = itPtr->servPtr->server;

        (void)Ns_RegisterFilter2(server, method, urlPattern,
                                 NsShortcutFilterProc, (Ns_FilterType)when, NULL, NS_FALSE,
                                 specPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterTraceObjCmd --
 *
 *      Implements "ns_register_trace".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRegisterTraceObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *method, *urlPattern;
    Tcl_Obj    *scriptObj;
    TCL_SIZE_T  remain = 0;
    int         result = TCL_OK;
    NsUrlSpaceContextSpec *specPtr = NULL;
    Ns_ObjvSpec opts[] = {
        {"-constraints", Ns_ObjvUrlspaceSpec, &specPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlpattern", Ns_ObjvString, &urlPattern, NULL},
        {"script",     Ns_ObjvObj,    &scriptObj,  NULL},
        {"?arg",       Ns_ObjvArgs,   &remain,     NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        Ns_TclCallback *cbPtr;

        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclFilterProc,
                                  scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
        (void)Ns_RegisterFilter2(itPtr->servPtr->server, method, urlPattern,
                                 NsTclFilterProc, NS_FILTER_VOID_TRACE, cbPtr, NS_FALSE,
                                 specPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterAuthObjCmd --
 *
 *      Implements "ns_register_auth".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */
int
NsTclRegisterAuthObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj      *scriptObj;
    TCL_SIZE_T    remain = 0;
    int           first = (int)NS_FALSE, result = TCL_OK;
    const char   *authority = NULL;
    unsigned int  proctype = 0u;
    Ns_ObjvSpec opts[] = {
        {"-authority", Ns_ObjvString, &authority, NULL},
        {"-first",     Ns_ObjvBool,   &first,     INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   args[] = {
        {"type",       Ns_ObjvIndex,  &proctype,   authprocs},
        {"script",     Ns_ObjvObj,    &scriptObj,  NULL},
        {"?arg",       Ns_ObjvArgs,   &remain,     NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp  *itPtr = clientData;
        Ns_TclCallback  *cbPtr;

        if (authority == NULL) {
            authority = Tcl_GetString(scriptObj);
        }
        switch (proctype) {
        case AUTH_TYPE_REQUEST: {
            cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclAuthorizeRequestProc,
                                      scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
            Ns_RegisterAuthorizeRequest(itPtr->servPtr->server, NsTclAuthorizeRequestProc,
                                        cbPtr, authority, first);
            break;
        }
        case AUTH_TYPE_USER: {
            cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclAuthorizeUserProc,
                                      scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
            Ns_RegisterAuthorizeUser(itPtr->servPtr->server, NsTclAuthorizeUserProc,
                                     cbPtr, authority, first);
            break;
        }
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclRequestProc --
 *
 *      Ns_OpProc for Tcl operations.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      '500 Internal Server Error' sent to client if Tcl script
 *      fails, or '503 Service Unavailable' on an NS_TIMEOUT exception.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsTclRequestProc(const void *arg, Ns_Conn *conn)
{
    const Ns_TclCallback *cbPtr = arg;
    Tcl_Interp           *interp;
    Tcl_DString           ds;
    Ns_ReturnCode         status = NS_OK;

    NS_NONNULL_ASSERT(conn != NULL);

    interp = Ns_GetConnInterp(conn);
    if (Ns_TclEvalCallback(interp, cbPtr, NULL, NS_SENTINEL) != TCL_OK) {
        if (NsTclTimeoutException(interp) == NS_TRUE) {
            Tcl_DStringInit(&ds);
            Ns_GetProcInfo(&ds, (ns_funcptr_t)NsTclRequestProc, arg);
            Ns_Log(Dev, "%s: %s", ds.string, Tcl_GetStringResult(interp));
            Ns_Log(Ns_LogTimeoutDebug, "Tcl request %s lead to a timeout: %s", conn->request.line, ds.string);
            Tcl_DStringFree(&ds);
            Tcl_ResetResult(interp);
            status = Ns_ConnReturnUnavailable(conn);
        } else {
            (void) Ns_TclLogErrorInfo(interp, "\n(context: request proc)");
            if (!Ns_ConnIsClosed(conn)) {
                status = Ns_ConnReturnInternalError(conn);
            }
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclFilterProc --
 *
 *      The callback for Tcl filters. Run the script.
 *
 * Results:
 *      NS_OK, NS_FILTER_RETURN, or NS_FILTER_BREAK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsTclFilterProc(const void *arg, Ns_Conn *conn, Ns_FilterType why)
{
    const Ns_TclCallback *cbPtr = arg;
    Tcl_DString           ds;
    Tcl_Interp           *interp;
    int                   rc;
    TCL_SIZE_T            ii;
    const char           *result;
    Ns_ReturnCode         status;

    interp = Ns_GetConnInterp(conn);
    Tcl_DStringInit(&ds);

    /*
     * Append the command
     */

    Tcl_DStringAppend(&ds, cbPtr->script, TCL_INDEX_NONE);

    /*
     * Append the 'why' argument
     */

    switch (why) {
    case NS_FILTER_PRE_AUTH:
        Tcl_DStringAppendElement(&ds, "preauth");
        break;
    case NS_FILTER_POST_AUTH:
        Tcl_DStringAppendElement(&ds, "postauth");
        break;
    case NS_FILTER_TRACE:
        Tcl_DStringAppendElement(&ds, "trace");
        break;
    case NS_FILTER_VOID_TRACE:
        /*
         * The filter was registered with ns_register_trace; always type VOID
         * TRACE, so add the filter reason as extra argument of the registered
         * callback proc.
         */
        break;
    }

    /*
     * Append optional arguments
     */

    for (ii = 0; ii < cbPtr->argc; ii++) {
        Tcl_DStringAppendElement(&ds, cbPtr->argv[ii]);
    }

    /*
     * Run the script.
     */

    Tcl_AllowExceptions(interp);
    rc = Tcl_EvalEx(interp, ds.string, ds.length, 0);
    result = Tcl_GetStringResult(interp);
    Tcl_DStringSetLength(&ds, 0);

    if (rc == TCL_ERROR) {

        /*
         * Handle Tcl errors and timeouts.
         */

        if (NsTclTimeoutException(interp) == NS_TRUE) {
            Ns_GetProcInfo(&ds, (ns_funcptr_t)NsTclFilterProc, arg);
            Ns_Log(Dev, "%s: %s", ds.string, result);
            Ns_Log(Ns_LogTimeoutDebug, "filter proc '%s' ends with timeout exception", ds.string);
            (void) Ns_ConnReturnUnavailable(conn);
            Tcl_ResetResult(interp);
            status = NS_FILTER_RETURN;
        } else {
            (void) Ns_TclLogErrorInfo(interp, "\n(context: filter proc)");
            status = NS_ERROR;
        }
    } else {

        /*
         * Determine the filter result code.
         */

        if (why == NS_FILTER_VOID_TRACE) {
            status = NS_OK;

        } else if (rc == TCL_CONTINUE || STREQ(result, "filter_ok")) {
            status = NS_OK;

        } else if (rc == TCL_BREAK    || STREQ(result, "filter_break")) {
            status = NS_FILTER_BREAK;

        } else if (rc == TCL_RETURN   || STREQ(result, "filter_return")) {
            status = NS_FILTER_RETURN;

        } else {
            Ns_Log(Error, "ns:tclfilter: %s returns invalid result: %s",
                   cbPtr->script, result);
            status = NS_ERROR;
        }
    }
    Tcl_DStringFree(&ds);

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * EvalTclAuthCallback --
 *
 *      Builds and invokes a Tcl-based authorization callback. Constructs a Tcl
 *      command by concatenating the registered script, a fixed list of
 *      arguments (such as method, URL, user, password, etc.), and any extra
 *      arguments provided at registration. Runs the script in the supplied
 *      interpreter and stores the raw Tcl return code in *continuationPtr.
 *      The Tcl result string is mapped to an Ns_ReturnCode.
 *
 * Parameters:
 *      cbPtr           - Pointer to the Ns_TclCallback holding script & arguments.
 *      authType        - The type of authorization (request or user).
 *      arg             - Connection (for request-level arguments) or interp.
 *      username        - Username for authorization.
 *      password        - Password for authorization.
 *      continuationPtr - Receives the raw Tcl return code (TCL_OK, TCL_ERROR, etc.).
 *
 * Results:
 *      NS_OK           if the script returned "OK".
 *      NS_FORBIDDEN    if the script returned "FORBIDDEN".
 *      NS_UNAUTHORIZED if the script returned "UNAUTHORIZED".
 *      NS_ERROR        on Tcl evaluation error or unexpected script result.
 *
 * Side Effects:
 *      Logs a warning on script errors or unexpected return values.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
EvalTclAuthCallback(const Ns_TclCallback *cbPtr, AuthType authType, void *arg,
                    const char *username, const char *password, int *continuation)
{
    Tcl_DString    ds;
    int            rc;
    const char    *result;
    TCL_SIZE_T     i;
    Ns_ReturnCode  status;
    Ns_Conn       *conn;
    Tcl_Interp    *interp;

    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, cbPtr->script, TCL_INDEX_NONE);

    if (authType == AUTH_TYPE_USER) {
        conn = NULL;
        interp = arg;
        /*
         * Append username and password for user-level authorization
         */
        Tcl_DStringAppendElement(&ds, username);
        Tcl_DStringAppendElement(&ds, password);
    } else if (authType == AUTH_TYPE_REQUEST) {
        conn = arg;
        interp = Ns_GetConnInterp(conn);
        /*
         * Append method, URL, username, password, and peer for request-level authorization
         */
        Tcl_DStringAppendElement(&ds, conn->request.method);
        Tcl_DStringAppendElement(&ds, conn->request.url);
        Tcl_DStringAppendElement(&ds, username);
        Tcl_DStringAppendElement(&ds, password);
        Tcl_DStringAppendElement(&ds, Ns_ConnConfiguredPeerAddr(conn));
    }

    /*
     * Append additional arguments from the registration
     */
    for (i = 0; i < cbPtr->argc; ++i) {
        Tcl_DStringAppendElement(&ds, cbPtr->argv[i]);
    }

    /*
     * Evaluate the Tcl script
     */
    Tcl_AllowExceptions(interp);
    rc = Tcl_EvalEx(interp, ds.string, ds.length, 0);
    result = Tcl_GetStringResult(interp);
    Tcl_DStringFree(&ds);

    /*
     *  Handle result and map to Ns_ReturnCode
     */
    if (rc == TCL_ERROR) {
        Ns_Log(Warning, "authorize script error: %s", result);
        status = NS_ERROR;
    } else if (STRIEQ(result, "OK")) {
        status = NS_OK;
    } else if (STRIEQ(result, "UNAUTHORIZED")) {
        status = NS_UNAUTHORIZED;
    } else if (STRIEQ(result, "FORBIDDEN")) {
        status = NS_FORBIDDEN;
    } else {
        Ns_Log(Warning, "authorize script returned unexpected: %s", result);
        status = NS_ERROR;
    }

    *continuation = rc;

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclAuthorizeRequestProc --
 *
 *      Adapter function for request-level authorization. It uses username and
 *      password from the connection and delegates to
 *      EvalTclAuthCallback. Returns the raw Tcl return code to control
 *      whether further callbacks should run.
 *
 * Results:
 *      returns the Ns_ReturnCode from EvalTclAuthCallback
 *
 * Side Effects:
 *      Allocates and deallocates a Tcl interpreter per invocation.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
NsTclAuthorizeRequestProc(void *arg, Ns_Conn *conn, int *continuationPtr)
{
    const Ns_TclCallback *cbPtr = arg;

    return EvalTclAuthCallback(cbPtr, AUTH_TYPE_REQUEST, conn,
                               conn->auth != NULL ? Ns_SetGet(conn->auth, "username") : "",
                               conn->auth != NULL ? Ns_SetGet(conn->auth, "password") : "",
                               continuationPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclAuthorizeUserProc --
 *
 *      Adapter function for user-level password checks. Allocates a Tcl
 *      interpreter, packs the user and password into arguments, then delegates
 *      to EvalTclAuthCallback. Returns the raw Tcl return code to control
 *      whether further callbacks should run.
 *
 * Results:
 *      Returns the Ns_ReturnCode from EvalTclAuthCallback.
 *
 * Side Effects:
 *      Allocates and deallocates a Tcl interpreter for the execution.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
NsTclAuthorizeUserProc(void *arg, const Ns_Server *servPtr,
                       const char *username, const char *password, int *continuationPtr)
{
    const Ns_TclCallback *cbPtr = arg;
    Tcl_Interp           *interp = NsTclAllocateInterp((NsServer*)servPtr);
    Ns_ReturnCode         status;

    status = EvalTclAuthCallback(cbPtr, AUTH_TYPE_USER, interp, username, password, continuationPtr);
    Ns_TclDeAllocateInterp(interp);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsShortcutFilterProc --
 *
 *      The callback for Tcl shortcut filters.
 *
 * Results:
 *      Always NS_FILTER_BREAK.
 *
 * Side effects:
 *      No other filters of this type will run for this connection.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
NsShortcutFilterProc(const void *UNUSED(arg), Ns_Conn *UNUSED(conn), Ns_FilterType UNUSED(why))
{
    return NS_FILTER_BREAK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclTimeoutException --
 *
 *      Check for an NS_TIMEOUT exception in the Tcl errorCode variable.
 *
 * Results:
 *      NS_TRUE if there is an exception, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
NsTclTimeoutException(Tcl_Interp *interp)
{
    bool        isException = NS_FALSE;
    const char *errorCode;

    NS_NONNULL_ASSERT(interp != NULL);

    errorCode = Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
    if (strncmp(errorCode, "NS_TIMEOUT", 10u) == 0) {
        isException = NS_TRUE;
    }
    return isException;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
