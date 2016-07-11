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

static void CreateTclThread(const NsInterp *itPtr, const char *script, bool detached,
                            const char *threadName, Ns_Thread *thrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void *CreateSynchObject(const NsInterp *itPtr,
                               Tcl_HashTable *typeTable, unsigned int *idPtr,
                               Ns_Callback *initProc, const char *type,
                               Tcl_Obj *objPtr, int cnt)
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
 *      NS_OK.  String result of script available via thrPtr.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclThread(Tcl_Interp *interp, const char *script, Ns_Thread *thrPtr)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(script != NULL);

    CreateTclThread(NsGetInterpData(interp), script, (thrPtr == NULL ? NS_TRUE : NS_FALSE),
                    NULL, thrPtr);
    return NS_OK;
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

int
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
 *      Implements ns_thread to get data on the current thread and
 *      create and wait on new Tcl-script based threads.  New threads will
 *      be created in the virtual-server context of the current interp,
 *      if any.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May create a new thread or wait for an existing thread to exit.
 *
 *----------------------------------------------------------------------
 */

int
NsTclThreadObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp  *itPtr = arg;
    void      *tidArg;
    Ns_Thread  tid;
    void      *result;
    int        opt;

    static const char *const opts[] = {
        "begin", "begindetached", "create", "wait", "join",
        "name", "get", "getid", "handle", "id", "yield", "stackinfo", NULL
    };
    enum {
        TBeginIdx, TBeginDetachedIdx, TCreateIdx, TWaitIdx, TJoinIdx,
        TNameIdx, TGetIdx, TGetIdIdx, THandleIdx, TIdIdx, TYieldIdx, TStackinfoIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case TCreateIdx:
        Ns_LogDeprecated(objv, 2, "ns_thread begin ...", NULL);
        /* FALLTHROUGH */
    case TBeginIdx:
    case TBeginDetachedIdx:
        {
            const char *threadName = NULL, *script;
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
                return TCL_ERROR;
            }

            if (opt == TBeginDetachedIdx) {
                CreateTclThread(itPtr, script, NS_TRUE, threadName, NULL);
            } else {
                CreateTclThread(itPtr, script, NS_FALSE, threadName, &tid);
                Ns_TclSetAddrObj(Tcl_GetObjResult(interp), threadType, tid);
            }
            break;
        }

    case TJoinIdx:
        Ns_LogDeprecated(objv, 2, "ns_thread wait ...", NULL);
        /* FALLTHROUGH */
    case TWaitIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "tid");
            return TCL_ERROR;
        }
        if (Ns_TclGetAddrFromObj(interp, objv[2], threadType, &tidArg) != TCL_OK) {
            return TCL_ERROR;
        }
        tid = tidArg;
        Ns_ThreadJoin(&tid, &result);
        Tcl_SetResult(interp, result, (Tcl_FreeProc *) ns_free);
        break;


    case TGetIdx:
        Ns_LogDeprecated(objv, 2, "ns_thread handle ...", NULL);
        /* FALLTHROUGH */
    case THandleIdx:
        Ns_ThreadSelf(&tid);
        Ns_TclSetAddrObj(Tcl_GetObjResult(interp), threadType, tid);
        break;


    case TGetIdIdx:
        Ns_LogDeprecated(objv, 2, "ns_thread id ...", NULL);
        /* FALLTHROUGH */
    case TIdIdx:
        Ns_TclPrintfResult(interp, "%" PRIxPTR, Ns_ThreadId());
        break;

    case TNameIdx:
        if (objc > 2) {
            Ns_ThreadSetName(Tcl_GetString(objv[2]));
        }
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_ThreadGetName(), -1));
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

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclMutexObjCmd --
 *
 *      Implements ns_mutex.
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
NsTclMutexObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp *itPtr   = arg;
    NsServer *servPtr = itPtr->servPtr;
    Ns_Mutex *lockPtr;
    int       opt, status = TCL_OK;

    static const char *const opts[] = {
        "create", "destroy", "eval", "lock", "trylock", "unlock", NULL
    };
    enum {
        MCreateIdx, MDestroyIdx, MEvalIdx, MLockIdx, MTryLockIdx, MUnlockIdx
    };
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    lockPtr = CreateSynchObject(itPtr,
                                &servPtr->tcl.synch.mutexTable,
                                &servPtr->tcl.synch.mutexId,
                                (Ns_Callback *) Ns_MutexInit,
                                mutexType,
                                (objc >= 3) ? objv[2] : NULL, -1);
    switch (opt) {
    case MCreateIdx:
        if (objc > 2) {
            Ns_MutexSetName(lockPtr, Tcl_GetString(objv[2]));
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
            return TCL_ERROR;
        }
        Ns_MutexLock(lockPtr);
        status = Tcl_EvalObjEx(interp, objv[3], 0);
        Ns_MutexUnlock(lockPtr);
        break;

    case MDestroyIdx:
        /* No-op. */
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        break;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclCritSecObjCmd --
 *
 *      Implements ns_critsec.
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
NsTclCritSecObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp *itPtr   = arg;
    NsServer *servPtr = itPtr->servPtr;
    Ns_Cs    *csPtr;
    int       opt, status = TCL_OK;

    static const char *const opts[] = {
        "create", "destroy", "enter", "eval", "leave", NULL
    };
    enum {
        CCreateIdx, CDestroyIdx, CEnterIdx, CEvalIdx, CLeaveIdx
    };
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    csPtr = CreateSynchObject(itPtr,
                              &servPtr->tcl.synch.csTable,
                              &servPtr->tcl.synch.csId,
                              (Ns_Callback *) Ns_CsInit,
                              csType,
                              (objc == 3) ? objv[2] : NULL, -1);
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
            return TCL_ERROR;
        }
        Ns_CsEnter(csPtr);
        status = Tcl_EvalObjEx(interp, objv[3], 0);
        Ns_CsLeave(csPtr);
        break;

    case CDestroyIdx:
        /* No-op. */
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        break;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSemaObjCmd --
 *
 *      Implements ns_sema.
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
NsTclSemaObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp *itPtr   = arg;
    NsServer *servPtr = itPtr->servPtr;
    Ns_Sema  *semaPtr;
    int       opt, cnt;

    static const char *const opts[] = {
        "create", "destroy", "release", "wait", NULL
    };
    enum {
        SCreateIdx, SDestroyIdx, SReleaseIdx, SWaitIdx
    };
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "cmd ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    if (opt == SCreateIdx && objc == 3) {
        if (Tcl_GetIntFromObj(interp, objv[2], &cnt) != TCL_OK) {
            return TCL_ERROR;
        }
    } else {
        cnt = 0;
    }

    semaPtr = CreateSynchObject(itPtr,
                                &servPtr->tcl.synch.semaTable,
                                &servPtr->tcl.synch.semaId,
                                NULL,
                                semaType,
                                (objc == 3) ? objv[2] : NULL, cnt);
    switch (opt) {
    case SCreateIdx:
        /* Handled above. */
        break;

    case SReleaseIdx:
        if (objc < 4) {
            cnt = 1;
        } else if (Tcl_GetIntFromObj(interp, objv[3], &cnt) != TCL_OK) {
            return TCL_ERROR;
        }
        Ns_SemaPost(semaPtr, cnt);
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

    return TCL_OK;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclCondObjCmd --
 *
 *      Implements ns_cond.
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
NsTclCondObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp     *itPtr   = arg;
    NsServer     *servPtr = itPtr->servPtr;
    Ns_Cond      *condPtr;
    Ns_Mutex     *lockPtr;
    Ns_Time       timeout, abstime;
    int           opt;
    Ns_ReturnCode result;

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
                                (objc >= 3) ? objv[2] : NULL, -1);
    switch (opt) {
    case ECreateIdx:
        /* Handled above. */
        break;

    case EAbsWaitIdx:
    case EWaitIdx:
        if (objc != 4 && objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "condId mutexId ?timeout?");
            return TCL_ERROR;
        }
        lockPtr = CreateSynchObject(itPtr,
                                    &servPtr->tcl.synch.mutexTable,
                                    &servPtr->tcl.synch.mutexId,
                                    (Ns_Callback *) Ns_MutexInit,
                                    mutexType,
                                    objv[3], -1);
        if (objc == 4) {
            timeout.sec = timeout.usec = 0;
        } else if (Ns_TclGetTimeFromObj(interp, objv[4], &timeout) != TCL_OK) {
            return TCL_ERROR;
        }

        if (opt == EAbsWaitIdx) {
            result = Ns_CondTimedWait(condPtr, lockPtr, &timeout);
        } else {
            if (objc == 4 || (timeout.sec == 0 && timeout.usec == 0)) {
                Ns_CondWait(condPtr, lockPtr);
                result = NS_OK;
            } else {
                Ns_GetTime(&abstime);
                Ns_IncrTime(&abstime, timeout.sec, timeout.usec);
                result = Ns_CondTimedWait(condPtr, lockPtr, &abstime);
            }
        }

        if (result == NS_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
        } else if (result == NS_TIMEOUT) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
        } else {
            return TCL_ERROR;
        }
        break;

    case EBroadcastIdx:
        Ns_CondBroadcast(condPtr);
        break;

    case ESetIdx:
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

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRWLockObjCmd --
 *
 *      Implements ns_rwlock.
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
NsTclRWLockObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp  *itPtr   = arg;
    NsServer  *servPtr = itPtr->servPtr;
    Ns_RWLock *rwlockPtr;
    int        opt, status = TCL_OK;

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
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "cmd", 1, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    rwlockPtr = CreateSynchObject(itPtr,
                                  &servPtr->tcl.synch.rwTable,
                                  &servPtr->tcl.synch.rwId,
                                  (Ns_Callback *) Ns_RWLockInit,
                                  rwType,
                                  (objc == 3) ? objv[2] : NULL, -1);
    switch (opt) {
    case RCreateIdx:
        /* Handled above. */
        break;

    case RReadLockIdx:
        Ns_RWLockRdLock(rwlockPtr);
        break;

    case RWriteLockIdx:
        Ns_RWLockWrLock(rwlockPtr);
        break;

    case RReadUnlockIdx:
    case RWriteUnlockIdx:
    case RUnlockIdx:
        Ns_RWLockUnlock(rwlockPtr);
        break;

    case RReadEvalIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 3, objv, "script");
            return TCL_ERROR;
        }
        Ns_RWLockRdLock(rwlockPtr);
        status = Tcl_EvalObjEx(interp, objv[3], 0);
        Ns_RWLockUnlock(rwlockPtr);
        break;

    case RWriteEvalIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 3, objv, "script");
            return TCL_ERROR;
        }
        Ns_RWLockWrLock(rwlockPtr);
        status = Tcl_EvalObjEx(interp, objv[3], 0);
        Ns_RWLockUnlock(rwlockPtr);
        break;

    case RDestroyIdx:
        /* No-op. */
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        break;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ThreadArgFree --
 *
 *      Free the argument structure of a NsTclThread.
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
    TclThreadArg *argPtr = (TclThreadArg *)arg;
    
    NS_NONNULL_ASSERT(arg != NULL);
    
    if (argPtr->threadName != NULL) {
        ns_free((char *)argPtr->threadName);
    }
    ns_free(argPtr);
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
    static bool      initialized = NS_FALSE;

    NS_NONNULL_ASSERT(arg != NULL);

    /*
     * The argument structure is a TclThreadArg, which has to be freed when
     * the thread shuts down.  The argument is used e.g. by the arg proc in
     * Ns_ThreadList(), which might be called during thread shutdown. To
     * ensure consistent cleanup in all success and error cases, we use a
     * thread local variable with ThreadArgFree() as cleanup proc.
     *
     * On the first call, allocate the thread local storage slot. This
     * initialization might be moved into some tclThreadInit() code, which
     * does not exist.
     */
    if (!initialized) {
        Ns_TlsAlloc(&argtls, ThreadArgFree);
        initialized = NS_TRUE;
    }

    Ns_TlsSet(&argtls, argPtr);
    
    if (argPtr->threadName != NULL) {
        static uintptr_t id = 0u;
        Ns_ThreadSetName("-tcl-%s:%" PRIuPTR "-", argPtr->threadName, id++);
    }

    detached = argPtr->detached;
    if (detached) {
        dsPtr = NULL;
    } else {
        Ns_DStringInit(&ds);
        dsPtr = &ds;
    }
    
    /*
     * Need to ensure that the server has completed it's initialization
     * prior to initiating TclEval.
     */
    (void) Ns_WaitForStartup();

    (void) Ns_TclEval(dsPtr, argPtr->server, argPtr->script);
    
    /*
     * No matter if the Tcl eval was successul or not, return in the
     * non-detached case the dstring result, since some other thread might be
     * waiting for a result. In the detached case, there is no dstring
     * content.
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
 *      None.
 *
 * Side effects:
 *      Depends on Tcl script.
 *
 *----------------------------------------------------------------------
 */

