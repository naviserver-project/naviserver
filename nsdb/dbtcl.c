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

#define NS_DBTCL_C 1
/*
 * dbtcl.c --
 *
 *      Tcl database access routines.
 */

#include "db.h"

/*
 * The following structure maintains per-interp data.
 */

typedef struct InterpData {
    const char *server;
    Tcl_HashTable dbs;
} InterpData;

/*
 * The data for this severity resides in this file (and is protected
 * in nsdb.h via NS_DBTCL_C).
 */
#ifdef __MINGW32__
NS_EXPORT Ns_LogSeverity Ns_LogSqlDebug;
#else
Ns_LogSeverity Ns_LogSqlDebug;
#endif

/*
 * Local functions defined in this file
 */

static int DbFail(Tcl_Interp *interp, Ns_DbHandle *handle, const char *cmd)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void EnterDbHandle(InterpData *idataPtr, Tcl_Interp *interp, Ns_DbHandle *handle, Tcl_Obj *listObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static int DbGetHandle(InterpData *idataPtr, Tcl_Interp *interp, const char *handleId,
                       Ns_DbHandle **handle, Tcl_HashEntry **hPtrPtr);

static Ns_ReturnCode QuoteSqlValue(Tcl_DString *dsPtr, Tcl_Obj *valueObj, int valueType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#if !defined(NS_TCL_PRE85)
static Ns_ReturnCode CurrentHandles( Tcl_Interp *interp, Tcl_HashTable *tablePtr, Tcl_Obj *dictObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
#endif

static Tcl_InterpDeleteProc FreeData;
static TCL_OBJCMDPROC_T
    DbConfigPathObjCmd,
    DbErrorCodeObjCmd,
    DbErrorMsgObjCmd,
    DbObjCmd,
    PoolDescriptionObjCmd,
    QuoteListObjCmd,
    QuoteListToListObjCmd,
    QuoteValueObjCmd;

static int ErrorObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, char cmd);

/*
 * Importing from the DLL requires NS_IMPORT under windows. NS_IMPORT
 * is a noop under Unix.
 */
extern NS_IMPORT const Tcl_ObjType *NS_intTypePtr;

/*
 * Local variables defined in this file.
 */

static const char *const datakey = "nsdb:data";

static const Ns_ObjvTable valueTypes[] = {
    {"decimal",  UCHAR('d')},
    {"double",   UCHAR('D')},
    {"integer",  UCHAR('i')},
    {"int",      UCHAR('I')},
    {"real",     UCHAR('r')},
    {"smallint", UCHAR('s')},
    {"bigint",   UCHAR('b')},
    {"bit",      UCHAR('B')},
    {"float",    UCHAR('f')},
    {"numeric",  UCHAR('n')},
    {"tinyint",  UCHAR('t')},
    {"text",     UCHAR('q')},
    {NULL,    0u}
};


/*
 *----------------------------------------------------------------------
 * Ns_TclDbGetHandle --
 *
 *      Get database handle from its handle id.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclDbGetHandle(Tcl_Interp *interp, const char *handleId, Ns_DbHandle **handlePtr)
{
    int         result;
    InterpData *idataPtr;

    idataPtr = Tcl_GetAssocData(interp, datakey, NULL);
    if (idataPtr == NULL) {
        result = TCL_ERROR;
    } else {
        result = DbGetHandle(idataPtr, interp, handleId, handlePtr, NULL);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsDbAddCmds --
 *
 *      Add the nsdb commands.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsDbAddCmds(Tcl_Interp *interp, const void *arg)
{
    const char *server = arg;
    InterpData *idataPtr;

    /*
     * Initialize the per-interp data.
     */

    idataPtr = ns_malloc(sizeof(InterpData));
    idataPtr->server = server;
    Tcl_InitHashTable(&idataPtr->dbs, TCL_STRING_KEYS);
    Tcl_SetAssocData(interp, datakey, FreeData, idataPtr);

    if (NS_intTypePtr == NULL) {
        /*
         * Not sure, why we see a problem with tcl9 here, maybe the
         * initialization order has to be checked.
         */
        Tcl_Obj *tmpObj = Tcl_NewIntObj(0);

        NS_intTypePtr = tmpObj->typePtr;
        Ns_Log(Warning, "NsDbAddCmds: obtain Tcl int type from temporary object");
        Tcl_DecrRefCount(tmpObj);

        if (NS_intTypePtr == NULL) {
            // Tcl_Panic("NsDbAddCmds: no int type");
            Ns_Log(Warning, "NsDbAddCmds: cannot determine Tcl int type");
        }
    }

    (void)TCL_CREATEOBJCOMMAND(interp, "ns_db", DbObjCmd, idataPtr, NULL);
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_dbconfigpath", DbConfigPathObjCmd, idataPtr, NULL);
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_dberrorcode", DbErrorCodeObjCmd, idataPtr, NULL);
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_dberrormsg", DbErrorMsgObjCmd, idataPtr, NULL);
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_dbquotevalue", QuoteValueObjCmd, idataPtr, NULL);
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_dbquotelist", QuoteListObjCmd, idataPtr, NULL);
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_dbpooldescription", PoolDescriptionObjCmd, idataPtr, NULL);
#ifdef NS_WITH_DEPRECATED
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_pooldescription", PoolDescriptionObjCmd, idataPtr, NULL);
#endif
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_quotelisttolist", QuoteListToListObjCmd, idataPtr, NULL);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 * NsDbReleaseHandles --
 *
 *      Release any database handles still held when an interp is
 *      deallocated.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsDbReleaseHandles(Tcl_Interp *interp, const void *UNUSED(arg))
{
    InterpData     *idataPtr;

    idataPtr = Tcl_GetAssocData(interp, datakey, NULL);
    if (idataPtr != NULL) {
        Tcl_HashSearch       search;
        const Tcl_HashEntry *hPtr = Tcl_FirstHashEntry(&idataPtr->dbs, &search);

        while (hPtr != NULL) {
            Ns_DbHandle *handlePtr = Tcl_GetHashValue(hPtr);

            Ns_DbPoolPutHandle(handlePtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&idataPtr->dbs);
        Tcl_InitHashTable(&idataPtr->dbs, TCL_STRING_KEYS);
    }

    return TCL_OK;
}

#if !defined(NS_TCL_PRE85)

/*
 *----------------------------------------------------------------------
 *
 * CurrentHandles --
 *
 *      Return a Tcl dict with information about the current allocated
 *      handles in the current interp/thread.
 *
 * Results:
 *      NS_OK if everything went well, NS_ERROR otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CurrentHandles( Tcl_Interp *interp, Tcl_HashTable *tablePtr, Tcl_Obj *dictObj)
{
    Tcl_HashSearch       search;
    const Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(tablePtr != NULL);
    NS_NONNULL_ASSERT(dictObj != NULL);

    hPtr = Tcl_FirstHashEntry(tablePtr, &search);
    while (hPtr != NULL) {
        Ns_DbHandle *handlePtr = Tcl_GetHashValue(hPtr);
        Tcl_Obj     *keyv[2];

        keyv[0] = Tcl_NewStringObj(handlePtr->poolname, TCL_INDEX_NONE);
        keyv[1] = Tcl_NewStringObj(Ns_TclGetHashKeyString(tablePtr, hPtr), TCL_INDEX_NONE);
        Tcl_DictObjPutKeyList(interp, dictObj, 2, keyv, Tcl_NewIntObj(NsDbGetActive(handlePtr) ? 1 : 0));
        hPtr = Tcl_NextHashEntry(&search);
    }

    return NS_OK;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * DbObjCmd --
 *
 *      Implements "ns_db".
 *
 * Results:
 *      Return TCL_OK upon success and TCL_ERROR otherwise.
 *
 * Side effects:
 *      Depends on the command.
 *
 *----------------------------------------------------------------------
 */

static int
DbObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    InterpData     *idataPtr = clientData;
    char            tmpbuf[32] = "";
    const char     *pool = NULL;
    int             cmd, nrows;
    Ns_DbHandle    *handlePtr = NULL;
    Ns_Set         *rowPtr;
    Tcl_HashEntry  *hPtr;
    int             result = TCL_OK;

    enum {
        ZERO_OR_ONE_ROW,
        ONE_ROW,
        BINDROW,
        BOUNCEPOOL,
        CANCEL,
        CONNECTED,
#if !defined(NS_TCL_PRE85)
        CURRENTHANDLES,
#endif
        DATASOURCE,
        DBTYPE,
        DISCONNECT,
        DML,
        DRIVER,
        EXCEPTION,
        EXEC,
        FLUSH,
        GETHANDLE,
        GETROW,
        INFO,
        INTERPRETSQLFILE,
        LOGMINDURATION,
        PASSWORD,
        POOLNAME,
        POOLS,
        RELEASEHANDLE,
        RESETHANDLE,
        ROWCOUNT,
        SELECT,
        SESSIONID,
        SETEXCEPTION,
        SP_EXEC,
        SP_GETPARAMS,
        SP_RETURNCODE,
        SP_SETPARAM,
        SP_START,
        STATS,
        USER
#ifdef NS_WITH_DEPRECATED
        ,VERBOSE
#endif
    };

    static const char *const subcmd[] = {
        "0or1row",
        "1row",
        "bindrow",
        "bouncepool",
        "cancel",
        "connected",
#if !defined(NS_TCL_PRE85)
        "currenthandles",
#endif
        "datasource",
        "dbtype",
        "disconnect",
        "dml",
        "driver",
        "exception",
        "exec",
        "flush",
        "gethandle",
        "getrow",
        "info",
        "interpretsqlfile",
        "logminduration",
        "password",
        "poolname",
        "pools",
        "releasehandle",
        "resethandle",
        "rowcount",
        "select",
        "session_id",
        "setexception",
        "sp_exec",
        "sp_getparams",
        "sp_returncode",
        "sp_setparam",
        "sp_start",
        "stats",
        "user",
#ifdef NS_WITH_DEPRECATED
        "verbose",
#endif
        NULL
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/subcommand/ ?/arg .../?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], subcmd, "subcommand", 0, &cmd) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (cmd) {
    case POOLS:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            result = TCL_ERROR;
        } else {
            pool = Ns_DbPoolList(idataPtr->server);
            if (pool != NULL) {
                Tcl_Obj  *listObj = Tcl_NewListObj(0, NULL);

                while (*pool != '\0') {
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(pool, TCL_INDEX_NONE));
                    pool = pool + strlen(pool) + 1;
                }
                Tcl_SetObjResult(interp, listObj);
            }
        }
        break;

    case BOUNCEPOOL:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "/poolname/");
            result = TCL_ERROR;

        } else if (Ns_DbBouncePool(Tcl_GetString(objv[2])) == NS_ERROR) {
            Ns_TclPrintfResult(interp, "could not bounce: %s", Tcl_GetString(objv[2]));
            result = TCL_ERROR;
        }
        break;

    case GETHANDLE: {
        int               nhandles = 1;
        Ns_Time          *timeoutPtr = NULL;
        Ns_DbHandle     **handlesPtrPtr;
        Ns_ReturnCode     status;
        char             *poolString = NULL;
        Ns_ObjvValueRange handlesRange = {1, INT_MAX};
        Ns_ObjvSpec    opts[] = {
            {"-timeout", Ns_ObjvTime,  &timeoutPtr, NULL},
            {"--",       Ns_ObjvBreak,  NULL,       NULL},
            {NULL, NULL, NULL, NULL}
        };
        Ns_ObjvSpec    args[] = {
            {"?poolname",   Ns_ObjvString, &poolString, NULL},
            {"?nhandles",   Ns_ObjvInt,    &nhandles,  &handlesRange},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }

        /*
         * Determine the pool and requested number of handles
         * from the remaining args.
         */

        if (poolString == NULL) {
            pool = Ns_DbPoolDefault(idataPtr->server);
            if (pool == NULL) {
                Ns_TclPrintfResult(interp, "no defaultpool configured");
                return TCL_ERROR;
            }
        } else {
            pool = (const char *)poolString;
        }

        if (Ns_DbPoolAllowable(idataPtr->server, pool) == NS_FALSE) {
            Ns_TclPrintfResult(interp, "no access to pool: \"%s\"", pool);
            return TCL_ERROR;
        }

        /*
         * When timeout is specified as 0 (or 0:0) then treat is as
         * non-specified (blocking).
         */
        if (timeoutPtr != NULL && timeoutPtr->sec == 0 && timeoutPtr->usec == 0) {
            timeoutPtr = NULL;
        }

        /*
         * Allocate handles and enter them into Tcl.
         */

        if (nhandles == 1) {
            handlesPtrPtr = &handlePtr;
        } else {
            handlesPtrPtr = ns_malloc((size_t)nhandles * sizeof(Ns_DbHandle *));
        }
        status = Ns_DbPoolTimedGetMultipleHandles(handlesPtrPtr, pool,
                                                  nhandles, timeoutPtr);
        if (status == NS_OK) {
            int      i;
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            for (i = 0; i < nhandles; ++i) {
                EnterDbHandle(idataPtr, interp, *(handlesPtrPtr + i), listObj);
            }
            Tcl_SetObjResult(interp, listObj);
        }
        if (handlesPtrPtr != &handlePtr) {
            ns_free(handlesPtrPtr);
        }
        if (status != NS_TIMEOUT && status != NS_OK) {
          Ns_TclPrintfResult(interp,
                             "could not allocate %d handle%s from pool \"%s\"",
                             nhandles,
                             nhandles > 1 ? "s" : NS_EMPTY_STRING,
                             pool);
          result = TCL_ERROR;
        }
        break;
    }

    case CURRENTHANDLES: {
        if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
            result = TCL_ERROR;

        } else {
            Ns_ReturnCode  status;
            Tcl_Obj       *dictObj = Tcl_NewDictObj();

            assert(idataPtr != NULL);

            status = CurrentHandles(interp, &idataPtr->dbs, dictObj);
            if (status != NS_OK) {
                Tcl_DecrRefCount(dictObj);
                result = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, dictObj);
            }
        }
        break;
    }


    case LOGMINDURATION: {
        Ns_Time     *minDurationPtr = NULL;
        char        *poolString = NULL;
        Ns_ObjvSpec args[] = {
            {"?poolname",    Ns_ObjvString, &poolString, NULL},
            {"?minduration", Ns_ObjvTime,   &minDurationPtr,  NULL},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
            result = TCL_ERROR;

        } else if (poolString == NULL) {
            /*
             * No argument, list min duration for every pool.
             */
            Tcl_SetObjResult(interp, Ns_DbListMinDurations(interp, idataPtr->server));

        } else if (minDurationPtr == NULL) {
            pool = (const char *)poolString;
            /*
             * In this case, minduration was not given, return the
             * actual minduration of this pool.
             */

            if (Ns_DbGetMinDuration(interp, pool, &minDurationPtr) != TCL_OK) {
                result = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, Ns_TclNewTimeObj(minDurationPtr));
            }

        } else {
            pool = (const char *)poolString;
            /*
             * Set the minduration the specified value.
             */
            if (Ns_DbSetMinDuration(interp, pool, minDurationPtr) != TCL_OK) {
                result = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, Ns_TclNewTimeObj(minDurationPtr));
            }
        }

        break;
    }

    case EXCEPTION:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "/handle/");
            result = TCL_ERROR;

        } else if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, NULL) != TCL_OK) {
            result = TCL_ERROR;

        } else {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(handlePtr->cExceptionCode, TCL_INDEX_NONE));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(handlePtr->dsExceptionMsg.string, handlePtr->dsExceptionMsg.length));
            Tcl_SetObjResult(interp, listObj);
        }
        break;

    case STATS:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            result = TCL_ERROR;
        } else if  (Ns_DbPoolStats(interp) != TCL_OK) {
            result = TCL_ERROR;
        }
        break;

    case BINDROW:        NS_FALL_THROUGH; /* fall through */
    case CANCEL:         NS_FALL_THROUGH; /* fall through */
    case CONNECTED:      NS_FALL_THROUGH; /* fall through */
    case DATASOURCE:     NS_FALL_THROUGH; /* fall through */
    case DBTYPE:         NS_FALL_THROUGH; /* fall through */
    case DISCONNECT:     NS_FALL_THROUGH; /* fall through */
    case DRIVER:         NS_FALL_THROUGH; /* fall through */
    case FLUSH:          NS_FALL_THROUGH; /* fall through */
    case INFO:           NS_FALL_THROUGH; /* fall through */
    case PASSWORD:       NS_FALL_THROUGH; /* fall through */
    case POOLNAME:       NS_FALL_THROUGH; /* fall through */
    case RELEASEHANDLE:  NS_FALL_THROUGH; /* fall through */
    case RESETHANDLE:    NS_FALL_THROUGH; /* fall through */
    case ROWCOUNT:       NS_FALL_THROUGH; /* fall through */
    case SESSIONID:      NS_FALL_THROUGH; /* fall through */
    case SP_EXEC:        NS_FALL_THROUGH; /* fall through */
    case SP_GETPARAMS:   NS_FALL_THROUGH; /* fall through */
    case SP_RETURNCODE:  NS_FALL_THROUGH; /* fall through */
    case USER:

        if (objc < 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "/handle/");
            return TCL_ERROR;
        }
        if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, &hPtr) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_DStringFree(&handlePtr->dsExceptionMsg);
        handlePtr->cExceptionCode[0] = '\0';

        /*
         * The following commands require just the handle.
         */

        switch (cmd) {
        case POOLNAME:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->poolname, TCL_INDEX_NONE));
            break;

        case PASSWORD:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->password, TCL_INDEX_NONE));
            break;

        case USER:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->user, TCL_INDEX_NONE));
            break;

        case DATASOURCE:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->datasource, TCL_INDEX_NONE));
            break;

        case DISCONNECT:
            NsDbDisconnect(handlePtr);
            break;

        case DBTYPE:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DbDriverDbType(handlePtr), TCL_INDEX_NONE));
            break;

        case DRIVER:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DbDriverName(handlePtr), TCL_INDEX_NONE));
            break;

        case CANCEL:
            if (Ns_DbCancel(handlePtr) != NS_OK) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case BINDROW:
            rowPtr = Ns_DbBindRow(handlePtr);
            if (rowPtr == NULL) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

            } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_STATIC) != TCL_OK)) {
                result = TCL_ERROR;
            }
            break;

        case ROWCOUNT:
            Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_DbGetRowCount(handlePtr)));
            break;

        case FLUSH:
            if (Ns_DbFlush(handlePtr) != NS_OK) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case RELEASEHANDLE:
            Tcl_DeleteHashEntry(hPtr);
            Ns_DbPoolPutHandle(handlePtr);
            break;

        case RESETHANDLE:
            if (Ns_DbResetHandle(handlePtr) != NS_OK) {
              result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            } else {
                Tcl_SetObjResult(interp, Tcl_NewIntObj(NS_OK));
            }
            break;

        case CONNECTED:
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(handlePtr->connected));
            break;

        case INFO:
            {
                Tcl_Obj *dictObj = Ns_DbDriverVersionInfo(handlePtr);

                if (dictObj == NULL) {
                    dictObj = Tcl_NewDictObj();
                }

                Tcl_DictObjPut(NULL, dictObj,
                               Tcl_NewStringObj("type", 4),
                               Tcl_NewStringObj(Ns_DbDriverDbType(handlePtr), TCL_INDEX_NONE));
                Tcl_DictObjPut(NULL, dictObj,
                               Tcl_NewStringObj("pool", 4),
                               Tcl_NewStringObj(handlePtr->poolname, TCL_INDEX_NONE));

                Tcl_SetObjResult(interp, dictObj);
            }
            break;

        case SESSIONID:
            {
                char idstr[TCL_INTEGER_SPACE + 4];
                TCL_SIZE_T length;

                memcpy(idstr, "sid", 3u);
                length = (TCL_SIZE_T)ns_uint64toa(&idstr[3], (uint64_t)NsDbGetSessionId(handlePtr));
                Tcl_SetObjResult(interp, Tcl_NewStringObj(idstr, length + 3));
            }
            break;

        case SP_EXEC:
            switch (Ns_DbSpExec(handlePtr)) {
            case NS_DML:
                Tcl_SetObjResult(interp, Tcl_NewStringObj("NS_DML", 6));
                break;
            case NS_ROWS:
                Tcl_SetObjResult(interp, Tcl_NewStringObj("NS_ROWS", 7));
                break;
            default:
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case SP_GETPARAMS:
            rowPtr = Ns_DbSpGetParams(handlePtr);
            if (rowPtr == NULL) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

            } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
                result = TCL_ERROR;
            }
            break;

        case SP_RETURNCODE:
            if (Ns_DbSpReturnCode(handlePtr, tmpbuf, 32) != NS_OK) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            } else {
                Tcl_SetObjResult(interp, Tcl_NewStringObj(tmpbuf, TCL_INDEX_NONE));
            }
            break;

        default:
            /* should not happen */
            assert(cmd && 0);
        }
        break;

    case DML:               NS_FALL_THROUGH; /* fall through */
    case EXEC:              NS_FALL_THROUGH; /* fall through */
    case GETROW:            NS_FALL_THROUGH; /* fall through */
    case INTERPRETSQLFILE:  NS_FALL_THROUGH; /* fall through */
    case ONE_ROW:           NS_FALL_THROUGH; /* fall through */
    case SELECT:            NS_FALL_THROUGH; /* fall through */
    case SP_START:          NS_FALL_THROUGH; /* fall through */
    case ZERO_OR_ONE_ROW:
        {
            const char *value;
            TCL_SIZE_T  valueLength = 0;
            Tcl_DString ds;

            /*
             * The following commands require a 3rd argument.
             */

            if (objc != 4) {
                if (cmd == INTERPRETSQLFILE) {
                    Tcl_WrongNumArgs(interp, 2, objv, "/handle/ /sqlfile/");

                } else if (cmd == GETROW) {
                    Tcl_WrongNumArgs(interp, 2, objv, "/handle/ /setId/");

                } else if (cmd == SP_START) {
                    Tcl_WrongNumArgs(interp, 2, objv, "/handle/ /procname/");

                } else {
                    Tcl_WrongNumArgs(interp, 2, objv, "/handle/ /sql/");
                }
                return TCL_ERROR;
            }

            if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, &hPtr) != TCL_OK) {
                return TCL_ERROR;
            }
            Tcl_DStringFree(&handlePtr->dsExceptionMsg);
            handlePtr->cExceptionCode[0] = '\0';
            value = Tcl_GetStringFromObj(objv[3], &valueLength);

            /*
             * Convert data to external UTF-8... and lets hope, the
             * driver can handle UTF-8. In case it does not, we might
             * have to tailor this functionality for certain drivers
             * (would need additional configuration options).
             */
            (void)Tcl_UtfToExternalDString(NULL, value, valueLength, &ds);
            value = ds.string;

            /*if (cmd != GETROW) {
                fprintf(stderr, "CMD %s: <%s> (%s)\n", Tcl_GetString(objv[1]), value,
                        objv[3]->typePtr ? objv[3]->typePtr->name : "none");
                        }*/

            switch (cmd) {
            case DML:
                if (Ns_DbDML(handlePtr, value) != NS_OK) {
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
                }
                break;

            case ONE_ROW:
                rowPtr = Ns_Db1Row(handlePtr, value);
                if (rowPtr == NULL) {
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

                } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
                    result = TCL_ERROR;
                }
                break;

            case ZERO_OR_ONE_ROW:
                rowPtr = Ns_Db0or1Row(handlePtr, value, &nrows);
                if (rowPtr == NULL) {
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

                } else if (nrows == 0) {
                    Ns_SetFree(rowPtr);

                } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
                    result = TCL_ERROR;
                }
                break;

            case EXEC:
                switch (Ns_DbExec(handlePtr, value)) {
                case NS_DML:
                    Tcl_SetObjResult(interp, Tcl_NewStringObj("NS_DML", 6));
                    break;
                case NS_ROWS:
                    Tcl_SetObjResult(interp, Tcl_NewStringObj("NS_ROWS", 7));
                    break;
                default:
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
                }
                break;

            case SELECT:
                rowPtr = Ns_DbSelect(handlePtr, value);
                if (rowPtr == NULL) {
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

                } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_STATIC) != TCL_OK)) {
                    result = TCL_ERROR;
                }
                break;

            case SP_START:
                if (Ns_DbSpStart(handlePtr, value) != NS_OK) {
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
                } else {
                    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
                }
                break;

            case INTERPRETSQLFILE:
                if (Ns_DbInterpretSqlFile(handlePtr, value) != NS_OK) {
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
                }
                break;


            case GETROW:
                if (Ns_TclGetSet2(interp, value, &rowPtr) != TCL_OK) {
                    result = TCL_ERROR;
                } else {
                    switch (Ns_DbGetRow(handlePtr, rowPtr)) {
                    case NS_OK:
                        Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
                        break;
                    case NS_END_DATA:
                        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
                        break;
                    default:
                        result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
                    }
                }
                break;

            default:
                /* should not happen */
                assert(cmd && 0);
            }

            Tcl_DStringFree(&ds);
        }
        break;

