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
 * tclcallbacks.c --
 *
 *      Support for executing Tcl code in response to a callback event.
 *
 */

#include "ns.h"

NS_RCSID("@(#) $Header$");


typedef void *(AtProc)(Ns_Callback *, void *);


/*
 * Local functions defined in this file
 */

static int AtObjCmd(AtProc *atProc, Tcl_Interp *interp,
                    int objc, Tcl_Obj *CONST objv[]);



/*
 *----------------------------------------------------------------------
 *
 * Ns_TclNewCallback, Ns_TclNewCallbackObj --
 *
 *      Create a new script callback.
 *
 * Results:
 *      Pointer to Ns_TclCallback.
 *
 * Side effects:
 *      Copies are made of script and scriptarg.
 *
 *----------------------------------------------------------------------
 */

Ns_TclCallback *
Ns_TclNewCallback(Tcl_Interp *interp, void *cbProc,
                  char *script, char *scriptarg)
{
    Ns_TclCallback *cbPtr;

    cbPtr = ns_malloc(sizeof(Ns_TclCallback));
    cbPtr->cbProc    = cbProc;
    cbPtr->server    = Ns_TclInterpServer(interp);
    cbPtr->script    = ns_strdup(script);
    cbPtr->scriptarg = ns_strcopy(scriptarg);

    return cbPtr;
}

Ns_TclCallback *
Ns_TclNewCallbackObj(Tcl_Interp *interp, void *cbProc,
                     Tcl_Obj *objPtr, Tcl_Obj *argObjPtr)
{
    return Ns_TclNewCallback(interp, cbProc, Tcl_GetString(objPtr),
                             (argObjPtr ? Tcl_GetString(argObjPtr) : NULL));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclFreeCallback --
 *
 *      Free a callback created with Ns_TclNewCallback.
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
Ns_TclFreeCallback(void *arg)
{
    Ns_TclCallback *cbPtr = arg;

    ns_free(cbPtr->script);
    ns_free(cbPtr->scriptarg);
    ns_free(cbPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclEvalCallback --
 *
 *      Evaluate a Tcl callback in the given interp.
 *
 * Results:
 *      Tcl return code.  Result of successful script execution will
 *      be appended to dstring if given.
 *
 * Side effects:
 *      An interp may be allocated if none given and none already
 *      cached for current thread.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclEvalCallback(Tcl_Interp *interp, Ns_TclCallback *cbPtr, Ns_DString *result, ...)
{
    va_list      ap;
    Ns_DString   ds;
    char        *arg;
    int          deallocInterp = 0;
    int          status = TCL_ERROR;

    if (interp == NULL) {
        interp = Ns_TclAllocateInterp(cbPtr->server);
        deallocInterp = 1;
    }
    if (interp != NULL) {
        Ns_DStringInit(&ds);
        Ns_DStringAppend(&ds, cbPtr->script);
        va_start(ap, result);
        while ((arg = va_arg(ap, char *)) != NULL) {
            Ns_DStringAppendElement(&ds, arg);
        }
        va_end(ap);
        if (cbPtr->scriptarg) {
            Ns_DStringAppendElement(&ds, cbPtr->scriptarg);
        }
        status = Tcl_EvalEx(interp, ds.string, ds.length, 0);
        if (status != TCL_OK) {
            Ns_DStringTrunc(&ds, 0);
            Ns_DStringAppend(&ds, "\n    while executing callback\n");
            Ns_GetProcInfo(&ds, cbPtr->cbProc, cbPtr);
            Tcl_AddObjErrorInfo(interp, ds.string, ds.length);
            Ns_TclLogError(interp);
        } else if (result != NULL) {
            Ns_DStringAppend(result, Tcl_GetStringResult(interp));
        }
        Ns_DStringFree(&ds);
        if (deallocInterp) {
            Ns_TclDeAllocateInterp(interp);
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclCallbackProc --
 *
 *      Callback routine which evaluates the registered Tcl script.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on Tcl script.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclCallbackProc(void *arg)
{
    Ns_TclCallback *cbPtr = arg;

    (void) Ns_TclEvalCallback(NULL, cbPtr, NULL, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclCallbackArgProc --
 *
 *      Proc info routine to copy Tcl callback script.
 *
 * Results:
 *      None. 
 *
 * Side effects:
 *      Will copy script to given dstring.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclCallbackArgProc(Tcl_DString *dsPtr, void *arg)
{
    Ns_TclCallback *cbPtr = arg;

    Tcl_DStringAppendElement(dsPtr, cbPtr->script);
    if (cbPtr->scriptarg != NULL) {
        Tcl_DStringAppendElement(dsPtr, cbPtr->scriptarg);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AtObjCmd --
 *
 *      Implements ns_atsignal, ns_atshutdown, ns_atstartup, ns_atexit.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      See docs. 
 *
 *----------------------------------------------------------------------
 */

static int
AtObjCmd(AtProc *atProc, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_TclCallback *cbPtr;

    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?arg?");
        return TCL_ERROR;
    }
    cbPtr = Ns_TclNewCallbackObj(interp, Ns_TclCallbackProc,
                                 objv[1], (objc == 3) ? objv[2] : NULL);
    (*atProc)(Ns_TclCallbackProc, cbPtr);

    return TCL_OK;
}
    
int
NsTclAtPreStartupObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return AtObjCmd(Ns_RegisterAtPreStartup, interp, objc, objv);
}

int
NsTclAtStartupObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return AtObjCmd(Ns_RegisterAtStartup, interp, objc, objv);
}

int
NsTclAtSignalObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return AtObjCmd(Ns_RegisterAtSignal, interp, objc, objv);
}

int
NsTclAtShutdownObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return AtObjCmd(Ns_RegisterShutdown, interp, objc, objv);
}

int
NsTclAtExitObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return AtObjCmd(Ns_RegisterAtExit, interp, objc, objv);
}
