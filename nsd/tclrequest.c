/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.com/.
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
 * tclrequest.c --
 *
 *      Routines for Tcl proc, filter and ADP registered requests.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


/*
 * Static variables defined in this file.
 */

static Ns_ObjvTable filters[] = {
    {"preauth",  NS_FILTER_PRE_AUTH},
    {"postauth", NS_FILTER_POST_AUTH},
    {"trace",    NS_FILTER_TRACE},
    {NULL, 0}
};


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

int
Ns_TclRequest(Ns_Conn *conn, CONST char *name)
{
    Ns_TclCallback cb;

    cb.cbProc = &NsTclRequestProc;
    cb.server = Ns_ConnServer(conn);
    cb.script = (char *) name;
    cb.argc   = 0;
    cb.argv   = NULL;

    return NsTclRequestProc(&cb, conn);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterProcObjCmd --
 *
 *      Implements ns_register_proc as obj command. 
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
NsTclRegisterProcObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                        Tcl_Obj *CONST objv[], int adp)
{
    NsInterp       *itPtr = arg;
    Ns_TclCallback *cbPtr;
    Tcl_Obj        *scriptObj;
    char           *method, *url;
    int             flags = 0, remain = 0;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &flags, (void *) NS_OP_NOINHERIT},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method,    NULL},
        {"url",        Ns_ObjvString, &url,       NULL},
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?args",      Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, NsTclRequestProc, scriptObj,
                              remain, objv + (objc - remain));
    Ns_RegisterRequest(itPtr->servPtr->server, method, url,
                       NsTclRequestProc, Ns_TclFreeCallback, cbPtr, flags);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterProxyObjCmd --
 *
 *      Implements ns_register_proxy as obj command. 
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
NsTclRegisterProxyObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                        Tcl_Obj *CONST objv[], int adp)
{
    NsInterp       *itPtr = arg;
    Ns_TclCallback *cbPtr;
    Tcl_Obj        *scriptObj;
    char           *method, *protocol;
    int             remain = 0;

    Ns_ObjvSpec opts[] = {
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method,    NULL},
        {"protocol",   Ns_ObjvString, &protocol,  NULL},
        {"script",     Ns_ObjvObj,    &scriptObj, NULL},
        {"?args",      Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, NsTclRequestProc, scriptObj,
                              remain, objv + (objc - remain));
    Ns_RegisterProxyRequest(itPtr->servPtr->server, method, protocol,
                       NsTclRequestProc, Ns_TclFreeCallback, cbPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterFastPathObjCmd --
 *
 *      Implements ns_register_fastpath as obj command.
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
NsTclRegisterFastPathObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                            Tcl_Obj *CONST objv[], int adp)
{
    NsInterp       *itPtr = arg;
    char           *method, *url;
    int             flags = 0;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &flags, (void *) NS_OP_NOINHERIT},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method, NULL},
        {"url",        Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_RegisterRequest(itPtr->servPtr->server, method, url,
                       NsFastPathProc, NULL, itPtr->servPtr, flags);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterAdpObjCmd --
 *
 *      Implements ns_register_adp as obj command.
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
NsTclRegisterAdpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp   *itPtr = arg;
    char       *method, *url, *file = NULL;
    int         flags = 0;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &flags, (void *) NS_OP_NOINHERIT},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",   Ns_ObjvString, &method,   NULL},
        {"url",      Ns_ObjvString, &url,      NULL},
        {"?file",     Ns_ObjvString, &file,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (file != NULL) {
        Ns_RegisterRequest(itPtr->servPtr->server, method, url,
                           NsAdpMapProc, ns_free, ns_strdup(file), flags);
    } else {
        Ns_RegisterRequest(itPtr->servPtr->server, method, url,
                           NsAdpRequestProc, NULL, itPtr->servPtr, flags);
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUnRegisterObjCmd --
 *
 *      Implements ns_unregister_proc and ns_unregister_adp commands.
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
NsTclUnRegisterObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    char     *method, *url;
    int       noinherit = 0, recurse = 0;
    
    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &noinherit, (void *) NS_OP_NOINHERIT},
        {"-recurse",   Ns_ObjvBool,  &recurse,   (void *) NS_OP_RECURSE},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",   Ns_ObjvString, &method, NULL},
        {"url",      Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    Ns_UnRegisterRequestEx(itPtr->servPtr->server, method, url, noinherit | recurse);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterFilterObjCmd --
 *
 *      Implements ns_register_filter. 
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
NsTclRegisterFilterObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp        *itPtr = arg;
    Ns_TclCallback  *cbPtr;
    char            *method, *urlPattern;
    Tcl_Obj         *scriptObj;
    int              remain = 0, when = 0;

    Ns_ObjvSpec opts[] = {
        {"-first", Ns_ObjvBool,  &when, (void *) NS_FILTER_FIRST},
        {"--",     Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"when",       Ns_ObjvFlags,  &when,       filters},
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlPattern", Ns_ObjvString, &urlPattern, NULL},
        {"script",     Ns_ObjvObj,    &scriptObj,  NULL},
        {"?args",      Ns_ObjvArgs,   &remain,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, NsTclFilterProc, scriptObj, 
                              remain, objv + (objc - remain));
    Ns_RegisterFilter(itPtr->servPtr->server, method, urlPattern,
                      NsTclFilterProc, when, cbPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclShortcutFilterObjCmd --
 *
 *      Implements ns_shortcut_filter.
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
NsTclShortcutFilterObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp  *itPtr = arg;
    char      *server = itPtr->servPtr->server;
    char      *method, *urlPattern;
    int        when = NS_FILTER_FIRST;

    Ns_ObjvSpec args[] = {
        {"when",       Ns_ObjvFlags,  &when,       filters},
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlPattern", Ns_ObjvString, &urlPattern, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_RegisterFilter(server, method, urlPattern,
                      NsShortcutFilterProc, when, NULL);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRegisterTraceObjCmd --
 *
 *      Implements ns_register_trace as obj command.
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
NsTclRegisterTraceObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp       *itPtr = arg;
    Ns_TclCallback *cbPtr;
    char           *method, *urlPattern;
    Tcl_Obj        *scriptObj;
    int             remain = 0;

    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlPattern", Ns_ObjvString, &urlPattern, NULL},
        {"script",     Ns_ObjvObj,    &scriptObj,  NULL},
        {"?args",      Ns_ObjvArgs,   &remain,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, NsTclFilterProc, scriptObj, 
                              remain, objv + (objc - remain));
    Ns_RegisterFilter(itPtr->servPtr->server, method, urlPattern,
                      NsTclFilterProc, NS_FILTER_VOID_TRACE, cbPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpMapProc --
 *
 *      Ns_OpProc which evaluates a specifically registered adp file.
 *
 * Results:
 *      See Ns_AdpRequest.
 *
 * Side effects:
 *      None. 
 *
 *----------------------------------------------------------------------
 */

int
NsAdpMapProc(void *arg, Ns_Conn *conn)
{
    char *adpFile = arg;

    return Ns_AdpRequest(conn, adpFile);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRequstProc --
 *
 *      Ns_OpProc for Tcl operations.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      '500 Internal Server Error' sent to client if Tcl script fails.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRequestProc(void *arg, Ns_Conn *conn)
{
    Ns_TclCallback *cbPtr = arg;
    Tcl_Interp     *interp;

    interp = Ns_GetConnInterp(conn);
    if (Ns_TclEvalCallback(interp, cbPtr, NULL, NULL) != TCL_OK) {
        return Ns_ConnReturnInternalError(conn);
    }

    return NS_OK;
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

int
NsTclFilterProc(void *arg, Ns_Conn *conn, int why)
{
    Ns_TclCallback      *cbPtr = arg;
    Tcl_DString          cmd;
    Tcl_Interp          *interp;
    int                  ii, status;
    CONST char          *result;
    
    interp = Ns_GetConnInterp(conn);
    Tcl_DStringInit(&cmd);
    
    /*
     * Append the command
     */
    
    Tcl_DStringAppend(&cmd, cbPtr->script, -1);
    
    /*
     * Append the 'why' argument
     */

    switch (why) {
    case NS_FILTER_PRE_AUTH:
        Tcl_DStringAppendElement(&cmd, "preauth");
        break;
    case NS_FILTER_POST_AUTH:
        Tcl_DStringAppendElement(&cmd, "postauth");
        break;
    case NS_FILTER_TRACE:
        Tcl_DStringAppendElement(&cmd, "trace");
        break;
    case NS_FILTER_VOID_TRACE:
        /* Registered with ns_register_trace; always type VOID TRACE, so don't append. */
        break;
    }

    /*
     * Append optional arguments
     */

    for (ii = 0; ii < cbPtr->argc; ii++) {
        Tcl_DStringAppendElement(&cmd, cbPtr->argv[ii]);
    }

    /*
     * Run the script.
     */
    
    Tcl_AllowExceptions(interp);
    status = Tcl_EvalEx(interp, cmd.string, cmd.length, 0);
    if (status != TCL_OK) {
        Ns_TclLogError(interp);
    }

    /*
     * Determine the filter result code.
     */

    result = Tcl_GetStringResult(interp);
    if (why == NS_FILTER_VOID_TRACE) {
        status = NS_OK;
    } else if (status != TCL_OK) {
        status = NS_ERROR;
    } else if (STREQ(result, "filter_ok")) {
        status = NS_OK;
    } else if (STREQ(result, "filter_break")) {
        status = NS_FILTER_BREAK;
    } else if (STREQ(result, "filter_return")) {
        status = NS_FILTER_RETURN;
    } else {
        Ns_Log(Error, "tclfilter: %s return invalid result: %s",
               cbPtr->script, result);
        status = NS_ERROR;
    }
    Tcl_DStringFree(&cmd);

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

int
NsShortcutFilterProc(void *arg, Ns_Conn *conn, int why)
{
    return NS_FILTER_BREAK;
}
