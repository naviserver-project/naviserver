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
 * Local functions defined in this file
 */

static int DbFail(Tcl_Interp *interp, Ns_DbHandle *handle, const char *cmd)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static void EnterDbHandle(InterpData *idataPtr, Tcl_Interp *interp, Ns_DbHandle *handle, Tcl_Obj *listObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);
static int DbGetHandle(InterpData *idataPtr, Tcl_Interp *interp, const char *handleId,
                       Ns_DbHandle **handle, Tcl_HashEntry **hPtrPtr);

#if !defined(NS_TCL_PRE85)
static Ns_ReturnCode CurrentHandles( Tcl_Interp *interp, Tcl_HashTable *tablePtr, Tcl_Obj *dictObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
#endif

static Tcl_InterpDeleteProc FreeData;
static Tcl_ObjCmdProc
    DbConfigPathObjCmd,
    DbErrorCodeObjCmd,
    DbErrorMsgObjCmd,
    DbObjCmd,
    GetCsvObjCmd,
    PoolDescriptionObjCmd,
    QuoteListObjCmd,
    QuoteListToListObjCmd,
    QuoteValueObjCmd;

static int ErrorObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, char cmd);


/*
 * Local variables defined in this file.
 */

static const char *const datakey = "nsdb:data";

