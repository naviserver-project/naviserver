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
 * NsTclThreadObjCmd --
 *
 *      Implements "ns_thread". This command provides a script
 *      interface to get data on the current thread and create and
 *      wait on new Tcl-script based threads.  New threads will be
 *      created in the virtual-server context of the current interp,
 *      if any.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May create a new thread or wait for an existing thread to
 *      exit.
 *
 *----------------------------------------------------------------------
 */

int
NsTclThreadObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    void           *tidArg;
    Ns_Thread       tid;
    int             opt, result = TCL_OK;

    static const char *const opts[] = {
        "begin", "begindetached", "create", "wait", "join",
        "name", "get", "getid", "handle", "id", "yield",
        "stackinfo", NULL
    };
    enum {
        TBeginIdx, TBeginDetachedIdx, TCreateIdx, TWaitIdx, TJoinIdx,
        TNameIdx, TGetIdx, TGetIdIdx, THandleIdx, TIdIdx, TYieldIdx,
        TStackinfoIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 0, &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        switch (opt) {
        case TCreateIdx:
            Ns_LogDeprecated(objv, 2, "ns_thread begin ...", NULL);
            NS_FALL_THROUGH; /* fall through */
        case TBeginIdx:
            NS_FALL_THROUGH; /* fall through */
        case TBeginDetachedIdx:
            {
                char          *threadName = (char *)"nsthread", *script;
                Ns_ReturnCode  status;
                Ns_ObjvSpec lopts[] = {
                    {"-name", Ns_ObjvString, &threadName, NULL},
                    {"--",    Ns_ObjvBreak,  NULL,    NULL},
                    {NULL, NULL, NULL, NULL}
                };
                Ns_ObjvSpec args[] = {
                    {"script", Ns_ObjvString, &script, NULL},
                    {NULL, NULL, NULL, NULL}
                };

                if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
                    result = TCL_ERROR;
                    status = NS_ERROR;
                } else if (opt == TBeginDetachedIdx) {
                    status = CreateTclThread(itPtr, script, NS_TRUE, threadName, NULL);
                } else {
                    status = CreateTclThread(itPtr, script, NS_FALSE, threadName, &tid);
                    if (status == NS_OK) {
                        Ns_TclSetAddrObj(Tcl_GetObjResult(interp), threadType, tid);
                    }
                }
                if (status != NS_OK) {
                    Ns_TclPrintfResult(interp, "cannot create thread");
                    result = TCL_ERROR;
                }
                break;
            }

        case TJoinIdx:
            Ns_LogDeprecated(objv, 2, "ns_thread wait ...", NULL);
            NS_FALL_THROUGH; /* fall through */

        case TWaitIdx:
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "tid");
                result = TCL_ERROR;

            } else if (Ns_TclGetAddrFromObj(interp, objv[2], threadType, &tidArg) != TCL_OK) {
                result = TCL_ERROR;

            } else {
                void *arg;
                char *resultPtr;

                tid = tidArg;
                Ns_ThreadJoin(&tid, &arg);
                /*
                 * The joined thread might provide a result string,
                 * generated by NsTclThread() via
                 * Ns_DStringExport(&ds). If this is the case, set the
                 * result to that string and use ns_free() for cleanup.
                 */
                resultPtr = Ns_ThreadResult(arg);
                Ns_Log(Debug, "=== WAIT for %p -> join DONE got arg %p result %p",
                       (void*)tidArg, (void*)arg, (void*)resultPtr);
                if (resultPtr != NULL) {
                    Tcl_SetResult(interp, resultPtr,
                                  (Tcl_FreeProc *)ns_free);
                }
            }
            break;


        case TGetIdx:
            Ns_LogDeprecated(objv, 2, "ns_thread handle ...", NULL);
            NS_FALL_THROUGH; /* fall through */
        case THandleIdx:
            Ns_ThreadSelf(&tid);
            assert(tid != NULL);
            Ns_TclSetAddrObj(Tcl_GetObjResult(interp), threadType, tid);
            break;


        case TGetIdIdx:
            Ns_LogDeprecated(objv, 2, "ns_thread id ...", NULL);
            NS_FALL_THROUGH; /* fall through */
        case TIdIdx:
            Ns_TclPrintfResult(interp, "%" PRIxPTR, Ns_ThreadId());
            break;

        case TNameIdx:
            if (objc > 2) {
                Ns_ThreadSetName("%s", Tcl_GetString(objv[2]));
            }
            Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_ThreadGetName(), TCL_INDEX_NONE));
            break;

        case TStackinfoIdx: {
            size_t maxStackSize, estimatedSize;
            Ns_ThreadGetThreadInfo(&maxStackSize, &estimatedSize);
            Ns_TclPrintfResult(interp, "max %" PRIdz " free %" PRIdz,
                               maxStackSize, maxStackSize - estimatedSize);
            break;
        }

        case TYieldIdx:
            Ns_ThreadYield();
            break;

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclMutexObjCmd --
 *
 *      Implements "ns_mutex".
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
NsTclMutexObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int opt, result = TCL_OK;

    static const char *const opts[] = {
        "create", "destroy", "eval", "lock", "trylock", "unlock", NULL
    };
    enum {
        MCreateIdx, MDestroyIdx, MEvalIdx, MLockIdx, MTryLockIdx, MUnlockIdx
    };
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
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
                                    (objc >= 3) ? objv[2] : NULL,
                                    TCL_INDEX_NONE);
        switch (opt) {
        case MCreateIdx:
            if (objc > 2) {
                /*
                 * If a name was provided, name the mutex created with
                 * CreateSynchObject().
                 */
                Ns_MutexSetName(lockPtr, Tcl_GetString(objv[2]));
            } else {
                Ns_Log(Notice, "created unnamed syncobj %s",Ns_MutexGetName(lockPtr));
            }
            break;

        case MLockIdx:
            Ns_MutexLock(lockPtr);
            break;

        case MTryLockIdx:
            Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_MutexTryLock(lockPtr)));
            break;

        case MUnlockIdx:
            Ns_MutexUnlock(lockPtr);
            break;

        case MEvalIdx:
            if (objc != 4) {
                Tcl_WrongNumArgs(interp, 3, objv, "script");
                result = TCL_ERROR;
            } else {
                Ns_MutexLock(lockPtr);
                result = Tcl_EvalObjEx(interp, objv[3], 0);
                Ns_MutexUnlock(lockPtr);
            }
            break;

        case MDestroyIdx:
            /* No-op. */
            break;

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCritSecObjCmd --
 *
 *      Implements "ns_critsec".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See doc.
 *
 *----------------------------------------------------------------------
 */

