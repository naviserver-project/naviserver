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
 * callbacks.c --
 *
 *      Support for Callbacks
 *
 *      These functions allow the registration of callbacks
 *      that are run at various points during the server's execution.
 */

#include "nsd.h"

/*
 * This structure is used as nodes in a linked list of callbacks.
 */

typedef struct Callback {
    struct Callback *nextPtr;
    Ns_Callback     *proc;
    void            *arg;
} Callback;

/*
 * Local functions defined in this file
 */

static Ns_ThreadProc ShutdownThread;

static void *RegisterAt(Callback **firstPtrPtr, Ns_Callback *proc, void *arg, bool fifo)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void RunCallbacks(const char *list, const Callback *cbPtr)
    NS_GNUC_NONNULL(1);

static void AppendList(Tcl_DString *dsPtr, const char *list, const Callback *cbPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * Static variables defined in this file
 */

static Callback *firstPreStartup = NULL;
static Callback *firstStartup = NULL;
static Callback *firstSignal = NULL;
static Callback *firstShutdown = NULL;
static Callback *firstExit = NULL;
static Callback *firstReady = NULL;

static Ns_Mutex  lock;
static Ns_Cond   cond;

static bool      shutdownPending  = NS_FALSE;
static bool      shutdownComplete = NS_FALSE;
static Ns_Thread shutdownThread   = NULL;



/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtPreStartup --
 *
 *      Register a callback to run at the pre-startup stage, at which
 *      point the config file has been parsed and modules loaded.
 *      Callbacks will run in FIFO order.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtPreStartup(Ns_Callback *proc, void *arg)
{
    NS_NONNULL_ASSERT(proc != NULL);
    return RegisterAt(&firstPreStartup, proc, arg, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtStartup --
 *
 *      Register a callback to run at server startup, just after the
 *      driver thread starts listening for connections.
 *      Callbacks will run in FIFO order.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtStartup(Ns_Callback *proc, void *arg)
{
    NS_NONNULL_ASSERT(proc != NULL);
    return RegisterAt(&firstStartup, proc, arg, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtSignal --
 *
 *      Register a callback to run when the server receives a SIGHUP.
 *      Callbacks will run in FIFO order.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtSignal(Ns_Callback *proc, void *arg)
{
    NS_NONNULL_ASSERT(proc != NULL);
    return RegisterAt(&firstSignal, proc, arg, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtready --
 *
 *      Register a callback to run when the driver thread becomes ready?
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtReady(Ns_Callback *proc, void *arg)
{
    NS_NONNULL_ASSERT(proc != NULL);
    return RegisterAt(&firstReady, proc, arg, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtShutdown --
 *
 *      Register a callback to run at server shutdown.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtShutdown(Ns_ShutdownProc *proc, void *arg)
{
    NS_NONNULL_ASSERT(proc != NULL);
    return RegisterAt(&firstShutdown, (Ns_Callback *)proc, arg, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtExit --
 *
 *      Register a callback to be run at server exit.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtExit(Ns_Callback *proc, void *arg)
{
    NS_NONNULL_ASSERT(proc != NULL);
    return RegisterAt(&firstExit, proc, arg, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * NsRunPreStartupProcs, NsRunStartupProcs,
 * NsRunSignalProcs, NsRunAtReadyProcs, NsRunAtExitProcs --
 *
 *      Run all callbacks in the corresponding queue.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Callbacks called back.
 *
 *----------------------------------------------------------------------
 */

void
NsRunPreStartupProcs(void)
{
    RunCallbacks("prestartup", firstPreStartup);
}

void
NsRunStartupProcs(void)
{
    RunCallbacks("startup", firstStartup);
}

void
NsRunSignalProcs(void)
{
    RunCallbacks("signal", firstSignal);
}

void
NsRunAtReadyProcs(void)
{
    RunCallbacks("ready", firstReady);
}

void
NsRunAtExitProcs(void)
{
    RunCallbacks("exit", firstExit);
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartShutdownProcs --
 *
 *      Run all shutdown procs sequentially in a detached thread. This
 *      proc returns immediately.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on registered shutdown procs.
 *
 *----------------------------------------------------------------------
 */

void
NsStartShutdownProcs(void)
{
    Ns_MutexLock(&lock);
    shutdownPending = NS_TRUE;
    if (firstShutdown != NULL) {
        Ns_ThreadCreate(ShutdownThread, firstShutdown, 0, &shutdownThread);
    }
    Ns_MutexUnlock(&lock);
}

static void
ShutdownThread(void *arg)
{
    const Callback *cbPtr;

    Ns_ThreadSetName("-shutdown-");

    /*
     * Well behaved callbacks will return quickly, deferring lengthy
     * work to threads which will be waited upon with NsWaitShutdownProcs().
     */

    for (cbPtr = arg; cbPtr != NULL; cbPtr = cbPtr->nextPtr) {
	Ns_ShutdownProc  *proc = (Ns_ShutdownProc *)cbPtr->proc;

        if (Ns_LogSeverityEnabled(Debug)) {
            Ns_DString ds;

            Ns_DStringInit(&ds);
            Ns_GetProcInfo(&ds, cbPtr->proc, cbPtr->arg);
            Ns_Log(Debug, "ns:callback:shutdown: %s", Ns_DStringValue(&ds));
            Ns_DStringFree(&ds);
        }

        (*proc)(NULL, cbPtr->arg);
    }

    Ns_MutexLock(&lock);
    shutdownComplete = NS_TRUE;
    Ns_CondSignal(&cond);
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsWaitShutdownProcs --
 *
 *      Wait for detached shutdown thread to complete, then wait for
 *      shutdown callbacks individually.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Give up waiting if timeout expires.
 *
 *----------------------------------------------------------------------
 */

void
NsWaitShutdownProcs(const Ns_Time *toPtr)
{
    /*
     * Execute shutdown callbacks only when we have a shutdownThread.
     */
    if (shutdownThread != NULL) {
        Ns_ReturnCode     status = NS_OK;

        /*
         * Wait for the shutdown thread to finish running shutdown
         * notification and one-shot callbacks.
         */
        Ns_MutexLock(&lock);
        while (status == NS_OK && !shutdownComplete) {
            status = Ns_CondTimedWait(&cond, &lock, toPtr);
        }
        Ns_MutexUnlock(&lock);

        if (status != NS_OK) {
            Ns_Log(Warning, "shutdown: timeout waiting for shutdown procs");
        } else {
            const Callback   *cbPtr;

            /*
             * Wait for each callback to complete.  Well behaved callbacks will
             * return immediately if timeout has expired.
             */

            for (cbPtr = firstShutdown; cbPtr != NULL; cbPtr = cbPtr->nextPtr) {
                Ns_ShutdownProc *proc = (Ns_ShutdownProc *)cbPtr->proc;
                (*proc)(toPtr, cbPtr->arg);
            }

            Ns_ThreadJoin(&shutdownThread, NULL);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetCallbacks --
 *
 *      Append callback info to given dstring. Called by ns_info.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsGetCallbacks(Tcl_DString *dsPtr)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MutexLock(&lock);
    AppendList(dsPtr, "prestartup", firstPreStartup);
    AppendList(dsPtr, "startup", firstStartup);
    AppendList(dsPtr, "signal", firstSignal);
    AppendList(dsPtr, "shutdown", firstShutdown);
    AppendList(dsPtr, "exit", firstExit);
    Ns_MutexUnlock(&lock);
}

static void
AppendList(Tcl_DString *dsPtr, const char *list, const Callback *cbPtr)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(list != NULL);

    while (cbPtr != NULL) {
        Tcl_DStringStartSublist(dsPtr);
        Tcl_DStringAppendElement(dsPtr, list);
        Ns_GetProcInfo(dsPtr, cbPtr->proc, cbPtr->arg);
        Tcl_DStringEndSublist(dsPtr);

        cbPtr = cbPtr->nextPtr;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * RegisterAt --
 *
 *      Generic function that registers callbacks for any event
 *
 * Results:
 *      Pointer to the newly-allocated Callback structure
 *
 * Side effects:
 *      Callback struct will be alloacated and put in the linked list.
 *
 *----------------------------------------------------------------------
 */

static void *
RegisterAt(Callback **firstPtrPtr, Ns_Callback *proc, void *arg, bool fifo)
{
    Callback   *cbPtr, *nextPtr;
    static bool first = NS_TRUE;

    NS_NONNULL_ASSERT(firstPtrPtr != NULL);
    NS_NONNULL_ASSERT(proc != NULL);

    cbPtr = ns_malloc(sizeof(Callback));
    cbPtr->proc = proc;
    cbPtr->arg = arg;

    Ns_MutexLock(&lock);
    if (first) {
        first = NS_FALSE;
        Ns_MutexSetName(&lock, "ns:callbacks");
    }
    if (shutdownPending) {
        ns_free(cbPtr);
        cbPtr = NULL;
    } else if (*firstPtrPtr == NULL) {
        *firstPtrPtr = cbPtr;
        cbPtr->nextPtr = NULL;
    } else if (fifo) {
        nextPtr = *firstPtrPtr;
        while (nextPtr->nextPtr != NULL) {
            nextPtr = nextPtr->nextPtr;
        }
        nextPtr->nextPtr = cbPtr;
        cbPtr->nextPtr = NULL;
    } else {
        cbPtr->nextPtr = *firstPtrPtr;
        *firstPtrPtr = cbPtr;
    }
    Ns_MutexUnlock(&lock);

    return cbPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * RunCallbacks --
 *
 *      Run all callbacks in the passed-in linked list.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Callbacks called back.
 *
 *----------------------------------------------------------------------
 */

static void
RunCallbacks(const char *list, const Callback *cbPtr)
{
    NS_NONNULL_ASSERT(list != NULL);

    while (cbPtr != NULL) {
	Ns_Callback *proc;
	Ns_DString   ds;

        if (Ns_LogSeverityEnabled(Debug)) {
            Ns_DStringInit(&ds);
            Ns_GetProcInfo(&ds, cbPtr->proc, cbPtr->arg);
            Ns_Log(Debug, "ns:callback: %s: %s", list, Ns_DStringValue(&ds));
            Ns_DStringFree(&ds);
        }
        proc = cbPtr->proc;
       (*proc)(cbPtr->arg);

        cbPtr = cbPtr->nextPtr;
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

