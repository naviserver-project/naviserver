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
 * MutexCreateObjCmd, MutexDestroyObjCmd, MutexEvalObjCmd, MutexLockObjCmd,
 * MutexTrylockObjCmd, MutexUnlockObjCmd --
 *
 *      Implements subcommands of "ns_mutex", i.e.,
 *         "ns_mutex create"
 *         "ns_mutex destroy"
 *         "ns_mutex eval"
 *         "ns_mutex lock"
 *         "ns_mutex trylock"
 *         "ns_mutex unlock"
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
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

static int
MutexDestroyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_Mutex   *lockPtr;
    Ns_ObjvSpec args[] = {
        {"mutexid", ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    return DestroyHelper(args, interp, objc, objv);
}


static int
MutexEvalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Tcl_Obj    *scriptObj = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexid", ObjvMutexObj, &lockPtr, NULL},
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

static int
MutexLockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexid", ObjvMutexObj, &lockPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_MutexLock(lockPtr);
    }
    return result;
}

static int
MutexTrylockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexid", ObjvMutexObj, &lockPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_MutexTryLock(lockPtr)));
    }
    return result;
}

static int
MutexUnlockObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Mutex   *lockPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"mutexid", ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_MutexUnlock(lockPtr);
    }
    return result;
}

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
 * CondAbswaitObjCmd. CondBroadcastObjCmd. CondCreateObjCmd,
 * CondDestroyObjCmd, CondSignalObjCmd, CondWaitObjCmd --
 *
 *      Implements subcommands of "ns_cond", i.e.,
 *         "ns_cond abswait"
 *         "ns_cond broadcast"
 *         "ns_cond create"
 *         "ns_cond destroy"
 *         "ns_cond set"
 *         "ns_cond signal"
 *         "ns_cond wait"
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */
static int
CondBroadcastObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int        result = TCL_OK;
    Ns_Cond   *condPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"condid", ObjvCondObj, &condPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_CondBroadcast(condPtr);
    }
    return result;
}

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

static int
CondDestroyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_Cond    *lockPtr;
    Ns_ObjvSpec args[] = {
        {"condid", ObjvCondObj, &lockPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    return DestroyHelper(args, interp, objc, objv);
}


static int
CondSignalObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cond    *condPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"condid", ObjvCondObj, &condPtr, INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_CondSignal(condPtr);
    }
    return result;
}

static int
CondAbswaitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cond    *condPtr = NULL;
    Ns_Mutex   *lockPtr = NULL;
    Ns_Time     timeout = {0, 0};
    long        epoch = -1;
    Ns_ObjvSpec args[] = {
        {"condid",   ObjvCondObj,  &condPtr, INT2PTR(NS_TRUE)},
        {"mutexid",  ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
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

static int
CondWaitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Cond    *condPtr = NULL;
    Ns_Mutex   *lockPtr = NULL;
    Ns_Time    *timeoutPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"condid",   ObjvCondObj,  &condPtr, INT2PTR(NS_TRUE)},
        {"mutexid",  ObjvMutexObj, &lockPtr, INT2PTR(NS_TRUE)},
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
    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * RWLockCreateObjCmd, RWLockDestroyObjCmd,RWLockReadevalObjCmd,
 * RWLockReadlockObjCmd, RWLockUnlockObjCmd, RWLockWriteevalkObjCmd,
 * RWLockWriteunlockObjCmd --
 *
 *      Implements subcommands of "ns_rwlock", i.e.,
 *         "ns_rwlock create"
 *         "ns_rwlock destroy"
 *         "ns_rwlock readeval"
 *         "ns_rwlock readlock"
 *         "ns_rwlock readunlock"
 *         "ns_rwlock unlock"
 *         "ns_rwlock writeeval"
 *         "ns_rwlock writelock"
 *         "ns_rwlock writeunlock"
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
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
 * CsCreateObjCmd, CsDestroyObjCmd, CsEvalObjCmd, CsLockObjCmd,
 * CsTrylockObjCmd, CsUnlockObjCmd --
 *
 *      Implements subcommands of "ns_critsec", i.e.,
 *         "ns_critsec create"
 *         "ns_critsec destroy"
 *         "ns_critsec enter"
 *         "ns_critsec eval"
 *         "ns_critsec leave"
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
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
 * SemaCreateObjCmd, SemaDestroyObjCmd, SemaReleaseObjCmd, SemaWaitObjCmd --
 *
 *      Implements subcommands of "ns_sema", i.e.,
 *         "ns_sema create"
 *         "ns_sema destroy"
 *         "ns_sema release"
 *         "ns_sema wait"
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
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