static const Ns_ObjvTable valueTypes[] = {
    {"decimal",  UCHAR('n')},
    {"double",   UCHAR('n')},
    {"integer",  UCHAR('n')},
    {"int",      UCHAR('n')},
    {"real",     UCHAR('n')},
    {"smallint", UCHAR('n')},
    {"bigint",   UCHAR('n')},
    {"bit",      UCHAR('n')},
    {"float",    UCHAR('n')},
    {"numeric",  UCHAR('n')},
    {"tinyint",  UCHAR('n')},
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

    (void)Tcl_CreateObjCommand(interp, "ns_db", DbObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_dbconfigpath", DbConfigPathObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_dberrorcode", DbErrorCodeObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_dberrormsg", DbErrorMsgObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_dbquotevalue", QuoteValueObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_dbquotelist", QuoteListObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_getcsv", GetCsvObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_pooldescription", PoolDescriptionObjCmd, idataPtr, NULL);
    (void)Tcl_CreateObjCommand(interp, "ns_quotelisttolist", QuoteListToListObjCmd, idataPtr, NULL);

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

        keyv[0] = Tcl_NewStringObj(handlePtr->poolname, -1);
        keyv[1] = Tcl_NewStringObj(Tcl_GetHashKey(tablePtr, hPtr), -1);
        Tcl_DictObjPutKeyList(interp, dictObj, 2, keyv, Tcl_NewIntObj(NsDbGetActive(handlePtr)));
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
 *      Implement the NaviServer ns_db Tcl command.
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
DbObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
        USER,
        VERBOSE
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
        "verbose",
        NULL
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], subcmd, "option", 0, &cmd) != TCL_OK) {
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
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(pool, -1));
                    pool = pool + strlen(pool) + 1;
                }
                Tcl_SetObjResult(interp, listObj);
            }
        }
        break;

    case BOUNCEPOOL:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool");
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
            {"?pool",       Ns_ObjvString, &poolString, NULL},
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
            {"?pool",        Ns_ObjvString, &poolString, NULL},
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
            Tcl_WrongNumArgs(interp, 2, objv, "dbId");
            result = TCL_ERROR;

        } else if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, NULL) != TCL_OK) {
            result = TCL_ERROR;

        } else {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(handlePtr->cExceptionCode, -1));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(handlePtr->dsExceptionMsg.string, -1));
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

    case POOLNAME:       NS_FALL_THROUGH; /* fall through */
    case PASSWORD:       NS_FALL_THROUGH; /* fall through */
    case USER:           NS_FALL_THROUGH; /* fall through */
    case DATASOURCE:     NS_FALL_THROUGH; /* fall through */
    case DISCONNECT:     NS_FALL_THROUGH; /* fall through */
    case DBTYPE:         NS_FALL_THROUGH; /* fall through */
    case DRIVER:         NS_FALL_THROUGH; /* fall through */
    case CANCEL:         NS_FALL_THROUGH; /* fall through */
    case BINDROW:        NS_FALL_THROUGH; /* fall through */
    case FLUSH:          NS_FALL_THROUGH; /* fall through */
    case RELEASEHANDLE:  NS_FALL_THROUGH; /* fall through */
    case RESETHANDLE:    NS_FALL_THROUGH; /* fall through */
    case CONNECTED:      NS_FALL_THROUGH; /* fall through */
    case SP_EXEC:        NS_FALL_THROUGH; /* fall through */
    case SP_GETPARAMS:   NS_FALL_THROUGH; /* fall through */
    case SP_RETURNCODE:  NS_FALL_THROUGH; /* fall through */
    case SESSIONID:

        if (objc < 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "dbId");
            return TCL_ERROR;
        }
        if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, &hPtr) != TCL_OK) {
            return TCL_ERROR;
        }
        Ns_DStringFree(&handlePtr->dsExceptionMsg);
        handlePtr->cExceptionCode[0] = '\0';

        /*
         * The following commands require just the handle.
         */

        switch (cmd) {
        case POOLNAME:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->poolname, -1));
            break;

        case PASSWORD:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->password, -1));
            break;

        case USER:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->user, -1));
            break;

        case DATASOURCE:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handlePtr->datasource, -1));
            break;

        case DISCONNECT:
            NsDbDisconnect(handlePtr);
            break;

        case DBTYPE:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DbDriverDbType(handlePtr), -1));
            break;

        case DRIVER:
            Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DbDriverName(handlePtr), -1));
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

        case SESSIONID:
            {
                char idstr[TCL_INTEGER_SPACE + 4];
                int  length;

                memcpy(idstr, "sid", 3u);
                length = ns_uint64toa(&idstr[3], (uint64_t)NsDbGetSessionId(handlePtr));
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
                Tcl_SetObjResult(interp, Tcl_NewStringObj(tmpbuf, -1));
            }
            break;

        default:
            /* should not happen */
            assert(cmd && 0);
        }
        break;

    case DML:               NS_FALL_THROUGH; /* fall through */
    case GETROW:            NS_FALL_THROUGH; /* fall through */
    case ONE_ROW:           NS_FALL_THROUGH; /* fall through */
    case ZERO_OR_ONE_ROW:   NS_FALL_THROUGH; /* fall through */
    case EXEC:              NS_FALL_THROUGH; /* fall through */
    case SELECT:            NS_FALL_THROUGH; /* fall through */
    case SP_START:          NS_FALL_THROUGH; /* fall through */
    case INTERPRETSQLFILE:

        /*
         * The following commands require a 3rd argument.
         */

        if (objc != 4) {
            if (cmd == INTERPRETSQLFILE) {
                Tcl_WrongNumArgs(interp, 2, objv, "dbId sqlfile");

            } else if (cmd == GETROW) {
                Tcl_WrongNumArgs(interp, 2, objv, "dbId row");

            } else {
                Tcl_WrongNumArgs(interp, 2, objv, "dbId sql");
            }
            return TCL_ERROR;
        }

        if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, &hPtr) != TCL_OK) {
            return TCL_ERROR;
        }
        Ns_DStringFree(&handlePtr->dsExceptionMsg);
        handlePtr->cExceptionCode[0] = '\0';

        switch (cmd) {
        case DML:
            if (Ns_DbDML(handlePtr, Tcl_GetString(objv[3])) != NS_OK) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case ONE_ROW:
            rowPtr = Ns_Db1Row(handlePtr, Tcl_GetString(objv[3]));
            if (rowPtr == NULL) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

            } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
                result = TCL_ERROR;
            }
            break;

        case ZERO_OR_ONE_ROW:
            rowPtr = Ns_Db0or1Row(handlePtr, Tcl_GetString(objv[3]), &nrows);
            if (rowPtr == NULL) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

            } else if (nrows == 0) {
                Ns_SetFree(rowPtr);

            } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
                result = TCL_ERROR;
            }
            break;

        case EXEC:
            switch (Ns_DbExec(handlePtr, Tcl_GetString(objv[3]))) {
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
            rowPtr = Ns_DbSelect(handlePtr, Tcl_GetString(objv[3]));
            if (rowPtr == NULL) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));

            } else if (unlikely(Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_STATIC) != TCL_OK)) {
                result = TCL_ERROR;
            }
            break;

        case SP_START:
            if (Ns_DbSpStart(handlePtr, Tcl_GetString(objv[3])) != NS_OK) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            } else {
                Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
            }
            break;

        case INTERPRETSQLFILE:
            if (Ns_DbInterpretSqlFile(handlePtr, Tcl_GetString(objv[3])) != NS_OK) {
                result = DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;


        case GETROW:
            if (Ns_TclGetSet2(interp, Tcl_GetString(objv[3]), &rowPtr) != TCL_OK) {
                return TCL_ERROR;
            }
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
            break;

        default:
            /* should not happen */
            assert(cmd && 0);
        }
        break;

    case VERBOSE:
        {
            int         verbose = 0;
            char       *idString;
            Ns_ObjvSpec args[] = {
                {"dbID",     Ns_ObjvString, &idString, NULL},
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

    case SETEXCEPTION:
        if (objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "dbId code message");
            result = TCL_ERROR;

        } else if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, NULL) != TCL_OK) {
            result = TCL_ERROR;

        } else {
            const char *code;
            int codeLen;

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
            Tcl_WrongNumArgs(interp, 2, objv, "dbId paramname type in|out value");
            result = TCL_ERROR;
        } else {
            const char *arg5 = Tcl_GetString(objv[5]);

            if (!STREQ(arg5, "in") && !STREQ(arg5, "out")) {
                Ns_TclPrintfResult(interp, "inout parameter of setparam must "
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
 * DbErrorCodeCmd --
 *
 *      Get database exception code for the database handle.
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
ErrorObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv, char cmd)
{
    InterpData  *idataPtr = clientData;
    Ns_DbHandle *handle;
    int          result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "dbId");
        result = TCL_ERROR;

    } else if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[1]), &handle, NULL) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        if (cmd == 'c') {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(handle->cExceptionCode, -1));
        } else {
            Tcl_DStringResult(interp, &handle->dsExceptionMsg);
        }
    }
    return result;
}

