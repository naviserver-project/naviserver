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
 * tclcallbacks.c --
 *
 *      Support for executing Tcl code in response to a callback event.
 *
 */

#include "nsd.h"

typedef void *(AtProc)(Ns_Callback *proc, void *data);


/*
 * Local functions defined in this file
 */

static Ns_ShutdownProc ShutdownProc;
static int AtObjCmd(AtProc *atProc, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);



/*
 *----------------------------------------------------------------------
 *
 * Ns_TclNewCallback --
 *
 *      Create a new script callback. The callback uses a single memory chunk,
 *      which can be freed as well by a single ns_free() operation. In order
 *      to get alignment right, we use a minimal array in the ns_callback
 *      guarantees proper alignment of the argument vector after the
 *      Ns_TclCallback structure.
 *
 * Results:
 *      Pointer to Ns_TclCallback.
 *
 * Side effects:
 *      Copies are made of script and arguments
 *
 *----------------------------------------------------------------------
 */
Ns_TclCallback *
Ns_TclNewCallback(Tcl_Interp *interp, ns_funcptr_t cbProc, Tcl_Obj *scriptObjPtr,
                  TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    Ns_TclCallback *cbPtr;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cbProc != NULL);
    NS_NONNULL_ASSERT(scriptObjPtr != NULL);

    cbPtr = ns_malloc(sizeof(Ns_TclCallback) +
                      + (objc > 1 ? (size_t)(objc-1) * sizeof(char *) : 0u) );
    if (unlikely(cbPtr == NULL)) {
        Ns_Fatal("tclcallback: out of memory while creating callback");
    } else {

        cbPtr->cbProc = cbProc;
        cbPtr->server = Ns_TclInterpServer(interp);
        cbPtr->script = ns_strdup(Tcl_GetString(scriptObjPtr));
        cbPtr->argc   = objc;
        cbPtr->argv   = (char **)&cbPtr->args;

        if (objc > 0) {
            TCL_OBJC_T i;

            for (i = 0; i < objc; i++) {
                cbPtr->argv[i] = ns_strdup(Tcl_GetString(objv[i]));
            }
        }
    }
    return cbPtr;
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
    TCL_SIZE_T      ii;
    Ns_TclCallback *cbPtr = arg;

    for (ii = 0; ii < cbPtr->argc; ii++) {
        ns_free(cbPtr->argv[ii]);
    }

    ns_free((void *)cbPtr->script);
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
Ns_TclEvalCallback(Tcl_Interp *interp, const Ns_TclCallback *cbPtr,
                   Tcl_DString *resultDString, ...)
{
    Ns_DString   ds;
    bool         deallocInterp = NS_FALSE;
    int          status = TCL_ERROR;

    NS_NONNULL_ASSERT(cbPtr != NULL);

    if (interp == NULL) {
        interp = Ns_TclAllocateInterp(cbPtr->server);
        deallocInterp = NS_TRUE;
    }
    if (interp != NULL) {
        const char *arg;
        TCL_SIZE_T  ii;
        va_list     ap;

        Ns_DStringInit(&ds);
        Ns_DStringAppend(&ds, cbPtr->script);
        va_start(ap, resultDString);

        for (arg = va_arg(ap, char *); arg != NULL; arg = va_arg(ap, char *)) {
            Ns_DStringAppendElement(&ds, arg);
        }
        va_end(ap);

        for (ii = 0; ii < cbPtr->argc; ii++) {
            Ns_DStringAppendElement(&ds, cbPtr->argv[ii]);
        }
        status = Tcl_EvalEx(interp, ds.string, ds.length, 0);
        if (status != TCL_OK) {
            Ns_DStringSetLength(&ds, 0);
            Ns_DStringAppend(&ds, "\n    while executing callback\n");
            Ns_GetProcInfo(&ds, (ns_funcptr_t)cbPtr->cbProc, cbPtr);
            Tcl_AddObjErrorInfo(interp, ds.string, ds.length);
            if (deallocInterp) {
                (void) Ns_TclLogErrorInfo(interp, NULL);
            }
        } else if (resultDString != NULL) {
            Ns_DStringAppend(resultDString, Tcl_GetStringResult(interp));
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
    const Ns_TclCallback *cbPtr = arg;

    (void) Ns_TclEvalCallback(NULL, cbPtr, (Ns_DString *)NULL, (char *)0L);
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
Ns_TclCallbackArgProc(Tcl_DString *dsPtr, const void *arg)
{
    TCL_SIZE_T            ii;
    const Ns_TclCallback *cbPtr = arg;

    Tcl_DStringAppendElement(dsPtr, cbPtr->script);
    for (ii = 0; ii < cbPtr->argc; ii++) {
        Tcl_DStringAppendElement(dsPtr, cbPtr->argv[ii]);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AtObjCmd --
 *
 *      Implements "ns_atprestartup", "ns_atstartup", "ns_atsignal",
 *      and "ns_atexit".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Script will be run some time in the future when the event occurs.
 *
 *----------------------------------------------------------------------
 */

static int
AtObjCmd(AtProc *atProc, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?args?");
        result = TCL_ERROR;

    } else {
        Ns_TclCallback *cbPtr = Ns_TclNewCallback(interp,
                                                  (ns_funcptr_t)Ns_TclCallbackProc, objv[1],
                                                  (TCL_SIZE_T)(objc - 2), objv + 2);
        (void) (*atProc)(Ns_TclCallbackProc, cbPtr);
    }

    return result;
}

int
NsTclAtPreStartupObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    return AtObjCmd(Ns_RegisterAtPreStartup, interp, objc, objv);
}

int
NsTclAtStartupObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    return AtObjCmd(Ns_RegisterAtStartup, interp, objc, objv);
}

int
NsTclAtSignalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    return AtObjCmd(Ns_RegisterAtSignal, interp, objc, objv);
}

int
NsTclAtExitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    return AtObjCmd(Ns_RegisterAtExit, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAtShutdownObjCmd --
 *
 *      Implements "ns_atshutdown".  The callback timeout parameter is
 *      ignored.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclAtShutdownObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    static bool initialized = NS_FALSE;

    if (!initialized) {
        Ns_RegisterProcInfo((ns_funcptr_t)ShutdownProc, "ns:tclshutdown",
                            Ns_TclCallbackArgProc);
        initialized = NS_TRUE;
    }
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?args?");
        result = TCL_ERROR;

    } else {
        Ns_TclCallback *cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)ShutdownProc,
                                                  objv[1], (TCL_SIZE_T)(objc - 2), objv + 2);
        (void) Ns_RegisterAtShutdown(ShutdownProc, cbPtr);
    }
    return result;
}

static void
ShutdownProc(const Ns_Time *toPtr, void *arg)
{
    if (toPtr == NULL) {
        Ns_TclCallbackProc(arg);
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
