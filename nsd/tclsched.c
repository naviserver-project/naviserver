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
 * tclsched.c --
 *
 *      Implement scheduled procs in Tcl.
 */

#include "nsd.h"

/*
 * Local functions defined in this file
 */

static Ns_SchedProc FreeSched;
static int SchedObjCmd(Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, char cmd);
static int ReturnValidId(Tcl_Interp *interp, int id, Ns_TclCallback *cbPtr)
    NS_GNUC_NONNULL(1)  NS_GNUC_NONNULL(3);





/*
 *----------------------------------------------------------------------
 *
 * NsTclAfterObjCmd --
 *
 *      Implements ns_after.
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
NsTclAfterObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result, seconds;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "seconds script ?args?");
        result = TCL_ERROR;

    } else if (Tcl_GetIntFromObj(interp, objv[1], &seconds) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        int             id;
        Ns_TclCallback *cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *)NsTclSchedProc, 
                                                  objv[2], objc - 3, objv + 3);
        id = Ns_After(seconds, (Ns_Callback *)NsTclSchedProc, cbPtr,
                      Ns_TclFreeCallback);
        result = ReturnValidId(interp, id, cbPtr);
    }
    
    return result;
}
   

/*
 *----------------------------------------------------------------------
 *
 * SchedObjCmd --
 *
 *      Implements ns_unschedule_proc, ns_cancel, ns_pause, and
 *      ns_resume commands.
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
SchedObjCmd(Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, char cmd)
{
    int id, result = TCL_OK;
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "id");
        result = TCL_ERROR;

    } else if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        bool ok;
        
        switch (cmd) {
        case 'u':
        case 'c':
            ok = Ns_Cancel(id);
            break;
        case 'p':
            ok = Ns_Pause(id);
            break;
        case 'r':
            ok = Ns_Resume(id);
            break;
        default:
            result = TCL_ERROR;
            ok = NS_FALSE;
            Ns_Log(Error, "unexpected code '%c' passed to SchedObjCmd", cmd);
            break;
        }
    
        if ((result == TCL_OK) && (cmd != 'u')) {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(ok));
        }
    }
    return result;
}

int
NsTclCancelObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return SchedObjCmd(interp, objc, objv, 'c');
}

int
NsTclPauseObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return SchedObjCmd(interp, objc, objv, 'p');
}

int
NsTclResumeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return SchedObjCmd(interp, objc, objv, 'r');
}

int
NsTclUnscheduleObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return SchedObjCmd(interp, objc, objv, 'u');
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSchedDailyObjCmd --
 *
 *      Implements ns_schedule_daily. 
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
NsTclSchedDailyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Tcl_Obj        *scriptObj;
    int             hour, minute, result = TCL_OK;
    int             remain = 0, once = 0, thread = 0;

    Ns_ObjvSpec opts[] = {
        {"-once",   Ns_ObjvBool,  &once,   INT2PTR(NS_TRUE)},
        {"-thread", Ns_ObjvBool,  &thread, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"hour",    Ns_ObjvInt,   &hour,      NULL},
        {"minute",  Ns_ObjvInt,   &minute,    NULL},
        {"script",  Ns_ObjvObj,   &scriptObj, NULL},
        {"?args",   Ns_ObjvArgs,  &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        unsigned int flags = 0u;

        if (once != 0) {
            flags |= NS_SCHED_ONCE;
        }
        if (thread != 0) {
            flags |= NS_SCHED_THREAD;
        }
        if (hour < 0 || hour > 23) {
            Ns_TclPrintfResult(interp, "hour should be >= 0 and <= 23");
            result = TCL_ERROR;
        } else if (minute < 0 || minute > 59) {
            Ns_TclPrintfResult(interp, "minute should be >= 0 and <= 59");
            result = TCL_ERROR;
        } else {
            int             id;
            Ns_TclCallback *cbPtr;

            cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclSchedProc, 
                                      scriptObj, remain, objv + (objc - remain));
            id = Ns_ScheduleDaily(NsTclSchedProc, cbPtr, flags, hour, minute,
                                  FreeSched);

            result = ReturnValidId(interp, id, cbPtr);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSchedWeeklyObjCmd --
 *
 *      Implements ns_sched_weekly.
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
NsTclSchedWeeklyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Tcl_Obj        *scriptObj;
    int             day, hour, minute, result = TCL_OK;
    int             remain = 0, once = 0, thread = 0;

    Ns_ObjvSpec opts[] = {
	{"-once",   Ns_ObjvBool,  &once,   INT2PTR(NS_TRUE)},
        {"-thread", Ns_ObjvBool,  &thread, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"day",     Ns_ObjvInt,    &day,       NULL},
        {"hour",    Ns_ObjvInt,    &hour,      NULL},
        {"minute",  Ns_ObjvInt,    &minute,    NULL},
        {"script",  Ns_ObjvObj,    &scriptObj, NULL},
        {"?args",   Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int    flags = 0u;

        if (once != 0) {
            flags |= NS_SCHED_ONCE;
        }
        if (thread != 0) {
            flags |= NS_SCHED_THREAD;
        }
        if (day < 0 || day > 6) {
            Ns_TclPrintfResult(interp, "day should be >= 0 and <= 6");
            result = TCL_ERROR;
        } else if (hour < 0 || hour > 23) {
            Ns_TclPrintfResult(interp, "hour should be >= 0 and <= 23");
            result = TCL_ERROR;
        } else if (minute < 0 || minute > 59) {
            Ns_TclPrintfResult(interp, "minute should be >= 0 and <= 59");
            result = TCL_ERROR;
        } else {
            Ns_TclCallback *cbPtr;
            int             id;

            cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclSchedProc, 
                                      scriptObj, remain, objv + (objc - remain));
            id = Ns_ScheduleWeekly(NsTclSchedProc, cbPtr, flags, day, hour, minute,
                                   FreeSched);

            result = ReturnValidId(interp, id, cbPtr);
        }
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSchedObjCmd --
 *
 *      Implements ns_schedule_proc. 
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
NsTclSchedObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Tcl_Obj        *scriptObj;
    int             interval, remain = 0, once = 0, thread = 0, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-once",    Ns_ObjvBool,  &once,   INT2PTR(NS_TRUE)},
        {"-thread",  Ns_ObjvBool,  &thread, INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"interval", Ns_ObjvInt,    &interval,  NULL},
        {"script",   Ns_ObjvObj,    &scriptObj, NULL},
        {"?args",    Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int flags = 0u;

        if (once != 0) {
            flags |= NS_SCHED_ONCE;
        }
        if (thread != 0) {
            flags |= NS_SCHED_THREAD;
        }
        if (interval < 0) {
            Ns_TclPrintfResult(interp, "interval should be >= 0");
            result = TCL_ERROR;
            
        } else {
            Ns_TclCallback *cbPtr;
            int             id;

            cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclSchedProc, 
                                      scriptObj, remain, objv + (objc - remain));
            id = Ns_ScheduleProcEx(NsTclSchedProc, cbPtr, flags, interval, FreeSched);
            
            result = ReturnValidId(interp, id, cbPtr);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSchedProc --
 *
 *      Callback for a Tcl scheduled proc.
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
NsTclSchedProc(void *arg, int UNUSED(id))
{
    const Ns_TclCallback *cbPtr = arg;

    (void) Ns_TclEvalCallback(NULL, cbPtr, NULL, (char *)0);
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnValidId --
 *
 *      Update the interp result with the given schedule id if valid.
 *      Otherwise, free the callback and leave an error in the interp.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ReturnValidId(Tcl_Interp *interp, int id, Ns_TclCallback *cbPtr)
{
    int result = TCL_OK;
    
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cbPtr != NULL);

    if (id == (int)NS_ERROR) {
        Ns_TclPrintfResult(interp, "could not schedule procedure");
        Ns_TclFreeCallback(cbPtr);
        result = TCL_ERROR;
        
    } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeSched --
 *
 *      Free a callback used for scheduled commands.
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
FreeSched(void *arg, int UNUSED(id))
{
    Ns_TclFreeCallback(arg);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
