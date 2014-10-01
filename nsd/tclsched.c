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
static int SchedObjCmd(Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[], int cmd);
static int ReturnValidId(Tcl_Interp *interp, int id, Ns_TclCallback *cbPtr);



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
NsTclAfterObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                 Tcl_Obj *CONST objv[])
{
    Ns_TclCallback *cbPtr;
    int             id, seconds;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "seconds script ?args?");
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &seconds) != TCL_OK) {
        return TCL_ERROR;
    }
    cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclSchedProc, 
			      objv[2], objc - 3, objv + 3);
    id = Ns_After(seconds, (Ns_Callback *) NsTclSchedProc, cbPtr,
                  Ns_TclFreeCallback);

    return ReturnValidId(interp, id, cbPtr);
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
SchedObjCmd(Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[], int cmd)
{
    int id, ok;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "id");
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK) {
        return TCL_ERROR;
    }
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
        ok = -1;
    }
    if (cmd != 'u') {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(ok));
    }
    return TCL_OK;
}

int
NsTclCancelObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return SchedObjCmd(interp, objc, objv, 'c');
}

int
NsTclPauseObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return SchedObjCmd(interp, objc, objv, 'p');
}

int
NsTclResumeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return SchedObjCmd(interp, objc, objv, 'r');
}

int
NsTclUnscheduleObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
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
NsTclSchedDailyObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                      Tcl_Obj *CONST objv[])
{
    Ns_TclCallback *cbPtr;
    Tcl_Obj        *scriptObj;
    int             id, hour, minute;
    int             remain = 0, flags = 0, once = 0, thread = 0;

    Ns_ObjvSpec opts[] = {
        {"-once",   Ns_ObjvBool,  &once,   (void *) 1},
        {"-thread", Ns_ObjvBool,  &thread, (void *) 1},
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
        return TCL_ERROR;
    }

    if (once) {
        flags |= NS_SCHED_ONCE;
    }
    if (thread) {
        flags |= NS_SCHED_THREAD;
    }
    if (hour < 0 || hour > 23) {
        Tcl_SetResult(interp, "hour should be >= 0 and <= 23", TCL_STATIC);
        return TCL_ERROR;
    }
    if (minute < 0 || minute > 59) {
        Tcl_SetResult(interp, "minute should be >= 0 and <= 59", TCL_STATIC);
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclSchedProc, 
			      scriptObj, remain, objv + (objc - remain));
    id = Ns_ScheduleDaily(NsTclSchedProc, cbPtr, flags, hour, minute,
                          FreeSched);

    return ReturnValidId(interp, id, cbPtr);
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
NsTclSchedWeeklyObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                       Tcl_Obj *CONST objv[])
{
    Ns_TclCallback *cbPtr;
    Tcl_Obj        *scriptObj;
    int             id, day, hour, minute;
    int             remain = 0, flags = 0, once = 0, thread = 0;

    Ns_ObjvSpec opts[] = {
        {"-once",   Ns_ObjvBool,  &once,   (void *) 1},
        {"-thread", Ns_ObjvBool,  &thread, (void *) 1},
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
        return TCL_ERROR;
    }

    if (once) {
        flags |= NS_SCHED_ONCE;
    }
    if (thread) {
        flags |= NS_SCHED_THREAD;
    }
    if (day < 0 || day > 6) {
        Tcl_SetResult(interp, "day should be >= 0 and <= 6", TCL_STATIC);
        return TCL_ERROR;
    }
    if (hour < 0 || hour > 23) {
        Tcl_SetResult(interp, "hour should be >= 0 and <= 23", TCL_STATIC);
        return TCL_ERROR;
    }
    if (minute < 0 || minute > 59) {
        Tcl_SetResult(interp, "minute should be >= 0 and <= 59", TCL_STATIC);
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclSchedProc, 
			      scriptObj, remain, objv + (objc - remain));
    id = Ns_ScheduleWeekly(NsTclSchedProc, cbPtr, flags, day, hour, minute,
                           FreeSched);

    return ReturnValidId(interp, id, cbPtr);
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
NsTclSchedObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                 Tcl_Obj *CONST objv[])
{
    Ns_TclCallback *cbPtr;
    Tcl_Obj        *scriptObj;
    int             id, interval;
    int             remain = 0, flags = 0, once = 0, thread = 0;

    Ns_ObjvSpec opts[] = {
        {"-once",    Ns_ObjvBool,  &once,   (void *) 1},
        {"-thread",  Ns_ObjvBool,  &thread, (void *) 1},
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
        return TCL_ERROR;
    }

    if (once) {
        flags |= NS_SCHED_ONCE;
    }
    if (thread) {
        flags |= NS_SCHED_THREAD;
    }
    if (interval < 0) {
        Tcl_SetResult(interp, "interval should be >= 0", TCL_STATIC);
        return TCL_ERROR;
    }

    cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *) NsTclSchedProc, 
			      scriptObj, remain, objv + (objc - remain));
    id = Ns_ScheduleProcEx(NsTclSchedProc, cbPtr, flags, interval, FreeSched);

    return ReturnValidId(interp, id, cbPtr);
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
NsTclSchedProc(void *arg, int id)
{
    Ns_TclCallback *cbPtr = arg;

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
    if (id == NS_ERROR) {
        Tcl_SetResult(interp, "could not schedule procedure", TCL_STATIC);
        Ns_TclFreeCallback(cbPtr);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(id));

    return TCL_OK;
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
FreeSched(void *arg, int id)
{
    Ns_TclFreeCallback(arg);
}
