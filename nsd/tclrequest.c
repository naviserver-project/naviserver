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

static const char *RCSID = "@(#) $Header$, compiled: " __DATE__ " " __TIME__;

#include "nsd.h"


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
 *      Dummy up a direct call to NsTclRequest for a connection.
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
Ns_TclRequest(Ns_Conn *conn, char *name)
{
    Ns_TclCallback cb;

    cb.cbProc        = &NsTclRequest;
    cb.server        = Ns_ConnServer(conn);
    cb.script        = name;
    cb.scriptarg     = NULL;

    return NsTclRequest(&cb, conn);
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
    char           *method, *url, *script, *scriptarg = "";
    int             flags = 0;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &flags, (void *) NS_OP_NOINHERIT},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method,    NULL},
        {"url",        Ns_ObjvString, &url,       NULL},
        {"script",     Ns_ObjvString, &script,    NULL},
        {"?arg",       Ns_ObjvString, &scriptarg, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, NsTclRequest, script, scriptarg);
    Ns_RegisterRequest(itPtr->servPtr->server, method, url,
                       NsTclRequest, Ns_TclFreeCallback, cbPtr, flags);

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
    char       *method, *url, *file;
    int         flags = 0;

    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &flags, (void *) NS_OP_NOINHERIT},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",   Ns_ObjvString, &method,   NULL},
        {"url",      Ns_ObjvString, &url,      NULL},
        {"file",     Ns_ObjvString, &file,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_RegisterRequest(itPtr->servPtr->server, method, url,
                       NsAdpRequest, ns_free, ns_strdup(file), flags);

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
    int       flags = 0;
    
    Ns_ObjvSpec opts[] = {
        {"-noinherit", Ns_ObjvBool,  &flags, (void *) NS_OP_NOINHERIT},
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

    Ns_UnRegisterRequest(itPtr->servPtr->server, method, url, flags);

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
    char            *method, *urlPattern, *script, *scriptarg = "";
    int              when = 0;

    Ns_ObjvSpec args[] = {
        {"when",       Ns_ObjvFlags,  &when,       filters},
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlPattern", Ns_ObjvString, &urlPattern, NULL},
        {"script",     Ns_ObjvString, &script,     NULL},
        {"?arg",       Ns_ObjvString, &scriptarg,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, NsTclFilter, script, scriptarg);
    Ns_RegisterFilter(itPtr->servPtr->server, method, urlPattern,
                      NsTclFilter, when, cbPtr);

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
    char           *method, *urlPattern, *script, *scriptarg = NULL;

    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method,     NULL},
        {"urlPattern", Ns_ObjvString, &urlPattern, NULL},
        {"script",     Ns_ObjvString, &script,     NULL},
        {"?arg",       Ns_ObjvString, &scriptarg,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, NsTclFilter, script, scriptarg);
    Ns_RegisterFilter(itPtr->servPtr->server, method, urlPattern,
                      NsTclFilter, NS_FILTER_VOID_TRACE, cbPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsAdpRequest --
 *
 *      Ns_OpProc for registered ADP's.
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
NsAdpRequest(void *arg, Ns_Conn *conn)
{
    return Ns_AdpRequest(conn, (char *) arg);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRequst --
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
NsTclRequest(void *arg, Ns_Conn *conn)
{
    Ns_TclCallback *cbPtr = arg;
    Tcl_Interp     *interp;

    interp = Ns_GetConnInterp(conn);
    if (Ns_TclEvalCallback(interp, cbPtr, NULL, NULL) != TCL_OK) {
        Ns_TclLogError(interp);
        return Ns_ConnReturnInternalError(conn);
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclFilter --
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
NsTclFilter(void *arg, Ns_Conn *conn, int why)
{
    Ns_TclCallback      *cbPtr = arg;
    Tcl_DString          cmd;
    Tcl_Interp          *interp;
    int                  status;
    CONST char          *result;

    interp = Ns_GetConnInterp(conn);
    Tcl_DStringInit(&cmd);

    /*
     * Start building the command with the script and arg.
     */

    Tcl_DStringAppendElement(&cmd, cbPtr->script);
    Tcl_DStringAppendElement(&cmd, cbPtr->scriptarg);

    /*
     * Append the 'why'
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
        Ns_Log(Warning, "tclfilter: %s return invalid result: %s",
               cbPtr->script, result);
        status = NS_ERROR;
    }
    Tcl_DStringFree(&cmd);

    return status;
}