#ifdef NS_WITH_DEPRECATED
    case VERBOSE:
        {
            int         verbose = 0;
            char       *idString;
            Ns_ObjvSpec args[] = {
                {"handle",   Ns_ObjvString, &idString, NULL},
                {"?verbose", Ns_ObjvBool,   &verbose, NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
                result = TCL_ERROR;

            } else if (DbGetHandle(idataPtr, interp, idString, &handlePtr, NULL) != TCL_OK) {
                result = TCL_ERROR;

            } else {
                assert(handlePtr != NULL);
                Ns_LogDeprecated(objv, 2, "ns_logctl debug(sql) ...", NULL);

                if (objc == 4) {
                    handlePtr->verbose = verbose;
                    (void) Ns_LogSeveritySetEnabled(Ns_LogSqlDebug, (bool)verbose);
                }

                Tcl_SetObjResult(interp, Tcl_NewBooleanObj(handlePtr->verbose));
            }
        }
        break;
#endif

    case SETEXCEPTION:
        if (objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "/handle/ /code/ /message/");
            result = TCL_ERROR;

        } else if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, NULL) != TCL_OK) {
            result = TCL_ERROR;

        } else {
            const char *code;
            TCL_SIZE_T  codeLen;

            assert(handlePtr != NULL);

            code = Tcl_GetStringFromObj(objv[3], &codeLen);
            if (codeLen > 5) {
                Ns_TclPrintfResult(interp, "code \"%s"" more than 5 characters", code);
                result = TCL_ERROR;
            } else {
                Ns_DbSetException(handlePtr, code, Tcl_GetString(objv[4]));
            }
        }
        break;

    case SP_SETPARAM:
        if (objc != 7) {
            Tcl_WrongNumArgs(interp, 2, objv, "/handle/ /paramname/ /type/ in|out /value/");
            result = TCL_ERROR;
        } else {
            const char *arg5 = Tcl_GetString(objv[5]);

            if (!STREQ(arg5, "in") && !STREQ(arg5, "out")) {
                Ns_TclPrintfResult(interp, "direction of setparam must "
                                   "be \"in\" or \"out\"");
                result = TCL_ERROR;

            } else if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, NULL) != TCL_OK) {
                result = TCL_ERROR;

            } else {
                assert(handlePtr != NULL);

                if (Ns_DbSpSetParam(handlePtr,
                                    Tcl_GetString(objv[3]),
                                    Tcl_GetString(objv[4]),
                                    arg5,
                                    Tcl_GetString(objv[6])) != NS_OK) {
                    result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
                } else {
                    Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
                }
            }
        }
        break;

    default:
        /* should not happen */
        assert(cmd && 0);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 * DbErrorCodeObjCmd --
 *
 *      Implements "ns_dberrorcode".
 *      Returns database exception code for the database handle.
 *
 * Results:
 *      Returns TCL_OK and database exception code is set as Tcl result
 *      or TCL_ERROR if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ErrorObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, char cmd)
{
    InterpData  *idataPtr = clientData;
    Ns_DbHandle *handle;
    int          result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/handle/");
        result = TCL_ERROR;

    } else if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[1]), &handle, NULL) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        if (cmd == 'c') {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handle->cExceptionCode, TCL_INDEX_NONE));
        } else {
            Tcl_DStringResult(interp, &handle->dsExceptionMsg);
        }
    }
    return result;
}

