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
 * tclthread.c --
 *
 *      Tcl wrappers around all thread objects
 */

#include "nsd.h"

/*
 * The following structure defines the script to run
 * in a Tcl thread.
 */

typedef struct TclThreadArg {
    const char *server;
    const char *threadName;
    bool        detached;
    char        script[1];
} TclThreadArg;

static Ns_Tls argtls = NULL;

/*
 * Local functions defined in this file
 */

static Ns_ReturnCode CreateTclThread(const NsInterp *itPtr, const char *script, bool detached,
                                     const char *threadName, Ns_Thread *thrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static void *CreateSynchObject(const NsInterp *itPtr,
                               Tcl_HashTable *typeTable, unsigned int *idPtr,
                               Ns_Callback *initProc, const char *type,
                               Tcl_Obj *objPtr, TCL_SIZE_T cnt)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

static void ThreadArgFree(void *arg)
    NS_GNUC_NONNULL(1);

static Ns_ObjvProc ObjvMutexObj;
static Ns_ObjvProc ObjvSemaObj;
static Ns_ObjvProc ObjvRWLockObj;
static Ns_ObjvProc ObjvCondObj;
static Ns_ObjvProc ObjvCsObj;
static Ns_ObjvProc ObjvThreadObj;

static TCL_OBJCMDPROC_T MutexCreateObjCmd;
static TCL_OBJCMDPROC_T MutexDestroyObjCmd;
static TCL_OBJCMDPROC_T MutexEvalObjCmd;
static TCL_OBJCMDPROC_T MutexLockObjCmd;
static TCL_OBJCMDPROC_T MutexTrylockObjCmd;
static TCL_OBJCMDPROC_T MutexUnlockObjCmd;

static TCL_OBJCMDPROC_T SemaCreateObjCmd;
static TCL_OBJCMDPROC_T SemaDestroyObjCmd;
static TCL_OBJCMDPROC_T SemaReleaseObjCmd;
static TCL_OBJCMDPROC_T SemaWaitObjCmd;

static TCL_OBJCMDPROC_T RWLockCreateObjCmd;
static TCL_OBJCMDPROC_T RWLockDestroyObjCmd;
static TCL_OBJCMDPROC_T RWLockReadevalObjCmd;
static TCL_OBJCMDPROC_T RWLockReadlockObjCmd;
static TCL_OBJCMDPROC_T RWLockUnlockObjCmd;
static TCL_OBJCMDPROC_T RWLockWriteevalObjCmd;
static TCL_OBJCMDPROC_T RWLockWritelockObjCmd;

static TCL_OBJCMDPROC_T CondAbswaitObjCmd;
static TCL_OBJCMDPROC_T CondBroadcastObjCmd;
static TCL_OBJCMDPROC_T CondCreateObjCmd;
static TCL_OBJCMDPROC_T CondDestroyObjCmd;
static TCL_OBJCMDPROC_T CondSignalObjCmd;
static TCL_OBJCMDPROC_T CondWaitObjCmd;


/*
 * Local variables defined in this file.
 */

static const char *const mutexType  = "ns:mutex";
static const char *const csType     = "ns:critsec";
static const char *const semaType   = "ns:semaphore";
static const char *const condType   = "ns:condition";
static const char *const rwType     = "ns:rwlock";
static const char *const threadType = "ns:thread";


/*
 *----------------------------------------------------------------------
 *
 * ObjvSemaObj --
 *
 *      objv converter for Ns_Sema*.
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
ObjvSemaObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    int result = TCL_ERROR;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const NsInterp *itPtr = NsGetInterpData(interp);
        NsServer       *servPtr = itPtr->servPtr;
        Ns_Sema       **dest = spec->dest;

        *dest = CreateSynchObject(itPtr,
                                  &servPtr->tcl.synch.semaTable,
                                  &servPtr->tcl.synch.semaId,
                                  NULL,
                                  semaType,
                                  objv[0],
                                  TCL_INDEX_NONE);
        //fprintf(stderr, "ns_sema: result of conversion %p\n",(void*)*dest);
        if (*dest == NULL) {
            Ns_TclPrintfResult(interp, "ns_sema: could not convert '%s' to semaphore object", Tcl_GetString(objv[0]));
        } else {
            *objcPtr -= 1;
            result = TCL_OK;
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjvMutexObj --
 *
 *      objv converter for Ns_Mutex*.
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
ObjvMutexObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    int result = TCL_ERROR;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const NsInterp *itPtr = NsGetInterpData(interp);
        NsServer       *servPtr = itPtr->servPtr;
        Ns_Mutex      **dest = spec->dest;

        /*
         * When spec->arg is set this means, that the syncobj mut
         * pre-exist and is not created on the fly.
         */
        *dest = CreateSynchObject(itPtr,
                                  &servPtr->tcl.synch.mutexTable,
                                  &servPtr->tcl.synch.mutexId,
                                  PTR2INT(spec->arg) == NS_TRUE ? NULL : (Ns_Callback *) Ns_MutexInit,
                                  mutexType,
                                  objv[0],
                                  TCL_INDEX_NONE);
        if (*dest == NULL) {
            Ns_TclPrintfResult(interp, "ns_mutex: could not convert '%s' to mutex object", Tcl_GetString(objv[0]));
        } else {
            *objcPtr -= 1;
            result = TCL_OK;
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjvCondObj --
 *
 *      objv converter for Ns_Cond*.
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
ObjvCondObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    int result = TCL_ERROR;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const NsInterp *itPtr = NsGetInterpData(interp);
        NsServer       *servPtr = itPtr->servPtr;
        Ns_Cond       **dest = spec->dest;

        /*
         * When spec->arg is set this means, that the syncobj mut
         * pre-exist and is not created on the fly.
         */
        *dest = CreateSynchObject(itPtr,
                                  &servPtr->tcl.synch.condTable,
                                  &servPtr->tcl.synch.condId,
                                  PTR2INT(spec->arg) == NS_TRUE ? NULL : (Ns_Callback *) Ns_CondInit,
                                  condType,
                                  objv[0],
                                  TCL_INDEX_NONE);
        if (*dest == NULL) {
            Ns_TclPrintfResult(interp, "ns_cond: could not convert '%s' to condition object", Tcl_GetString(objv[0]));
        } else {
            *objcPtr -= 1;
            result = TCL_OK;
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjvRWLockObj --
 *
 *      objv converter for Ns_RWLock*.
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
ObjvRWLockObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    int result = TCL_ERROR;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const NsInterp *itPtr = NsGetInterpData(interp);
        NsServer       *servPtr = itPtr->servPtr;
        Ns_RWLock     **dest = spec->dest;

        /*
         * When spec->arg is set this means, that the syncobj mut
         * pre-exist and is not created on the fly.
         */
        *dest = CreateSynchObject(itPtr,
                                  &servPtr->tcl.synch.rwTable,
                                  &servPtr->tcl.synch.rwId,
                                  PTR2INT(spec->arg) == NS_TRUE ? NULL : (Ns_Callback *) Ns_RWLockInit,
                                  rwType,
                                  objv[0],
                                  TCL_INDEX_NONE);
        if (*dest == NULL) {
            Ns_TclPrintfResult(interp, "ns_rwlock: could not convert '%s' to RWLock object", Tcl_GetString(objv[0]));
        } else {
            *objcPtr -= 1;
            result = TCL_OK;
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjvCsObj --
 *
 *      objv converter for Ns_Cs*.
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
ObjvCsObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    int result = TCL_ERROR;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const NsInterp *itPtr = NsGetInterpData(interp);
        NsServer       *servPtr = itPtr->servPtr;
        Ns_Cs         **dest = spec->dest;

        /*
         * When spec->arg is set this means, that the syncobj mut
         * pre-exist and is not created on the fly.
         */
        *dest = CreateSynchObject(itPtr,
                                  &servPtr->tcl.synch.csTable,
                                  &servPtr->tcl.synch.csId,
                                  PTR2INT(spec->arg) == NS_TRUE ? NULL : (Ns_Callback *) Ns_CsInit,
                                  csType,
                                  objv[0],
                                  TCL_INDEX_NONE);
        if (*dest == NULL) {
            Ns_TclPrintfResult(interp, "ns_critsec: could not convert '%s' to critsec object", Tcl_GetString(objv[0]));
        } else {
            *objcPtr -= 1;
            result = TCL_OK;
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjvThreadObj --
 *
 *      objv converter for Ns_Thread*.
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
ObjvThreadObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    int result = TCL_ERROR;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        void          **dest = spec->dest;

        if (Ns_TclGetAddrFromObj(interp, objv[0], threadType, dest) == TCL_OK) {
            if (*dest == NULL) {
                Ns_TclPrintfResult(interp, "ns_thread: could not convert '%s' to Ns_Thread object",
                                   Tcl_GetString(objv[0]));
            } else {
                *objcPtr -= 1;
                result = TCL_OK;
            }
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * DestroyHelper --
 *
 *      Factored-out code used identical in many of the ns_* sync
 *      interfaces as a delete operation.  The destroy operation a
 *      no-op, since the synchronization objects are normally created
 *      at process startup and exist until the process exits.
 *
 * Results:
 *      NS_OK or NS_ERROR.  String result of script available via interp.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
DestroyHelper(Ns_ObjvSpec args[], Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv ) {
    return ((Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) ? TCL_ERROR : TCL_OK);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclThread --
 *
 *      Run a Tcl script in a new thread and wait for the result.
 *
 * Results:
 *      NS_OK or NS_ERROR.  String result of script available via thrPtr.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclThread(Tcl_Interp *interp, const char *script, Ns_Thread *thrPtr)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(script != NULL);

    return CreateTclThread(NsGetInterpData(interp), script, (thrPtr == NULL),
                           "tcl", thrPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclDetachedThread --
 *
 *      Run a Tcl script in a detached thread, returning immediately.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      Result of script is discarded.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TclDetachedThread(Tcl_Interp *interp, const char *script)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(script != NULL);

    return Ns_TclThread(interp, script, NULL);
}




/*
 *----------------------------------------------------------------------
 *
 * ThreadCreateObjCmd --
 *
 *      Implements "ns_thread create. Creates a new Tcl thread to
 *      execute a specified script in this thread.
 *
 *      This command parses command-line options to determine thread attributes.
 *      It accepts the following options:
 *          -detached  : When set, the thread is created in detached mode,
 *                       meaning its result is not captured.
 *          -name      : Specifies a name for the thread (default is "nsthread").
 *
 *      The "script" argument is the Tcl script to be executed in the
 *      new thread.  For non-detached threads, the thread's identifier
 *      is returned as the result.
 *
 * Results:
 *      A standard Tcl result (TCL_OK on success, TCL_ERROR on failure).
 *
 * Side effects:
 *      A new thread is spawned to execute the provided Tcl script.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    int             result = TCL_OK, isDetached = (int)NS_FALSE;
    char           *threadName = (char *)"nsthread", *scriptString;
    Ns_ObjvSpec opts[] = {
        {"-detached", Ns_ObjvBool,   &isDetached, INT2PTR(NS_TRUE)},
        {"-name",     Ns_ObjvString, &threadName, NULL},
        {"--",        Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"script", Ns_ObjvString, &scriptString, NULL},
        {NULL, NULL, NULL, NULL}
    };

#ifdef NS_WITH_DEPRECATED
    if (objc > 1) {
        const char  *subcmdName = Tcl_GetString(objv[1]);

        if (*subcmdName == 'b' && strcmp(subcmdName, "begin") == 0) {
            Ns_LogDeprecated(objv, 2, "ns_thread create ...", NULL);
        } else if (*subcmdName == 'b' && strcmp(subcmdName, "begindetached") == 0) {
            Ns_LogDeprecated(objv, 2, "ns_thread create -detached ...", NULL);
            isDetached = (int)NS_TRUE;
        }
    }
#endif

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_ReturnCode status;
        Ns_Thread     tid;

        if (isDetached == (int)NS_TRUE) {
            status = CreateTclThread(itPtr, scriptString, NS_TRUE, threadName, NULL);
        } else {
            status = CreateTclThread(itPtr, scriptString, NS_FALSE, threadName, &tid);
            if (status == NS_OK) {
                Ns_TclSetAddrObj(Tcl_GetObjResult(interp), threadType, tid);
            }
        }
        if (status != NS_OK) {
            Ns_TclPrintfResult(interp, "cannot create thread");
            result = TCL_ERROR;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCreateObjCmd --
 *
 *      This function implements "ns_thread handle". It retrieves the
 *      handle (i.e. a pointer or reference) of the current
 *      thread. The function obtains the current thread using
 *      Ns_ThreadSelf() and returns the handle as a Tcl opaque object.
 *
 * Results:
 *      A standard Tcl result. On success, the current thread handle is set as
 *      the command result.
 *
 * Side effects:
 *      May set an error message in interp if argument parsing fails.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadHandleObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

#ifdef NS_WITH_DEPRECATED
    if (objc > 1) {
        const char  *subcmdName = Tcl_GetString(objv[1]);
        if (*subcmdName == 'g' && strcmp(subcmdName, "get") == 0) {
            Ns_LogDeprecated(objv, 2, "ns_thread handle ...", NULL);
        }
    }
#endif

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_Thread threadPtr;

        Ns_ThreadSelf(&threadPtr);
        assert(threadPtr != NULL);
        Ns_TclSetAddrObj(Tcl_GetObjResult(interp), threadType, threadPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadIdObjCmd --
 *
 *      This command implements "ns_thread id". It retrieves the
 *      unique identifier of the current thread by calling
 *      Ns_ThreadId(), formats the result as a hexadecimal string, and
 *      returns it as the Tcl command result.
 *
 * Results:
 *      A standard Tcl result leaving the thread's unique identifier
 *      in the interpreter result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadIdObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

#ifdef NS_WITH_DEPRECATED
    if (objc > 1) {
        const char  *subcmdName = Tcl_GetString(objv[1]);
        if (*subcmdName == 'g' && strcmp(subcmdName, "getid") == 0) {
            Ns_LogDeprecated(objv, 2, "ns_thread id ...", NULL);
        }
    }
#endif

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_TclPrintfResult(interp, "%" PRIxPTR, Ns_ThreadId());
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadNameObjCmd --
 *
 *      This command implements "ns_thread name". It sets or retrieves
 *      the name of the current thread. If a name argument is
 *      provided, it updates the thread's name using
 *      Ns_ThreadSetName(); otherwise, it retrieves the current thread
 *      name via Ns_ThreadGetName() and returns it as the Tcl command
 *      result.
 *
 * Results:
 *      A standard Tcl result, leaving the thread's name in the
 *      interpreter result.
 *
 * Side effects:
 *      May change the current thread's name if an argument is provided.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadNameObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *nameString = NULL;
    Ns_ObjvSpec args[] = {
        {"?name", Ns_ObjvString, &nameString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        if (nameString != NULL) {
            Ns_ThreadSetName("%s", nameString);
        }
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_ThreadGetName(), TCL_INDEX_NONE));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadStackinfoObjCmd --
 *
 *      This command implements "ns_thread stackinfo". It retrieves
 *      information about the current thread's stack usage, including
 *      the maximum stack size and the estimated free stack space.
 *      The information is formatted as a string and returned as the
 *      command result.
 *
 * Results:
 *      A standard Tcl result containing a formatted string with the stack
 *      information (max stack size and free space).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ThreadStackinfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        size_t maxStackSize, estimatedSize;

        Ns_ThreadGetThreadInfo(&maxStackSize, &estimatedSize);
        Ns_TclPrintfResult(interp, "max %" PRIdz " free %" PRIdz,
                           maxStackSize, maxStackSize - estimatedSize);
        Ns_TclPrintfResult(interp, "%" PRIxPTR, Ns_ThreadId());
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadYieldObjCmd --
 *
 *      This command implements "ns_thread yield". It yields the
 *      processor, allowing other threads to run. This command does
 *      not return any value, but ensures that the calling thread
 *      gives up its current time slice.
 *
 * Results:
 *      A standard Tcl result. On success, the command returns TCL_OK.
 *
 * Side effects:
 *      Causes the calling thread to yield its execution, potentially allowing
 *      other threads to run.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadYieldObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_ThreadYield();
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadWaitObjCmd --
 *
 *      This command implements "ns_thread wait". It waits for the specified
 *      thread (provided as a thread identifier) to terminate. Once the thread
 *      terminates, the command returns any result produced by that thread.
 *
 * Results:
 *      A standard Tcl result. On success, if the joined thread produced a
 *      result string, that string is returned as the command result.
 *
 * Side effects:
 *      The calling thread will block until the specified thread terminates.
 *
 *----------------------------------------------------------------------
 */
static int
ThreadWaitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Thread   thread;
    Ns_ObjvSpec args[] = {
        {"threadid", ObjvThreadObj, &thread, NULL},
        {NULL, NULL, NULL, NULL}
    };

#ifdef NS_WITH_DEPRECATED
    if (strcmp(Tcl_GetString(objv[1]), "join") == 0) {
        Ns_LogDeprecated(objv, 2, "ns_thread wait ...", NULL);
    }
#endif

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        void *arg;
        char *resultPtr;

        Ns_ThreadJoin(&thread, &arg);
        /*
         * The joined thread might provide a result string,
         * generated by NsTclThread() via
         * Ns_DStringExport(&ds). If this is the case, set the
         * result to that string and use ns_free() for cleanup.
         */
        resultPtr = Ns_ThreadResult(arg);
        Ns_Log(Debug, "=== WAIT for %p -> join DONE got arg %p result %p",
               (void*)thread, (void*)arg, (void*)resultPtr);
        if (resultPtr != NULL) {
            Tcl_SetResult(interp, resultPtr,
                          (Tcl_FreeProc *)ns_free);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclThreadObjCmd --
 *
 *      This command implements "ns_thread" and its subcommands. It
 *      provides a Tcl interface for managing threads by dispatching
 *      operations such as creating new threads, retrieving thread
 *      handles, obtaining thread IDs, setting or getting thread
 *      names, retrieving stack information, waiting for a thread to
 *      finish, and yielding the processor. The command routes the
 *      specified subcommand to the corresponding helper function.
 *
 * Results:
 *      A standard Tcl result, which varies depending on the executed subcommand.
 *
 * Side effects:
 *      May create new threads, block while waiting for threads to finish, or yield
 *      the current thread.
 *
 *----------------------------------------------------------------------
 */

int
NsTclThreadObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
#ifdef NS_WITH_DEPRECATED
        {"begin",         ThreadCreateObjCmd},
        {"begindetached", ThreadCreateObjCmd},
#endif
        {"create",        ThreadCreateObjCmd},
#ifdef NS_WITH_DEPRECATED
        {"get",           ThreadHandleObjCmd},
        {"getid",         ThreadIdObjCmd},
#endif
        {"handle",        ThreadHandleObjCmd},
        {"id",            ThreadIdObjCmd},
#ifdef NS_WITH_DEPRECATED
        {"join",          ThreadWaitObjCmd},
#endif
        {"name",          ThreadNameObjCmd},
        {"stackinfo",     ThreadStackinfoObjCmd},
        {"wait",          ThreadWaitObjCmd},
        {"yield",         ThreadYieldObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * MutexCreateObjCmd --
 *
 *      This command implements "ns_mutex create". It creates a new mutex
 *      synchronization object. An optional name can be provided to label
 *      the mutex; if no name is specified, the mutex is created as unnamed.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Allocates and initializes a new mutex object.
 *
 *----------------------------------------------------------------------
 */
static int
MutexCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *nameString = NULL;
    Ns_ObjvSpec args[] = {
        {"?name", Ns_ObjvString, &nameString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_Mutex       *lockPtr;
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;

        lockPtr = CreateSynchObject(itPtr,
                                    &servPtr->tcl.synch.mutexTable,
                                    &servPtr->tcl.synch.mutexId,
                                    (Ns_Callback *) Ns_MutexInit,
                                    mutexType,
                                    NULL,
                                    TCL_INDEX_NONE);
        if (nameString != NULL) {
            /*
             * If a name was provided, name the mutex created with
             * CreateSynchObject().
             */
            Ns_MutexSetName(lockPtr, nameString);
        } else {
            Ns_Log(Notice, "created unnamed syncobj %s %p",Ns_MutexGetName(lockPtr), (void*)lockPtr);
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * MutexDestroyObjCmd --
 *
 *      This command implements "ns_mutex destroy". It destroys an existing
 *      mutex synchronization object, freeing its associated resources.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      The specified mutex object is deallocated.
 *
 *----------------------------------------------------------------------
 */
static int
MutexDestroyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_Mutex   *lockPtr;
    Ns_ObjvSpec args[] = {
        {"mutexId", ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    return DestroyHelper(args, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * MutexEvalObjCmd --
 *
 *      This command implements "ns_mutex eval". It evaluates a Tcl script
 *      while holding the mutex lock, ensuring that the execution of the
 *      script is performed in a thread-safe context.
 *
 * Results:
 *      A standard Tcl result reflecting the outcome of the script evaluation.
 *
 * Side effects:
 *      The mutex is locked before the script is executed and unlocked afterward.
 *
 *----------------------------------------------------------------------
 */
static int
MutexEvalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Tcl_Obj    *scriptObj = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexId", ObjvMutexObj, &lockPtr, NULL},
        {"script",  Ns_ObjvObj,   &scriptObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_MutexLock(lockPtr);
        result = Tcl_EvalObjEx(interp, scriptObj, 0);
        Ns_MutexUnlock(lockPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * MutexLockObjCmd --
 *
 *      This command implements "ns_mutex lock". It locks the specified mutex,
 *      blocking the calling thread until the lock can be acquired.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      The specified mutex is locked.
 *
 *----------------------------------------------------------------------
 */
static int
MutexLockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexId", ObjvMutexObj, &lockPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_MutexLock(lockPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * MutexTrylockObjCmd --
 *
 *      This command implements "ns_mutex trylock". It attempts to
 *      lock the specified mutex without blocking. If the mutex is
 *      already locked, it returns a value indicating that the lock
 *      could not be acquired.
 *
 * Results:
 *      A standard Tcl result with an integer value (nonzero if the
 *      lock was acquired, zero otherwise).
 *
 * Side effects:
 *      The specified mutex may be locked if it was available.
 *
 *----------------------------------------------------------------------
 */
static int
MutexTrylockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexId", ObjvMutexObj, &lockPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        int rc = Ns_MutexTryLock(lockPtr) == TCL_OK ? 0 : -1;
        Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * MutexUnlockObjCmd --
 *
 *      This command implements "ns_mutex unlock". It releases the
 *      lock on the specified mutex.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      The specified mutex is unlocked.
 *
 *----------------------------------------------------------------------
 */
static int
MutexUnlockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexId", ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_MutexUnlock(lockPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclMutexObjCmd --
 *
 *      This command implements the "ns_mutex" command, which provides a unified
 *      interface for various mutex-related subcommands, including create, destroy,
 *      eval, lock, trylock, and unlock.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on the subcommand invoked.
 *
 *----------------------------------------------------------------------
 */
int
NsTclMutexObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"create",  MutexCreateObjCmd},
        {"destroy", MutexDestroyObjCmd},
        {"eval",    MutexEvalObjCmd},
        {"lock",    MutexLockObjCmd},
        {"trylock", MutexTrylockObjCmd},
        {"unlock",  MutexUnlockObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * CondBroadcastObjCmd --
 *
 *      This command implements "ns_cond broadcast". It broadcasts a signal
 *      to all threads waiting on the specified condition variable.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      All threads waiting on the condition variable are awakened.
 *
 *----------------------------------------------------------------------
 */

static int
CondBroadcastObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int        result = TCL_OK;
    Ns_Cond   *condPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"condId", ObjvCondObj, &condPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_CondBroadcast(condPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CondCreateObjCmd --
 *
 *      This command implements "ns_cond create". It creates a new
 *      condition variable for thread synchronization.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      A new condition variable is allocated and initialized.
 *
 *----------------------------------------------------------------------
 */
static int
CondCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;

        (void) CreateSynchObject(itPtr,
                                 &servPtr->tcl.synch.condTable,
                                 &servPtr->tcl.synch.condId,
                                 (Ns_Callback *) Ns_CondInit,
                                 condType,
                                 NULL,
                                 TCL_INDEX_NONE);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CondDestroyObjCmd --
 *
 *      This command implements "ns_cond destroy". It destroys the specified
 *      condition variable and frees associated resources.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      The condition variable is removed and its memory is deallocated.
 *
 *----------------------------------------------------------------------
 */
static int
CondDestroyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_Cond    *lockPtr;
    Ns_ObjvSpec args[] = {
        {"condId", ObjvCondObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    return DestroyHelper(args, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * CondSignalObjCmd --
 *
 *      This command implements "ns_cond signal" (also aliased as
 *      "ns_cond set").  It signals the specified condition variable,
 *      waking one waiting thread.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      One thread waiting on the condition variable is notified.
 *
 *----------------------------------------------------------------------
 */
static int
CondSignalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cond    *condPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"condId", ObjvCondObj, &condPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_CondSignal(condPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CondAbswaitObjCmd --
 *
 *      This command implements "ns_cond abswait". It waits on the specified
 *      condition variable until a given absolute time is reached.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      The calling thread is blocked until the condition is signaled or the
 *      specified absolute timeout expires.
 *
 *----------------------------------------------------------------------
 */
static int
CondAbswaitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cond    *condPtr = NULL;
    Ns_Mutex   *lockPtr = NULL;
    Ns_Time     timeout = {0, 0};
    long        epoch = -1;
    Ns_ObjvSpec args[] = {
        {"condId",   ObjvCondObj,  &condPtr, INT2PTR(NS_TRUE)},
        {"mutexId",  ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
        {"?epoch",  Ns_ObjvLong,   &epoch, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_ReturnCode status;
        /*
         * Absolute time wait: ns_cond abswait
         */
        if (epoch >= 0 ) {
            timeout.sec = epoch;
        }
        status = Ns_CondTimedWait(condPtr, lockPtr, &timeout);

        if (status == NS_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        } else if (status == NS_TIMEOUT) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        } else {
            result = TCL_ERROR;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CondWaitObjCmd --
 *
 *      This command implements "ns_cond wait". It waits on the specified
 *      condition variable for a relative timeout period (if provided).
 *
 * Results:
 *      A standard Tcl result indicating whether the wait succeeded or
 *      timed out.
 *
 * Side effects:
 *      The calling thread is blocked until the condition is signaled or the
 *      timeout expires.
 *
 *----------------------------------------------------------------------
 */
static int
CondWaitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cond    *condPtr = NULL;
    Ns_Mutex   *lockPtr = NULL;
    Ns_Time    *timeoutPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"condId",   ObjvCondObj,  &condPtr, INT2PTR(NS_TRUE)},
        {"mutexId",  ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
        {"?timeout", Ns_ObjvTime,  &timeoutPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_ReturnCode status;
        /*
         * Relative time wait: ns_cond wait
         */
        if (timeoutPtr == NULL) {
            Ns_CondWait(condPtr, lockPtr);
            status = NS_OK;
        } else {
            Ns_Time       abstime;

            Ns_GetTime(&abstime);
            Ns_IncrTime(&abstime, timeoutPtr->sec, timeoutPtr->usec);
            status = Ns_CondTimedWait(condPtr, lockPtr, &abstime);
        }
        if (status == NS_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        } else if (status == NS_TIMEOUT) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        } else {
            result = TCL_ERROR;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCondObjCmd --
 *
 *      This command implements the "ns_cond" command, providing a
 *      unified interface to condition variable operations. Its
 *      subcommands include: abswait, broadcast, create, destroy, set
 *      (signal), and wait.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on the specific subcommand invoked.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCondObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"abswait",   CondAbswaitObjCmd},
        {"broadcast", CondBroadcastObjCmd},
        {"create",    CondCreateObjCmd},
        {"destroy",   CondDestroyObjCmd},
        {"set",       CondSignalObjCmd},
        {"signal",    CondSignalObjCmd},
        {"wait",      CondWaitObjCmd},
        {NULL, NULL}
    };

#ifdef NS_WITH_DEPRECATED
    if (objc > 1) {
        const char  *cmdName = Tcl_GetString(objv[0]);

        if (strcmp(cmdName, "ns_event") == 0) {
            Ns_LogDeprecated(objv, 2, "ns_cond ...", NULL);
        }
    }
#endif
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * RWLockCreateObjCmd --
 *
 *      This command implements "ns_rwlock create". It creates a new
 *      read-write lock, allocating and initializing the necessary
 *      data structures.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Allocates and initializes a new read-write lock object.
 *
 *----------------------------------------------------------------------
 */
static int
RWLockCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;

        (void) CreateSynchObject(itPtr,
                                 &servPtr->tcl.synch.rwTable,
                                 &servPtr->tcl.synch.rwId,
                                 (Ns_Callback *) Ns_RWLockInit,
                                 rwType,
                                 NULL,
                                 TCL_INDEX_NONE);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * RWLockDestroyObjCmd --
 *
 *      This command implements "ns_rwlock destroy". It destroys the
 *      specified read-write lock and frees all associated resources.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Deallocates the read-write lock object and removes it from
 *      internal tables.
 *
 *----------------------------------------------------------------------
 */
static int
RWLockDestroyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_RWLock  *lockPtr;
    Ns_ObjvSpec args[] = {
        {"rwlockid", ObjvRWLockObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    return DestroyHelper(args, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * RWLockReadlockObjCmd --
 *
 *      This command implements "ns_rwlock readlock". It acquires a
 *      read lock on the specified read-write lock, blocking if
 *      necessary until the lock is available.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Blocks the calling thread until a read lock is acquired.
 *
 *----------------------------------------------------------------------
 */
static int
RWLockReadlockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_RWLock  *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"rwlockid", ObjvRWLockObj, &lockPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_RWLockRdLock(lockPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * RWLockReadevalObjCmd --
 *
 *      This command implements "ns_rwlock readeval". It acquires a
 *      read lock, evaluates the provided Tcl script while holding the
 *      lock, and then releases the lock.
 *
 * Results:
 *      A standard Tcl result containing the output of the evaluated script.
 *
 * Side effects:
 *      The specified Tcl script is executed while the read lock is held.
 *
 *----------------------------------------------------------------------
 */
static int
RWLockReadevalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_RWLock  *lockPtr = NULL;
    Tcl_Obj    *scriptObj = NULL;
    Ns_ObjvSpec args[] = {
        {"rwlockid", ObjvRWLockObj, &lockPtr, NULL},
        {"script",   Ns_ObjvObj,    &scriptObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_RWLockRdLock(lockPtr);
        result = Tcl_EvalObjEx(interp, scriptObj, 0);
        Ns_RWLockUnlock(lockPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * RWLockWritelockObjCmd --
 *
 *      This command implements "ns_rwlock writelock". It acquires a
 *      write lock on the specified read-write lock, blocking until
 *      the lock is available.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Blocks the calling thread until a write lock is acquired.
 *
 *----------------------------------------------------------------------
 */
static int
RWLockWritelockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_RWLock  *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"rwlockid", ObjvRWLockObj, &lockPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_RWLockWrLock(lockPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * RWLockWriteevalObjCmd --
 *
 *      This command implements "ns_rwlock writeeval". It acquires a write lock,
 *      evaluates the given Tcl script while holding the lock, and then releases
 *      the lock.
 *
 * Results:
 *      A standard Tcl result with the output of the evaluated script.
 *
 * Side effects:
 *      The provided Tcl script is executed while the write lock is held.
 *
 *----------------------------------------------------------------------
 */
static int
RWLockWriteevalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_RWLock  *lockPtr = NULL;
    Tcl_Obj    *scriptObj = NULL;
    Ns_ObjvSpec args[] = {
        {"rwlockid", ObjvRWLockObj, &lockPtr, NULL},
        {"script",   Ns_ObjvObj,    &scriptObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_RWLockWrLock(lockPtr);
        result = Tcl_EvalObjEx(interp, scriptObj, 0);
        Ns_RWLockUnlock(lockPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * RWLockUnlockObjCmd --
 *
 *      This command implements "ns_rwlock unlock", "ns_rwlock
 *      readunlock" and "ns_rwlock writeunlock". It releases the
 *      currently held read or write lock on the specified read-write
 *      lock.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Releases the lock, allowing other threads to acquire it.
 *
 *----------------------------------------------------------------------
 */
static int
RWLockUnlockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_RWLock   *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"rwlockid", ObjvRWLockObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_RWLockUnlock(lockPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclRWLockObjCmd --
 *
 *      This command implements "ns_rwlock", providing a unified interface
 *      for read-write lock operations. Its subcommands include: create,
 *      destroy, readeval, readlock, readunlock, writeeval, writelock, and
 *      writeunlock.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on the specific subcommand invoked.
 *
 *----------------------------------------------------------------------
 */
int
NsTclRWLockObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"create",      RWLockCreateObjCmd},
        {"destroy",     RWLockDestroyObjCmd},
        {"readeval",    RWLockReadevalObjCmd},
        {"readlock",    RWLockReadlockObjCmd},
        {"readunlock",  RWLockUnlockObjCmd},
        {"unlock",      RWLockUnlockObjCmd},
        {"writeeval",   RWLockWriteevalObjCmd},
        {"writelock",   RWLockWritelockObjCmd},
        {"writeunlock", RWLockUnlockObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * CsCreateObjCmd --
 *
 *      This command implements "ns_critsec create". It creates a new
 *      critical section object and registers it for later use.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Allocates and initializes a new critical section object.
 *
 *----------------------------------------------------------------------
 */
static int
CsCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;

        (void) CreateSynchObject(itPtr,
                                 &servPtr->tcl.synch.csTable,
                                 &servPtr->tcl.synch.csId,
                                 (Ns_Callback *) Ns_CsInit,
                                 csType,
                                 NULL,
                                 TCL_INDEX_NONE);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CsDestroyObjCmd --
 *
 *      This command implements "ns_critsec destroy". It destroys the
 *      specified critical section object, freeing any resources
 *      associated with it.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Deallocates the critical section object and removes it from internal
 *      management.
 *
 *----------------------------------------------------------------------
 */
static int
CsDestroyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_Cs   *lockPtr;
    Ns_ObjvSpec args[] = {
        {"csid", ObjvCsObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    return DestroyHelper(args, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * CsEnterObjCmd --
 *
 *      This command implements "ns_critsec enter". It acquires the
 *      lock on the specified critical section, blocking until the
 *      lock is available.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Blocks the calling thread until the critical section lock is acquired.
 *
 *----------------------------------------------------------------------
 */
static int
CsEnterObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cs      *csPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"csid", ObjvCsObj, &csPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_CsEnter(csPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CsEvalObjCmd --
 *
 *      This command implements "ns_critsec eval". It acquires the
 *      critical section lock, evaluates a provided Tcl script while
 *      holding the lock, and then releases the lock.
 *
 * Results:
 *      A standard Tcl result containing the output of the evaluated script.
 *
 * Side effects:
 *      Executes the Tcl script while the critical section lock is held.
 *
 *----------------------------------------------------------------------
 */
static int
CsEvalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cs      *csPtr = NULL;
    Tcl_Obj    *scriptObj = NULL;
    Ns_ObjvSpec args[] = {
        {"csid",   ObjvCsObj,  &csPtr, NULL},
        {"script", Ns_ObjvObj, &scriptObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_CsEnter(csPtr);
        result = Tcl_EvalObjEx(interp, scriptObj, 0);
        Ns_CsLeave(csPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CsLeaveObjCmd --
 *
 *      This command implements "ns_critsec leave". It releases the
 *      lock on the specified critical section, allowing other threads
 *      to acquire it.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Releases the critical section lock.
 *
 *----------------------------------------------------------------------
 */
static int
CsLeaveObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cs      *csPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"csid", ObjvCsObj, &csPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_CsLeave(csPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclCritSecObjCmd --
 *
 *      This command implements "ns_critsec", providing a unified interface for
 *      critical section operations. Its subcommands include "create", "destroy",
 *      "enter", "eval", and "leave".
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on the subcommand invoked.
 *
 *----------------------------------------------------------------------
 */
int
NsTclCritSecObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"create",  CsCreateObjCmd},
        {"destroy", CsDestroyObjCmd},
        {"enter",   CsEnterObjCmd},
        {"eval",    CsEvalObjCmd},
        {"leave",   CsLeaveObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * SemaCreateObjCmd --
 *
 *      This command implements "ns_sema create". It creates a new
 *      semaphore object with an optional initial count and registers
 *      it for later use.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Allocates and initializes a new semaphore object.
 *
 *----------------------------------------------------------------------
 */
static int
SemaCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        count = 0;
    Ns_ObjvSpec args[] = {
        {"?count", Ns_ObjvLong, &count, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;

        (void) CreateSynchObject(itPtr,
                                 &servPtr->tcl.synch.semaTable,
                                 &servPtr->tcl.synch.semaId,
                                 NULL,
                                 semaType,
                                 NULL,
                                 (TCL_SIZE_T)count);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SemaDestroyObjCmd --
 *
 *      This command implements "ns_sema destroy". It destroys the specified
 *      semaphore object, freeing all associated resources.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Deallocates the semaphore object and removes it from internal management.
 *
 *----------------------------------------------------------------------
 */
static int
SemaDestroyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_Sema    *lockPtr;
    Ns_ObjvSpec args[] = {
        {"handle", ObjvSemaObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    return DestroyHelper(args, interp, objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * SemaReleaseObjCmd --
 *
 *      This command implements "ns_sema release". It increments
 *      (posts) the semaphore by the specified count (defaulting to
 *      one if not provided).
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Increases the semaphore's count, potentially unblocking
 *      waiting threads.
 *
 *----------------------------------------------------------------------
 */
static int
SemaReleaseObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        count = 1;
    Ns_Sema    *semaPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"handle", ObjvSemaObj, &semaPtr, NULL},
        {"?count", Ns_ObjvLong, &count,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_SemaPost(semaPtr, (TCL_SIZE_T)count);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SemaWaitObjCmd --
 *
 *      This command implements "ns_sema wait". It decrements (waits
 *      on) the semaphore, blocking the calling thread until the
 *      semaphore count is greater than zero.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      May block the calling thread until the semaphore is available.
 *
 *----------------------------------------------------------------------
 */

static int
SemaWaitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Sema    *semaPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"handle", ObjvSemaObj, &semaPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_SemaWait(semaPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclSemaObjCmd --
 *
 *      This command implements "ns_sema", providing a unified
 *      interface for semaphore operations. Its subcommands include
 *      "create", "destroy", "release", and "wait".
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on the subcommand invoked.
 *
 *----------------------------------------------------------------------
 */
int
NsTclSemaObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"create",  SemaCreateObjCmd},
        {"destroy", SemaDestroyObjCmd},
        {"release", SemaReleaseObjCmd},
        {"wait",    SemaWaitObjCmd},
        {NULL, NULL}
    };
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * ThreadArgFree --
 *
 *      Free the argument structure of an NsTclThread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Free memory.
 *
 *----------------------------------------------------------------------
 */
static void ThreadArgFree(void *arg)
{
    TclThreadArg *argPtr;

    NS_NONNULL_ASSERT(arg != NULL);
    argPtr = (TclThreadArg *)arg;

    ns_free((char *)argPtr->threadName);
    ns_free(argPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * InitializeTls --
 *
 *      Initialize once the data structures needed for thread local
 *      storage.
 *
 * Results:
 *      Boolean value, has to return NS_TRUE for Windows compatibility.
 *
 * Side effects:
 *      One-time initialization.
 *
 *----------------------------------------------------------------------
 */
static bool InitializeTls(void) {

    //fprintf(stderr, "==== InitializeTls\n");
#if defined(_WIN32) || defined(HAVE_PTHREAD)
     Ns_MasterLock();
#endif

    Ns_TlsAlloc(&argtls, ThreadArgFree);

#if defined(_WIN32) || defined(HAVE_PTHREAD)
     Ns_MasterUnlock();
#endif

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclThread --
 *
 *      Tcl thread main.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Copy of string result is return as exit arg to be reaped
 *      by ns_thread wait.
 *
 *----------------------------------------------------------------------
 */
void
NsTclThread(void *arg)
{
    TclThreadArg    *argPtr = arg;
    Tcl_DString      ds, *dsPtr;
    bool             detached;

    NS_NONNULL_ASSERT(arg != NULL);

    /*
     * The argument structure is a TclThreadArg, which has to be freed
     * when the thread shuts down.  The argument is used e.g. by the
     * arg proc in Ns_ThreadList(), which might be called during
     * thread shutdown. To ensure consistent cleanup in all success
     * and error cases, we use a thread local variable with
     * ThreadArgFree() as cleanup proc.
     *
     * On the first call, allocate the thread local storage slot. This
     * initialization might be moved into some tclThreadInit() code,
     * which does not exist.
     */
    NS_INIT_ONCE(InitializeTls);

    Ns_TlsSet(&argtls, argPtr);

    if (argPtr->threadName != NULL) {
        static uintptr_t id = 0u;

        Ns_ThreadSetName("-tcl-%s:%" PRIuPTR "-",
                         argPtr->threadName, id++);
    }

    detached = argPtr->detached;
    if (detached) {
        dsPtr = NULL;
    } else {
        Tcl_DStringInit(&ds);
        dsPtr = &ds;
    }

    /*
     * Need to ensure that the server has completed its
     * initialization prior to initiating TclEval.
     */
    (void) Ns_WaitForStartup();
    (void) Ns_TclEval(dsPtr, argPtr->server, argPtr->script);

    /*
     * No matter if the Tcl eval was successful or not, return in the
     * non-detached case the dstring result, since some other thread
     * might be waiting for a result. In the detached case, there is
     * no dstring content.
     */
    if (!detached) {
        Ns_ThreadExit(Ns_DStringExport(&ds));
    } else {
        Ns_ThreadExit(NULL);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclThreadArgProc --
 *
 *      Proc info callback to describe a Tcl thread.
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
NsTclThreadArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const TclThreadArg *argPtr = arg;

    Tcl_DStringAppendElement(dsPtr, argPtr->server);
    if (argPtr->detached) {
        Tcl_DStringAppendElement(dsPtr, "detached");
    }
    Tcl_DStringAppendElement(dsPtr, argPtr->script);
}


/*
 *----------------------------------------------------------------------
 *
 * CreateTclThread --
 *
 *      Create a new Tcl thread.
 *
 * Results:
 *      Ns_ReturnCode (NS_OK or NS_ERROR).
 *
 * Side effects:
 *      Depends on Tcl script.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CreateTclThread(const NsInterp *itPtr, const char *script, bool detached,
                const char *threadName, Ns_Thread *thrPtr)
{
    TclThreadArg *argPtr;
    size_t        scriptLength;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(script != NULL);
    NS_NONNULL_ASSERT(threadName != NULL);

    scriptLength = strlen(script);
    argPtr = ns_malloc(sizeof(TclThreadArg) + scriptLength);
    if (likely(argPtr != NULL)) {
        argPtr->detached = detached;
        argPtr->threadName = ns_strdup(threadName);
        memcpy(argPtr->script, script, scriptLength + 1u);

        if (itPtr->servPtr != NULL) {
            argPtr->server = itPtr->servPtr->server;
        } else {
            argPtr->server = NULL;
        }
        Ns_ThreadCreate(NsTclThread, argPtr, 0, thrPtr);
        result = NS_OK;
    } else {
        result = NS_ERROR;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateSynchObject --
 *
 *      This function creates and initializes a new synchronization
 *      object of the specified type (such as a mutex, semaphore,
 *      condition, etc.), or returns an existing one if an object with
 *      the same name is already present in the provided hash
 *      table. If a Tcl object is provided and already holds an opaque
 *      pointer for the given type, that object is returned directly.
 *
 *      When creating a new object, the function optionally calls an
 *      initialization callback (if provided) and, for certain types
 *      (e.g., mutexes), sets a name for the object. The new object is
 *      stored in the specified hash table and associated with a Tcl
 *      opaque object for later retrieval.
 *
 * Results:
 *      Returns a pointer to the synchronization object (either newly
 *      created or previously existing).
 *
 * Side effects:
 *      May allocate memory and update the synchronization object's
 *      hash table. It also sets the Tcl object result to the opaque
 *      object representing the synchronization object.
 *
 *----------------------------------------------------------------------
 */
static void *
CreateSynchObject(const NsInterp *itPtr,
                  Tcl_HashTable *typeTable, unsigned int *idPtr,
                  Ns_Callback *initProc, const char *type,
                  Tcl_Obj *objPtr, TCL_SIZE_T cnt)
{
    NsServer      *servPtr;
    Tcl_Interp    *interp;
    void          *addr;
    int            isNew = 0;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(typeTable != NULL);
    NS_NONNULL_ASSERT(idPtr != NULL);
    NS_NONNULL_ASSERT(type != NULL);

    interp  = itPtr->interp;

    if (objPtr != NULL
        && Ns_TclGetOpaqueFromObj(objPtr, type, &addr) == TCL_OK
        ) {
        Tcl_SetObjResult(interp, objPtr);
    } else {
        Tcl_HashEntry *hPtr;

        servPtr = itPtr->servPtr;
        Ns_MutexLock(&servPtr->tcl.synch.lock);

        if (objPtr == NULL) {
            Tcl_DString    ds;

            Tcl_DStringInit(&ds);
            do {
                Tcl_DStringSetLength(&ds, 0);
                Ns_DStringPrintf(&ds, "%s:tcl:%u", type, (*idPtr)++);
                hPtr = Tcl_CreateHashEntry(typeTable, ds.string, &isNew);
            } while (isNew == 0);

            objPtr = Tcl_NewStringObj(ds.string, ds.length);
            Tcl_SetObjResult(interp, objPtr);
            Tcl_DStringFree(&ds);

        } else if (likely(initProc != NULL)) {
            /*
             * When an initProc is specified, create automatically a
             * sync object, even it it does not pre-exist.
             */
            hPtr = Tcl_CreateHashEntry(typeTable, Tcl_GetString(objPtr), &isNew);
            //fprintf(stderr, "Lookup from obj '%s' -> isNew %d\n", Tcl_GetString(objPtr), isNew);
            //Tcl_SetObjResult(interp, objPtr);
        } else {
            /*
             * Perform just a lookup.
             */
            hPtr = Tcl_FindHashEntry(typeTable, Tcl_GetString(objPtr));
            isNew = 0;
        }

        if (isNew != 0) {
            addr = ns_calloc(1u, sizeof(void *));
            if (type == semaType && cnt != TCL_INDEX_NONE) {
                Ns_SemaInit((Ns_Sema *) addr, cnt);
            } else if (initProc != NULL) {
                (*initProc)(addr);
                /*
                 * Just for mutexes, provide a name
                 */
                if (type == mutexType) {
                    Ns_MutexSetName2(addr, "syncobj", Tcl_GetString(objPtr));
                }
            }
            Tcl_SetHashValue(hPtr, addr);
            Ns_TclSetOpaqueObj(objPtr, type, addr);
        } else if (hPtr != NULL) {
            addr = Tcl_GetHashValue(hPtr);
        } else {
            addr = NULL;
        }
        Ns_MutexUnlock(&servPtr->tcl.synch.lock);
    }
    return addr;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