static void
CreateTclThread(const NsInterp *itPtr, const char *script, bool detached,
                const char *threadName, Ns_Thread *thrPtr)
{
    TclThreadArg *argPtr;
    size_t scriptLength;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(script != NULL);

    scriptLength = strlen(script);
    argPtr = ns_malloc(sizeof(TclThreadArg) + scriptLength);
    argPtr->detached = detached;
    argPtr->threadName = threadName;
    memcpy(argPtr->script, script, scriptLength + 1u);
    
    if (itPtr->servPtr != NULL) {
        argPtr->server = itPtr->servPtr->server;
    } else {
        argPtr->server = NULL;
    }
    Ns_ThreadCreate(NsTclThread, argPtr, 0, thrPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * CreateSynchObject --
 *
 *      Create and initialize a new synchronization object of the
 *      requested type, or return an existing one with the same name.
 *
 * Results:
 *      Pointer to the lock or cond etc. Tcl object representing the lock
 *      is left in interp.
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
                  Tcl_Obj *objPtr, int cnt)
{
    NsServer      *servPtr;
    Tcl_Interp    *interp;
    Tcl_HashEntry *hPtr;
    void          *addr;
    int            isNew = 0;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(typeTable != NULL);
    NS_NONNULL_ASSERT(idPtr != NULL);
    NS_NONNULL_ASSERT(type != NULL);

    interp  = itPtr->interp;

    if (objPtr != NULL
	&& Ns_TclGetOpaqueFromObj(objPtr, type, &addr) == TCL_OK) {
        Tcl_SetObjResult(interp, objPtr);
        return addr;
    }

    servPtr = itPtr->servPtr;
    Ns_MutexLock(&servPtr->tcl.synch.lock);

    if (objPtr == NULL) {
        Ns_DString     ds;
        
        Ns_DStringInit(&ds);
        do {
            Ns_DStringTrunc(&ds, 0);
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
        if (cnt > -1) {
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

    return addr;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