static int
DbErrorCodeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return ErrorObjCmd(clientData, interp, objc, objv, 'c');
}

static int
DbErrorMsgObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return ErrorObjCmd(clientData, interp, objc, objv, 'm');
}


/*
 *----------------------------------------------------------------------
 * DbConfigPathObjCmd --
 *
 *      Implements "ns_dbconfigpath". Get the database section name
 *      from the configuration file.
 *
 * Results:
 *      TCL_OK and the database section name is set as the Tcl result
 *      or TCL_ERROR if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
DbConfigPathObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const InterpData *idataPtr = clientData;
        const char *section = Ns_ConfigSectionPath(NULL, idataPtr->server, NULL, "db", NS_SENTINEL);

        Tcl_SetObjResult(interp, Tcl_NewStringObj(section, TCL_INDEX_NONE));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * PoolDescriptionObjCmd --
 *
 *      Implements "ns_dbpooldescription". Returns the DB pool's
 *      description string.
 *
 * Results:
 *      Return TCL_OK and the pool's description string is set as the
 *      Tcl result string or TCL_ERROR if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
PoolDescriptionObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/poolname/");
        result = TCL_ERROR;
    } else {
#ifdef NS_WITH_DEPRECATED
        const char *subcmdName = Tcl_GetString(objv[0]);

        if (strcmp(subcmdName, "ns_pooldescription") == 0) {
            Ns_LogDeprecated(objv, 2, "ns_dbpooldescription ...", NULL);
        }
#endif

        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DbPoolDescription(Tcl_GetString(objv[1])), TCL_INDEX_NONE));
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 * QuoteListToListObjCmd --
 *
 *      Implements "ns_quotelisttolist". Removes space, \ and '
 *      characters in a string.
 *
 * Results:
 *      TCL_OK and set the stripped string as the Tcl result or TCL_ERROR
 *      if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
QuoteListToListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/value/");
        result = TCL_ERROR;
    } else {
        const char *quotelist;
        bool        inquotes;
        Tcl_DString ds;
        Tcl_Obj    *listObj = Tcl_NewListObj(0, NULL);

        Tcl_DStringInit(&ds);
        quotelist = Tcl_GetString(objv[1]);
        inquotes = NS_FALSE;

        while (*quotelist != '\0') {
            if (CHARTYPE(space, *quotelist) != 0 && !inquotes) {
                if (ds.length != 0) {
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ds.string, ds.length));
                    Tcl_DStringSetLength(&ds, 0);
                }
                while (CHARTYPE(space, *quotelist) != 0) {
                    quotelist++;
                }
            } else if (*quotelist == '\\' && (*(quotelist + 1) != '\0')) {
                Tcl_DStringAppend(&ds, quotelist + 1, 1);
                quotelist += 2;
            } else if (*quotelist == '\'') {
                if (inquotes) {
                    /* Finish element */
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ds.string, ds.length));
                    Tcl_DStringSetLength(&ds, 0);
                    inquotes = NS_FALSE;
                } else {
                    /* Start element */
                    inquotes = NS_TRUE;
                }
                quotelist++;
            } else {
                Tcl_DStringAppend(&ds, quotelist, 1);
                quotelist++;
            }
        }
        if (ds.length != 0) {
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ds.string, ds.length));
        }
        Tcl_DStringFree(&ds);
        Tcl_SetObjResult(interp, listObj);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * QuoteSqlValue --
 *
 *      Helper function for QuoteValueObjCmd() and QuoteListObjCmd()
 *      doing the actual quoting work.
 *
 * Results:
 *      Ns_ReturnCode to indicate success (NS_OK or NS_ERROR).
 *
 * Side effects:
 *      Updates dsPtr by appending the value.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