static int
DbErrorCodeObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    return ErrorObjCmd(clientData, interp, objc, objv, 'c');
}

static int
DbErrorMsgObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    return ErrorObjCmd(clientData, interp, objc, objv, 'm');
}


/*
 *----------------------------------------------------------------------
 * DbConfigPathCmd --
 *
 *      Get the database section name from the configuration file.
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
DbConfigPathObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 0, objv, NULL);
        result = TCL_ERROR;
    } else {
        const InterpData *idataPtr = clientData;
        const char *section = Ns_ConfigGetPath(idataPtr->server, NULL, "db", (char *)0L);

        Tcl_SetObjResult(interp, Tcl_NewStringObj(section, -1));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * PoolDescriptionCmd --
 *
 *      Get the pool's description string.
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
PoolDescriptionObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "poolname");
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DbPoolDescription(Tcl_GetString(objv[1])), -1));
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 * QuoteListToListCmd --
 *
 *      Remove space, \ and ' characters in a string.
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
QuoteListToListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "quotelist");
        result = TCL_ERROR;
    } else {
        const char *quotelist;
        bool        inquotes;
        Ns_DString  ds;
        Tcl_Obj    *listObj = Tcl_NewListObj(0, NULL);

        Ns_DStringInit(&ds);
        quotelist = Tcl_GetString(objv[1]);
        inquotes = NS_FALSE;

        while (*quotelist != '\0') {
            if (CHARTYPE(space, *quotelist) != 0 && !inquotes) {
                if (ds.length != 0) {
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ds.string, ds.length));
                    Ns_DStringSetLength(&ds, 0);
                }
                while (CHARTYPE(space, *quotelist) != 0) {
                    quotelist++;
                }
            } else if (*quotelist == '\\' && (*(quotelist + 1) != '\0')) {
                Ns_DStringNAppend(&ds, quotelist + 1, 1);
                quotelist += 2;
            } else if (*quotelist == '\'') {
                if (inquotes) {
                    /* Finish element */
                    Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ds.string, ds.length));
                    Ns_DStringSetLength(&ds, 0);
                    inquotes = NS_FALSE;
                } else {
                    /* Start element */
                    inquotes = NS_TRUE;
                }
                quotelist++;
            } else {
                Ns_DStringNAppend(&ds, quotelist, 1);
                quotelist++;
            }
        }
        if (ds.length != 0) {
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(ds.string, ds.length));
        }
        Ns_DStringFree(&ds);
        Tcl_SetObjResult(interp, listObj);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * QuoteValueObjCmd --
 *
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
static void
QuoteSqlValue(Tcl_DString *dsPtr, Tcl_Obj *valueObj, unsigned char valueType)
{
    int         valueLength;
    const char *valueString;

    valueString = Tcl_GetStringFromObj(valueObj, &valueLength);

    if (valueType == INTCHAR('n')) {
        Tcl_DStringAppend(dsPtr, valueString, valueLength);

    } else {
        Tcl_DStringAppend(dsPtr, "'", 1);

        while (1) {
            const char *p = strchr(valueString, INTCHAR('\''));
            if (p == NULL) {
                Tcl_DStringAppend(dsPtr, valueString, valueLength);
                break;
            } else {
                int length = (p - valueString) + 1;
                Tcl_DStringAppend(dsPtr, valueString, length);
                Tcl_DStringAppend(dsPtr, "'", 1);
                valueString = p+1;
                valueLength -= length;
            }
        }
        Tcl_DStringAppend(dsPtr, "'", 1);
    }
}