int
NsTclCritSecObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int opt, result = TCL_OK;
    static const char *const opts[] = {
        "create", "destroy", "enter", "eval", "leave", NULL
    };
    enum {
        CCreateIdx, CDestroyIdx, CEnterIdx, CEvalIdx, CLeaveIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr   = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        Ns_Cs          *csPtr;

        csPtr = CreateSynchObject(itPtr,
                                  &servPtr->tcl.synch.csTable,
                                  &servPtr->tcl.synch.csId,
                                  (Ns_Callback *) Ns_CsInit,
                                  csType,
                                  (objc >= 3) ? objv[2] : NULL,
                                  TCL_INDEX_NONE);
        switch (opt) {
        case CCreateIdx:
            /* Handled above. */
            break;

        case CEnterIdx:
            Ns_CsEnter(csPtr);
            break;

        case CLeaveIdx:
            Ns_CsLeave(csPtr);
            break;

        case CEvalIdx:
            if (objc != 4) {
                Tcl_WrongNumArgs(interp, 3, objv, "script");
                result = TCL_ERROR;
            } else {
                Ns_CsEnter(csPtr);
                result = Tcl_EvalObjEx(interp, objv[3], 0);
                Ns_CsLeave(csPtr);
            }
            break;

        case CDestroyIdx:
            /* No-op. */
            break;

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSemaObjCmd --
 *
 *      Implements "ns_sema".
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
NsTclSemaObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int                      opt = 0, result = TCL_OK;
    long                     cnt = 0;
    static const char *const opts[] = {
        "create", "destroy", "release", "wait", NULL
    };
    enum {
        SCreateIdx, SDestroyIdx, SReleaseIdx, SWaitIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else if (opt == SCreateIdx && objc == 3) {
        if (Tcl_GetLongFromObj(interp, objv[2], &cnt) != TCL_OK) {
            result = TCL_ERROR;
        }
    }

    if (result == TCL_OK) {
        Ns_Sema        *semaPtr;
        const NsInterp *itPtr = clientData;
        NsServer       *servPtr = itPtr->servPtr;

        semaPtr = CreateSynchObject(itPtr,
                                    &servPtr->tcl.synch.semaTable,
                                    &servPtr->tcl.synch.semaId,
                                    NULL,
                                    semaType,
                                    (objc == 3) ? objv[2] : NULL,
                                    (TCL_SIZE_T)cnt);
        switch (opt) {
        case SCreateIdx:
            /* Handled above. */
            break;

        case SReleaseIdx:
            if (objc < 4) {
                cnt = 1;
            } else if (Tcl_GetLongFromObj(interp, objv[3], &cnt) != TCL_OK) {
                result = TCL_ERROR;
            }
            if (result == TCL_OK) {
                Ns_SemaPost(semaPtr, (TCL_SIZE_T)cnt);
            }
            break;

        case SWaitIdx:
            Ns_SemaWait(semaPtr);
            break;

        case SDestroyIdx:
            /* No-op. */
            break;

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCondObjCmd --
 *
 *      Implements "ns_cond".
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
NsTclCondObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr   = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    Ns_Cond        *condPtr;
    Ns_Time         timeout, abstime;
    int             opt, result = TCL_OK;

    static const char *const opts[] = {
        "abswait", "broadcast", "create", "destroy", "set",
        "signal", "wait", NULL
    };
    enum {
        EAbsWaitIdx, EBroadcastIdx, ECreateIdx, EDestroyIdx, ESetIdx,
        ESignalIdx, EWaitIdx
    };
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    condPtr = CreateSynchObject(itPtr,
                                &servPtr->tcl.synch.condTable,
                                &servPtr->tcl.synch.condId,
                                (Ns_Callback *) Ns_CondInit,
                                condType,
                                (objc >= 3) ? objv[2] : NULL,
                                TCL_INDEX_NONE);
    switch (opt) {
    case ECreateIdx:
        /* Handled above. */
        break;

    case EAbsWaitIdx:
        NS_FALL_THROUGH; /* fall through */
    case EWaitIdx:
        if (objc != 4 && objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "condId mutexId ?timeout?");
            result = TCL_ERROR;

        } else {
            Ns_Mutex       *lockPtr;

            lockPtr = CreateSynchObject(itPtr,
                                        &servPtr->tcl.synch.mutexTable,
                                        &servPtr->tcl.synch.mutexId,
                                        (Ns_Callback *) Ns_MutexInit,
                                        mutexType,
                                        objv[3],
                                        TCL_INDEX_NONE);
            if (objc == 4) {
                timeout.sec = timeout.usec = 0;
            } else if (Ns_TclGetTimeFromObj(interp, objv[4], &timeout) != TCL_OK) {
                result = TCL_ERROR;
            }

            if (result == TCL_OK) {
                Ns_ReturnCode   status;

                /*
                 * Get timeout and wait.
                 */
                if (opt == EAbsWaitIdx) {
                    /*
                     * Absolute time wait: ns_cond abswait
                     */
                    status = Ns_CondTimedWait(condPtr, lockPtr, &timeout);
                } else {
                    /*
                     * Relative time wait: ns_cond wait
                     */
                    if (objc == 4 || (timeout.sec == 0 && timeout.usec == 0)) {
                        Ns_CondWait(condPtr, lockPtr);
                        status = NS_OK;
                    } else {
                        Ns_GetTime(&abstime);
                        Ns_IncrTime(&abstime, timeout.sec, timeout.usec);
                        status = Ns_CondTimedWait(condPtr, lockPtr, &abstime);
                    }
                }

                if (status == NS_OK) {
                    Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
                } else if (status == NS_TIMEOUT) {
                    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
                } else {
                    result = TCL_ERROR;
                }
            }
        }
        break;

    case EBroadcastIdx:
        Ns_CondBroadcast(condPtr);
        break;

    case ESetIdx:
        NS_FALL_THROUGH; /* fall through */
    case ESignalIdx:
        Ns_CondSignal(condPtr);
        break;

    case EDestroyIdx:
        /* No-op. */
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        break;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRWLockObjCmd --
 *
 *      Implements "ns_rwlock".
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
NsTclRWLockObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int             opt, result = TCL_OK;

    static const char *const opts[] = {
        "create", "destroy", "readlock", "readunlock", "readeval",
        "writelock", "writeunlock", "writeeval", "unlock", NULL
    };
    enum {
        RCreateIdx, RDestroyIdx, RReadLockIdx, RReadUnlockIdx, RReadEvalIdx,
        RWriteLockIdx, RWriteUnlockIdx, RWriteEvalIdx, RUnlockIdx
    };
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr   = clientData;
        NsServer       *servPtr = itPtr->servPtr;
        Ns_RWLock      *rwlockPtr = CreateSynchObject(itPtr,
                                                      &servPtr->tcl.synch.rwTable,
                                                      &servPtr->tcl.synch.rwId,
                                                      (Ns_Callback *) Ns_RWLockInit,
                                                      rwType,
                                                      (objc == 3) ? objv[2] : NULL,
                                                      TCL_INDEX_NONE);
        switch (opt) {
        case RCreateIdx:
            /* Handled above. */
            Ns_RWLockSetName2(rwlockPtr, "rw:ns_rwlock", servPtr->server);
            break;

        case RReadLockIdx:
            Ns_RWLockRdLock(rwlockPtr);
            break;

        case RWriteLockIdx:
            Ns_RWLockWrLock(rwlockPtr);
            break;

        case RReadUnlockIdx:
            NS_FALL_THROUGH; /* fall through */
        case RWriteUnlockIdx:
            NS_FALL_THROUGH; /* fall through */
        case RUnlockIdx:
            Ns_RWLockUnlock(rwlockPtr);
            break;

        case RReadEvalIdx:
            if (objc != 4) {
                Tcl_WrongNumArgs(interp, 3, objv, "script");
                result = TCL_ERROR;
            } else {
                Ns_RWLockRdLock(rwlockPtr);
                result = Tcl_EvalObjEx(interp, objv[3], 0);
                Ns_RWLockUnlock(rwlockPtr);
            }
            break;

        case RWriteEvalIdx:
            if (objc != 4) {
                Tcl_WrongNumArgs(interp, 3, objv, "script");
                result = TCL_ERROR;
            } else {
                Ns_RWLockWrLock(rwlockPtr);
                result = Tcl_EvalObjEx(interp, objv[3], 0);
                Ns_RWLockUnlock(rwlockPtr);
            }
            break;

        case RDestroyIdx:
            /* No-op. */
            break;

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }
    }
    return result;
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
 *      Initialize once the data structures needed for thread local storage.
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
    Ns_DString       ds, *dsPtr;
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
        Ns_DStringInit(&ds);
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
 *      Create and initialize a new synchronization object of the
 *      requested type (mutex, critsec, condition, ...), or return an
 *      existing one with the same name.
 *
 * Results:
 *      Pointer to the lock or cond etc. Tcl_Obj representing the
 *      lock is left in interp.
 *
 * Side effects:
 *      None.
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
            Ns_DString     ds;

            Ns_DStringInit(&ds);
            do {
                Ns_DStringSetLength(&ds, 0);
                Ns_DStringPrintf(&ds, "%s:tcl:%u", type, (*idPtr)++);
                hPtr = Tcl_CreateHashEntry(typeTable, ds.string, &isNew);
            } while (isNew == 0);

            objPtr = Tcl_NewStringObj(ds.string, ds.length);
            Tcl_SetObjResult(interp, objPtr);
            Ns_DStringFree(&ds);

        } else {
            hPtr = Tcl_CreateHashEntry(typeTable, Tcl_GetString(objPtr), &isNew);
            Tcl_SetObjResult(interp, objPtr);
        }

        if (isNew != 0) {
            addr = ns_calloc(1u, sizeof(void *));
            if (cnt != TCL_INDEX_NONE) {
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
        } else {
            addr = Tcl_GetHashValue(hPtr);
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