QuoteSqlValue(Tcl_DString *dsPtr, Tcl_Obj *valueObj, int valueType)
{
    TCL_SIZE_T    valueLength;
    const char   *valueString;
    Ns_ReturnCode result = NS_OK;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(valueObj != NULL);

    valueString = Tcl_GetStringFromObj(valueObj, &valueLength);

    if (NS_intTypePtr == NULL) {
        Tcl_Panic("QuoteSqlValue: no int type");
    }

    if (valueObj->typePtr == NS_intTypePtr) {
        /*
         * Since we can trust the byterep, we can bypass the expensive
         * Tcl_UtfToExternalDString check.
         */
        if (valueType != INTCHAR('q')) {
            Tcl_DStringAppend(dsPtr, valueString, valueLength);
        } else {
            Tcl_DStringAppend(dsPtr, "'", 1);
            Tcl_DStringAppend(dsPtr, valueString, valueLength);
            Tcl_DStringAppend(dsPtr, "'", 1);
        }
    } else {
        Tcl_DString ds;
        /*
         * Protect against potential attacks, e.g. embedded nulls
         * appearing after conversion to the external value.
         */
        Tcl_DStringInit(&ds);
        (void)Tcl_UtfToExternalDString(NULL, valueString, valueLength, &ds);

        if (strlen(ds.string) < (size_t)ds.length) {
            result = NS_ERROR;

        } else if (valueType != INTCHAR('q')) {
            Tcl_DStringAppend(dsPtr, valueString, valueLength);

        } else {
            Tcl_DStringAppend(dsPtr, "'", 1);

            for (;;) {
                const char *p = strchr(valueString, INTCHAR('\''));
                if (p == NULL) {
                    Tcl_DStringAppend(dsPtr, valueString, valueLength);
                    break;
                } else {
                    TCL_SIZE_T length = (TCL_SIZE_T)((p - valueString) + 1);

                    Tcl_DStringAppend(dsPtr, valueString, length);
                    Tcl_DStringAppend(dsPtr, "'", 1);
                    valueString = p+1;
                    valueLength -= length;
                }
            }
            Tcl_DStringAppend(dsPtr, "'", 1);
        }
        Tcl_DStringFree(&ds);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 * QuoteValueObjCmd --
 *
 *      Implements "ns_dbquotevalue".
 *      Prepare a value string for inclusion in an SQL statement:
 *      -  "" is translated into NULL.
 *      -  All values of any numeric type are left alone.
 *      -  All other values are surrounded by single quotes and any
 *         single quotes included in the value are escaped (i.e. translated
 *         into 2 single quotes).
 *
 * Results:
 *      Tcl standard result codes.
 *
 * Side effects:
 *      Modifying interp result.
 *
 *----------------------------------------------------------------------
 */
static int
QuoteValueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result, valueType = INTCHAR('q');
    Tcl_Obj    *valueObj;
    Ns_ObjvSpec args[] = {
        {"value",    Ns_ObjvObj,   &valueObj,  NULL},
        {"?type",    Ns_ObjvIndex, &valueType, ns_const2voidp(valueTypes)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (*Tcl_GetString(valueObj) == '\0') {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("NULL", 4));
        result = TCL_OK;

    } else {
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        if (unlikely(QuoteSqlValue(&ds, valueObj, valueType) == NS_ERROR)) {
            Ns_TclPrintfResult(interp, "input string '%s' contains invalid characters",
                               Tcl_GetString(valueObj));
            Tcl_DStringFree(&ds);
            result = TCL_ERROR;

        } else {
            Tcl_DStringResult(interp, &ds);
            result = TCL_OK;
        }

    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * QuoteListObjCmd --
 *
 *      Implements "ns_dbquotelist".
 *      Prepare a value string for inclusion in an SQL statement:
 *      -  "" is translated into NULL.
 *      -  All values of any numeric type are left alone.
 *      -  All other values are surrounded by single quotes and any
 *         single quotes included in the value are escaped (i.e. translated
 *         into 2 single quotes).
 *
 * Results:
 *      Tcl standard result codes.
 *
 * Side effects:
 *      Modifying interp result.
 *
 *----------------------------------------------------------------------
 */

static int
QuoteListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, valueType = INTCHAR('q');
    Tcl_Obj    *listObj;
    Ns_ObjvSpec args[] = {
        {"list",  Ns_ObjvObj,   &listObj,   NULL},
        {"?type", Ns_ObjvIndex, &valueType, ns_const2voidp(valueTypes)},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString ds;
        TCL_SIZE_T  oc;
        Tcl_Obj   **ov;

        Tcl_DStringInit(&ds);
        if (Tcl_ListObjGetElements(interp, listObj, &oc, &ov) == TCL_OK) {
            TCL_SIZE_T i;

            for (i = 0; i < oc; i++) {

                if (QuoteSqlValue(&ds, ov[i], valueType) != NS_OK) {
                    Ns_TclPrintfResult(interp, "input string '%s' contains invalid characters",
                                       Tcl_GetString(ov[i]));
                    result = TCL_ERROR;
                    break;

                } else {
                    if (i < oc-1) {
                        Tcl_DStringAppend(&ds, ",", 1);
                    }
                }
            }
            if (result == TCL_OK) {
                Tcl_DStringResult(interp, &ds);
            } else {
                Tcl_DStringFree(&ds);
            }

        } else {
            result = TCL_ERROR;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * DbGetHandle --
 *
 *      Get database handle from its handle id.
 *
 * Results:
 *      Return TCL_OK if handle is found or TCL_ERROR otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
DbGetHandle(InterpData *idataPtr, Tcl_Interp *interp, const char *handleId, Ns_DbHandle **handle,
            Tcl_HashEntry **hPtrPtr)
{
    Tcl_HashEntry  *hPtr;
    int             result = TCL_OK;

    hPtr = Tcl_FindHashEntry(&idataPtr->dbs, handleId);
    if (hPtr == NULL) {
        Ns_TclPrintfResult(interp, "invalid database id: \"%s\"", handleId);
        result = TCL_ERROR;

    } else {
        *handle = (Ns_DbHandle *) Tcl_GetHashValue(hPtr);
        if (hPtrPtr != NULL) {
            *hPtrPtr = hPtr;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * EnterDbHandle --
 *
 *      Enter a database handle and create its handle id.
 *
 * Results:
 *      The database handle id is returned as a Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
EnterDbHandle(InterpData *idataPtr, Tcl_Interp *interp, Ns_DbHandle *handle, Tcl_Obj *listObj)
{
    Tcl_HashEntry *hPtr;
    int            isNew;
    TCL_SIZE_T     len, next;
    char           buf[100];

    NS_NONNULL_ASSERT(idataPtr != NULL);
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(handle != NULL);
    NS_NONNULL_ASSERT(listObj != NULL);

    next = (TCL_SIZE_T)idataPtr->dbs.numEntries;
    do {
        len = (TCL_SIZE_T)snprintf(buf, sizeof(buf), "nsdb%lx", (unsigned long)next++);
        hPtr = Tcl_CreateHashEntry(&idataPtr->dbs, buf, &isNew);
    } while (isNew == 0);

    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(buf, len));
    Tcl_SetHashValue(hPtr, handle);
}


/*
 *----------------------------------------------------------------------
 * DbFail --
 *
 *      Common routine that creates database failure message.
 *
 * Results:
 *      Return TCL_ERROR and set database failure message as Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
DbFail(Tcl_Interp *interp, Ns_DbHandle *handle, const char *cmd)
{
    Tcl_DString ds;
    NS_NONNULL_ASSERT(handle != NULL);

    Tcl_DStringInit(&ds);
    Ns_DStringPrintf(&ds, "Database operation \"%s\" failed", cmd);
    if (handle->cExceptionCode[0] != '\0') {
        Ns_DStringPrintf(&ds, " (exception %s", handle->cExceptionCode);
        if (handle->dsExceptionMsg.length > 0) {
            Ns_DStringPrintf(&ds, ", \"%s\"", handle->dsExceptionMsg.string);
        }
        Ns_DStringPrintf(&ds, ")");
    }
    Tcl_DStringResult(interp, &ds);
    NsDbSetActive("dbfail", handle, NS_FALSE);

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 * FreeData --
 *
 *      Free per-interp data at interp delete time.
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
FreeData(ClientData clientData, Tcl_Interp *UNUSED(interp))
{
    InterpData *idataPtr = clientData;

    Tcl_DeleteHashTable(&idataPtr->dbs);
    ns_free(idataPtr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