static int
QuoteValueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result, valueType = INTCHAR('q');
    Tcl_Obj    *valueObj;
    Ns_ObjvSpec args[] = {
        {"value",    Ns_ObjvObj,   &valueObj,  NULL},
        {"?type",    Ns_ObjvIndex, &valueType, (void*)valueTypes},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;


    } else if (*Tcl_GetString(valueObj) == '\0') {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("NULL", 4));
        result = TCL_OK;

    } else if (valueType == INTCHAR('n')) {
        Tcl_SetObjResult(interp, valueObj);
        result = TCL_OK;

    } else {
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        QuoteSqlValue(&ds, valueObj, valueType);
        Tcl_DStringResult(interp, &ds);

        result = TCL_OK;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * QuoteListObjCmd --
 *
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
QuoteListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result, valueType = INTCHAR('q');
    Tcl_Obj    *listObj;
    Ns_ObjvSpec args[] = {
        {"list",  Ns_ObjvObj,   &listObj,   NULL},
        {"?type", Ns_ObjvIndex, &valueType, (void*)valueTypes},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString ds;
        int         objc;
        Tcl_Obj   **objv;

        Tcl_DStringInit(&ds);
        if (Tcl_ListObjGetElements(interp, listObj, &objc, &objv) == TCL_OK) {
            int i;

            for (i = 0; i < objc; i++) {
                QuoteSqlValue(&ds, objv[i], valueType);
                if (i < objc-1) {
                    Tcl_DStringAppend(&ds, ",", 1);
                }
            }
            Tcl_DStringResult(interp, &ds);
            result = TCL_OK;

        } else {
            result = TCL_ERROR;
        }

        result = TCL_OK;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * GetCsvCmd --
 *
 *      Implement the ns_getcsv command to read a single line from a CSV file
 *      and parse the results into a Tcl list variable.
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      One line is read for given open channel.
 *
 *----------------------------------------------------------------------
 */

static int
GetCsvObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int           result = TCL_OK;
    char         *delimiter = (char *)",", *fileId, *varName;
    Tcl_Channel   chan;
    Ns_ObjvSpec   opts[] = {
        {"-delimiter", Ns_ObjvString,   &delimiter, NULL},
        {"--",         Ns_ObjvBreak,    NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec   args[] = {
        {"fileId",     Ns_ObjvString, &fileId,   NULL},
        {"varName",    Ns_ObjvString, &varName,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (Ns_TclGetOpenChannel(interp, fileId, 0, NS_FALSE, &chan) == TCL_ERROR) {
        result = TCL_ERROR;

    } else {
        int             ncols;
        bool            inquote, quoted, blank;
        const char     *p, *value;
        Tcl_DString     line, cols, elem;
        char            c;

        Tcl_DStringInit(&line);
        if (Tcl_Gets(chan, &line) < 0) {
            Tcl_DStringFree(&line);
            if (Tcl_Eof(chan) == 0) {
                Ns_TclPrintfResult(interp, "could not read from %s: %s",
                                   fileId, Tcl_PosixError(interp));
                return TCL_ERROR;
            }
            Tcl_SetObjResult(interp, Tcl_NewIntObj(-1));
            return TCL_OK;
        }

        Tcl_DStringInit(&cols);
        Tcl_DStringInit(&elem);
        ncols = 0;
        inquote = NS_FALSE;
        quoted = NS_FALSE;
        blank = NS_TRUE;

        p = line.string;
        while (*p != '\0') {
            c = *p++;
        loopstart:
            if (inquote) {
                if (c == '"') {
                    c = *p++;
                    if (c == '\0') {
                        break;
                    }
                    if (c == '"') {
                        Tcl_DStringAppend(&elem, &c, 1);
                    } else {
                        inquote = NS_FALSE;
                        goto loopstart;
                    }
                } else {
                    Tcl_DStringAppend(&elem, &c, 1);
                }
            } else {
                if ((c == '\n') || (c == '\r')) {
#if 0
                    /*
                     * Not sure, what the intention of the following block was,
                     * since the final break after this block jumps out of the
                     * loop.
                     */
                    while ((c = *p++) != '\0') {
                        if ((c != '\n') && (c != '\r')) {
                            *--p;
                            break;
                        }
                    }
#endif
                    break;
                }
                if (c == '"') {
                    inquote = NS_TRUE;
                    quoted = NS_TRUE;
                    blank = NS_FALSE;
                } else if ((c == '\r')
                           || ((elem.length == 0) && (CHARTYPE(space, c) != 0))
                           ) {
                    continue;
                } else if (strchr(delimiter, INTCHAR(c)) != NULL) {
                    if (!quoted) {
                        (void) Ns_StrTrimRight(elem.string);
                    }
                    Tcl_DStringAppendElement(&cols, elem.string);
                    Tcl_DStringSetLength(&elem, 0);
                    ncols++;
                    quoted = NS_FALSE;
                } else {
                    blank = NS_FALSE;
                    Tcl_DStringAppend(&elem, &c, 1);
                }
            }
        }
        if (!quoted) {
            (void) Ns_StrTrimRight(elem.string);
        }
        if (!blank) {
            Tcl_DStringAppendElement(&cols, elem.string);
            ncols++;
        }
        value = Tcl_SetVar(interp, varName, cols.string, TCL_LEAVE_ERR_MSG);
        Tcl_DStringFree(&line);
        Tcl_DStringFree(&cols);
        Tcl_DStringFree(&elem);

        if (value == NULL) {
            result = TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(ncols));
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
    int            isNew, next, len;
    char           buf[100];

    NS_NONNULL_ASSERT(idataPtr != NULL);
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(handle != NULL);
    NS_NONNULL_ASSERT(listObj != NULL);

    next = idataPtr->dbs.numEntries;
    do {
        len = snprintf(buf, sizeof(buf), "nsdb%x", next++);
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
