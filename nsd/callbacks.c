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

NS_RCSID("@(#) $Header$");


/*
 * This structure is used as nodes in a linked list of callbacks.
 */

typedef struct Callback {
    struct Callback *nextPtr;
    struct Callback *prevPtr;
    void            *proc;
    void            *arg;
} Callback;

/*
 * Local functions defined in this file
 */

static Ns_ThreadProc ShutdownThread;

static void  RunCallbacks(Callback *firstPtr, int reverse);
static void *RegisterAt(Callback **firstPtrPtr, void *proc, void *arg);

/*
 * Static variables defined in this file
 */

static Callback *firstPreStartup;
static Callback *firstStartup;
static Callback *firstSignal;
static Callback *firstShutdown;
static Callback *firstExit;
static Callback *firstReady;
static Ns_Mutex  lock;
static Ns_Cond   cond;
static int       shutdownPending;
static int       shutdownComplete;
static Ns_Thread shutdownThread;

void *
Ns_RegisterAtReady(Ns_Callback *proc, void *arg)
{
    return RegisterAt(&firstReady, proc, arg);
}

void
NsRunAtReadyProcs(void)
{
    RunCallbacks(firstReady, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtStartup --
 *
 *      Register a callback to run at server startup 
 *
 * Results:
 *      None 
 *
 * Side effects:
 *      The callback will be registered 
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtStartup(Ns_Callback *proc, void *arg)
{
    return RegisterAt(&firstStartup, proc, arg);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtPreStartup --
 *
 *      Register a callback to run at pre-server startup 
 *
 * Results:
 *      None 
 *
 * Side effects:
 *      The callback will be registered 
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtPreStartup(Ns_Callback *proc, void *arg)
{
    return RegisterAt(&firstPreStartup, proc, arg);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterAtSignal --
 *
 *      Register a callback to run when a signal arrives 
 *
 * Results:
 *      None 
 *
 * Side effects:
 *      The callback will be registered
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtSignal(Ns_Callback * proc, void *arg)
{
    return RegisterAt(&firstSignal, proc, arg);
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
 *      The callback will be registered. 
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtShutdown(Ns_ShutdownProc *proc, void *arg)
{
    return RegisterAt(&firstShutdown, proc, arg);
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
 *      The callback will be registered. 
 *
 *----------------------------------------------------------------------
 */

void *
Ns_RegisterAtExit(Ns_Callback * proc, void *arg)
{
    return RegisterAt(&firstExit, proc, arg);
}


/*
 *----------------------------------------------------------------------
 *
 * NsRunStartupProcs --
 *
 *      Run any callbacks registered for server startup. 
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
NsRunStartupProcs(void)
{
    RunCallbacks(firstStartup, 1);
}


/*
 *----------------------------------------------------------------------
 *
 * NsRunPreStartupProcs --
 *
 *      Run any callbacks registered for pre-server startup. 
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
    RunCallbacks(firstPreStartup, 1);
}


/*
 *----------------------------------------------------------------------
 *
 * NsRunSignalProcs --
 *
 *      Run any callbacks registered for when a signal arrives 
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
NsRunSignalProcs(void)
{
    RunCallbacks(firstSignal, 1);
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
NsStartShutdownProcs()
{
    Ns_MutexLock(&lock);
    shutdownPending = 1;
    if (firstShutdown != NULL) {
        Ns_ThreadCreate(ShutdownThread, firstShutdown, 0, &shutdownThread);
    }
    Ns_MutexUnlock(&lock);
}

static void
ShutdownThread(void *arg)
{
    Callback         *cbPtr;
    Ns_ShutdownProc  *proc;

    Ns_ThreadSetName("-shutdown-");

    /*
     * Well behaved callbacks will return quickly, deferring lengthy
     * work to threads which will be waited upon with NsWaitShutdownProcs().
     */

    for (cbPtr = arg; cbPtr != NULL; cbPtr = cbPtr->nextPtr) {
        proc = cbPtr->proc;
        (*proc)(NULL, cbPtr->arg);
    }

    Ns_MutexLock(&lock);
    shutdownComplete = 1;
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
NsWaitShutdownProcs(Ns_Time *toPtr)
{
    Callback         *cbPtr;
    Ns_ShutdownProc  *proc;
    int               status = NS_OK;

    if (shutdownThread == NULL) {
        return; /* No shutdown callbacks. */
    }

    /*
     * Wait for the shutdown thread to finnish running shutdown
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

        /*
         * Wait for each callback to complete.  Well behaved callbacks will
         * return immediately if timeout has expired.
         */

        for (cbPtr = firstShutdown; cbPtr != NULL; cbPtr = cbPtr->nextPtr) {
            proc = cbPtr->proc;
            (*proc)(toPtr, cbPtr->arg);
        }

        Ns_ThreadJoin(&shutdownThread, NULL);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsRunExitProcs --
 *
 *      Run any callbacks registered for server startup, then 
 *      shutdown, then exit. 
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
NsRunAtExitProcs(void)
{
    RunCallbacks(firstExit, 0);
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
RegisterAt(Callback **firstPtrPtr, void *proc, void *arg)
{
    Callback   *cbPtr;
    static int first = 1;

    cbPtr = ns_malloc(sizeof(Callback));
    cbPtr->proc = proc;
    cbPtr->arg = arg;
    Ns_MutexLock(&lock);
    if (first) {
        Ns_MutexSetName(&lock, "ns:callbacks");
        first = 0;
    }
    if (shutdownPending) {
        ns_free(cbPtr);
        cbPtr = NULL;
    } else if (*firstPtrPtr == NULL) {
        *firstPtrPtr = cbPtr;
        cbPtr->nextPtr = NULL;
        cbPtr->prevPtr = NULL;
    } else {
        (*firstPtrPtr)->prevPtr = cbPtr;
        cbPtr->nextPtr = *firstPtrPtr;
        cbPtr->prevPtr = NULL;
        *firstPtrPtr = cbPtr;
    }
    Ns_MutexUnlock(&lock);

    return (void *) cbPtr;
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
RunCallbacks(Callback *cbPtr, int reverse)
{
    Ns_Callback *proc;

    if (reverse) {
        while (cbPtr != NULL && cbPtr->nextPtr != NULL) {
            cbPtr = cbPtr->nextPtr;
        }
    }

    while (cbPtr != NULL) {
        proc = cbPtr->proc;
        (*proc)(cbPtr->arg);
        if (reverse) {
            cbPtr = cbPtr->prevPtr;
        } else {
            cbPtr = cbPtr->nextPtr;
        }
    }
}

static void
AppendList(Tcl_DString *dsPtr, char *list, Callback *firstPtr, int reverse)
{
    Callback *cbPtr = firstPtr;

    if (reverse) {
        while (cbPtr != NULL && cbPtr->nextPtr != NULL) {
            cbPtr = cbPtr->nextPtr;
        }
    }

    while (cbPtr != NULL) {
        Tcl_DStringStartSublist(dsPtr);
        Tcl_DStringAppendElement(dsPtr, list);
        Ns_GetProcInfo(dsPtr, (void *) cbPtr->proc, cbPtr->arg);
        Tcl_DStringEndSublist(dsPtr);
        if (reverse) {
            cbPtr = cbPtr->prevPtr;
        } else {
            cbPtr = cbPtr->nextPtr;
        }
    }
}


void
NsGetCallbacks(Tcl_DString *dsPtr)
{
    Ns_MutexLock(&lock);
    AppendList(dsPtr, "prestartup", firstPreStartup, 1);
    AppendList(dsPtr, "startup", firstStartup, 1);
    AppendList(dsPtr, "signal", firstSignal, 1);
    AppendList(dsPtr, "shutdown", firstShutdown, 0);
    AppendList(dsPtr, "exit", firstExit, 0);
    Ns_MutexUnlock(&lock);
}
