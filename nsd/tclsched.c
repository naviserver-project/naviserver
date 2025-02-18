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
 * tclsched.c --
 *
 *      Implement scheduled procs in Tcl.
 */

#include "nsd.h"

/*
 * Static variables defined in this file.
 */

static Ns_ObjvValueRange dayRange    = {0, 6};
static Ns_ObjvValueRange hourRange   = {0, 23};
static Ns_ObjvValueRange minuteRange = {0, 59};
static const Ns_ObjvTimeRange posTimeRange = {{0, 1}, {LONG_MAX, 0}};
static const Ns_ObjvTimeRange nonnegTimeRange = {{0, 0}, {LONG_MAX, 0}};

/*
 * Local functions defined in this file.
 */

static Ns_SchedProc FreeSchedCallback;
static int SchedObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, char cmd);
static int ReturnValidId(Tcl_Interp *interp, int id, Ns_TclCallback *cbPtr)
    NS_GNUC_NONNULL(1)  NS_GNUC_NONNULL(3);





/*
 *----------------------------------------------------------------------
 *
 * NsTclAfterObjCmd --
 *
 *      Implements "ns_after".
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
NsTclAfterObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj          *scriptObj;
    Ns_Time          *interval;
    TCL_SIZE_T        remain = 0;
    int               result = TCL_OK;
    Ns_ObjvSpec       args[] = {
        {"interval", Ns_ObjvTime, &interval,  (void*)&nonnegTimeRange},
        {"script",   Ns_ObjvObj,  &scriptObj, NULL},
        {"?arg",     Ns_ObjvArgs, &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        int             id;
        Ns_TclCallback *cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclSchedProc,
                                                  scriptObj, (TCL_SIZE_T)(objc - 3), objv + 3);
        id = Ns_After(interval, NsTclSchedProc, cbPtr, (ns_funcptr_t)Ns_TclFreeCallback);
        result = ReturnValidId(interp, id, cbPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SchedObjCmd --
 *
 *      Implements "ns_unschedule_proc", "ns_cancel", "ns_pause", and
 *      "ns_resume".
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
SchedObjCmd(Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, char cmd)
{
    int id, result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/id/");
        result = TCL_ERROR;

    } else if (Tcl_GetIntFromObj(interp, objv[1], &id) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        bool ok;

        switch (cmd) {
#ifdef NS_WITH_DEPRECATED
        case 'c':
            Ns_LogDeprecated(objv, 1, "ns_unschedule_proc ...", NULL);
            ok = Ns_Cancel(id);
            break;
#endif
        case 'u':
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

        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(ok));
        }
    }
    return result;
}

#ifdef NS_WITH_DEPRECATED
int
NsTclCancelObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return SchedObjCmd(interp, objc, objv, 'c');
}
#endif

int
NsTclPauseObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return SchedObjCmd(interp, objc, objv, 'p');
}

int
NsTclResumeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return SchedObjCmd(interp, objc, objv, 'r');
}

int
NsTclUnscheduleObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return SchedObjCmd(interp, objc, objv, 'u');
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSchedDailyObjCmd --
 *
 *      Implements "ns_schedule_daily".
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
NsTclSchedDailyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *scriptObj;
    int         hour = 0, minute = 0, once = 0, thread = 0, result;
    TCL_SIZE_T  remain = 0;
    Ns_ObjvSpec opts[] = {
        {"-once",   Ns_ObjvBool,  &once,   INT2PTR(NS_TRUE)},
        {"-thread", Ns_ObjvBool,  &thread, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"hour",    Ns_ObjvInt,   &hour,      &hourRange},
        {"minute",  Ns_ObjvInt,   &minute,    &minuteRange},
        {"script",  Ns_ObjvObj,   &scriptObj, NULL},
        {"?arg",    Ns_ObjvArgs,  &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int    flags = 0u;
        int             id;
        Ns_TclCallback *cbPtr;

        if (once != 0) {
            flags |= NS_SCHED_ONCE;
        }
        if (thread != 0) {
            flags |= NS_SCHED_THREAD;
        }
        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclSchedProc,
                                  scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
        id = Ns_ScheduleDaily(NsTclSchedProc, cbPtr, flags, hour, minute,
                              FreeSchedCallback);

        result = ReturnValidId(interp, id, cbPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSchedWeeklyObjCmd --
 *
 *      Implements "ns_sched_weekly".
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
NsTclSchedWeeklyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *scriptObj;
    int         day = 0, hour = 0, minute = 0, once = 0, thread = 0, result;
    TCL_SIZE_T  remain = 0;
    Ns_ObjvSpec opts[] = {
        {"-once",   Ns_ObjvBool,  &once,   INT2PTR(NS_TRUE)},
        {"-thread", Ns_ObjvBool,  &thread, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"day",     Ns_ObjvInt,    &day,       &dayRange},
        {"hour",    Ns_ObjvInt,    &hour,      &hourRange},
        {"minute",  Ns_ObjvInt,    &minute,    &minuteRange},
        {"script",  Ns_ObjvObj,    &scriptObj, NULL},
        {"?arg",    Ns_ObjvArgs,   &remain,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int    flags = 0u;
        Ns_TclCallback *cbPtr;
        int             id;

        if (once != 0) {
            flags |= NS_SCHED_ONCE;
        }
        if (thread != 0) {
            flags |= NS_SCHED_THREAD;
        }

        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclSchedProc,
                                  scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
        id = Ns_ScheduleWeekly(NsTclSchedProc, cbPtr, flags, day, hour, minute,
                               FreeSchedCallback);

        result = ReturnValidId(interp, id, cbPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSchedObjCmd --
 *
 *      Implements "ns_schedule_proc".
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
NsTclSchedObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *scriptObj;
    Ns_Time    *intervalPtr;
    TCL_SIZE_T  remain = 0;
    int         once = 0, thread = 0, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-once",    Ns_ObjvBool,  &once,   INT2PTR(NS_TRUE)},
        {"-thread",  Ns_ObjvBool,  &thread, INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"interval", Ns_ObjvTime,   &intervalPtr, (void*)&nonnegTimeRange},
        {"script",   Ns_ObjvObj,    &scriptObj,   NULL},
        {"?arg",     Ns_ObjvArgs,   &remain,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int flags = 0u;

        if (thread != 0) {
            flags |= NS_SCHED_THREAD;
        }
        if (once != 0) {
            flags |= NS_SCHED_ONCE;
        } else {
            result = Ns_CheckTimeRange(interp, "interval", &posTimeRange, intervalPtr);
        }

        if (result == TCL_OK) {
            Ns_TclCallback *cbPtr;
            int             id;

            cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclSchedProc,
                                      scriptObj, remain, objv + ((TCL_SIZE_T)objc - remain));
            id = Ns_ScheduleProcEx(NsTclSchedProc, cbPtr, flags, intervalPtr, FreeSchedCallback);

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

    (void) Ns_TclEvalCallback(NULL, cbPtr, NULL, NS_SENTINEL);
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
 * FreeSchedCallback --
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
FreeSchedCallback(void *arg, int UNUSED(id))
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
