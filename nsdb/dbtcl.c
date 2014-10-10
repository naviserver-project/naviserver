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
 *	Tcl database access routines.
 */

#include "db.h"

/*
 * The following structure maintains per-interp data.
 */

typedef struct InterpData {
    char *server;
    Tcl_HashTable dbs;
} InterpData;

/*
 * Local functions defined in this file
 */

static int DbFail(Tcl_Interp *interp, Ns_DbHandle *handle, char *cmd)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static void EnterDbHandle(InterpData *idataPtr, Tcl_Interp *interp, Ns_DbHandle *handle);
static int DbGetHandle(InterpData *idataPtr, Tcl_Interp *interp, char *handleId,
		       Ns_DbHandle **handle, Tcl_HashEntry **phe);
static Tcl_InterpDeleteProc FreeData;
static Tcl_ObjCmdProc DbObjCmd, QuoteListToListObjCmd, GetCsvObjCmd, DbErrorCodeObjCmd,
       DbErrorMsgObjCmd, GetCsvObjCmd, DbConfigPathObjCmd, PoolDescriptionObjCmd;

/*
 * Local variables defined in this file.
 */

static const char *datakey = "nsdb:data";


/*
 *----------------------------------------------------------------------
 * Ns_TclDbGetHandle --
 *
 *      Get database handle from its handle id.
 *
 * Results:
 *      See DbGetHandle().
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclDbGetHandle(Tcl_Interp *interp, char *id, Ns_DbHandle **handlePtr)
{
    InterpData *idataPtr;

    idataPtr = Tcl_GetAssocData(interp, datakey, NULL);
    if (idataPtr == NULL) {
	return TCL_ERROR;
    }
    return DbGetHandle(idataPtr, interp, id, handlePtr, NULL);
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
NsDbAddCmds(Tcl_Interp *interp, void *arg)
{
    char       *server = arg;
    InterpData *idataPtr;

    /*
     * Initialize the per-interp data.
     */

    idataPtr = ns_malloc(sizeof(InterpData));
    idataPtr->server = server;
    Tcl_InitHashTable(&idataPtr->dbs, TCL_STRING_KEYS);
    Tcl_SetAssocData(interp, datakey, FreeData, idataPtr);

    Tcl_CreateObjCommand(interp, "ns_db", DbObjCmd, idataPtr, NULL);
    Tcl_CreateObjCommand(interp, "ns_quotelisttolist", QuoteListToListObjCmd, idataPtr, NULL);
    Tcl_CreateObjCommand(interp, "ns_getcsv", GetCsvObjCmd, idataPtr, NULL);
    Tcl_CreateObjCommand(interp, "ns_dberrorcode", DbErrorCodeObjCmd, idataPtr, NULL);
    Tcl_CreateObjCommand(interp, "ns_dberrormsg", DbErrorMsgObjCmd, idataPtr, NULL);
    Tcl_CreateObjCommand(interp, "ns_dbconfigpath", DbConfigPathObjCmd, idataPtr, NULL);
    Tcl_CreateObjCommand(interp, "ns_pooldescription", PoolDescriptionObjCmd, idataPtr, NULL);

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
NsDbReleaseHandles(Tcl_Interp *interp, void *arg)
{
    InterpData     *idataPtr;

    idataPtr = Tcl_GetAssocData(interp, datakey, NULL);
    if (idataPtr != NULL) {
        Tcl_HashSearch  search;
        Tcl_HashEntry *hPtr = Tcl_FirstHashEntry(&idataPtr->dbs, &search);

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


/*
 *----------------------------------------------------------------------
 *
 * DbCmd --
 *
 *      Implement the Naviserver ns_db Tcl command.
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
DbObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    InterpData	   *idataPtr = data;
    char            tmpbuf[32], *pool = NULL;
    int             cmd, nrows;
    Ns_DbHandle    *handlePtr = NULL;
    Ns_Set         *rowPtr;
    Tcl_HashEntry  *hPtr;

    enum {
        POOLS, BOUNCEPOOL, GETHANDLE, EXCEPTION, POOLNAME,
	PASSWORD, USER, DATASOURCE, DISCONNECT, DBTYPE, DRIVER, CANCEL, ROWCOUNT,
	BINDROW, FLUSH, RELEASEHANDLE, RESETHANDLE, CONNECTED, SP_EXEC,
	SP_GETPARAMS, SP_RETURNCODE, GETROW, DML, ONE_ROW, ZERO_OR_ONE_ROW, EXEC,
	SELECT, SP_START, INTERPRETSQLFILE, VERBOSE, SETEXCEPTION, SP_SETPARAM
    };
    static const char *subcmd[] = {
        "pools", "bouncepool", "gethandle", "exception", "poolname",
	"password", "user", "datasource", "disconnect", "dbtype", "driver", "cancel", "rowcount",
	"bindrow", "flush", "releasehandle", "resethandle", "connected", "sp_exec",
	"sp_getparams", "sp_returncode", "getrow", "dml", "1row", "0or1row", "exec",
	"select", "sp_start", "interpretsqlfile", "verbose", "setexception", "sp_setparam",
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
            return TCL_ERROR;
        }
	pool = Ns_DbPoolList(idataPtr->server);
	if (pool != NULL) {
	    while (*pool != '\0') {
		Tcl_AppendElement(interp, pool);
		pool = pool + strlen(pool) + 1;
	    }
	}
        break;

    case BOUNCEPOOL:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "pool");
            return TCL_ERROR;
	}
	if (Ns_DbBouncePool(Tcl_GetString(objv[2])) == NS_ERROR) {
	    Tcl_AppendResult(interp, "could not bounce: ", Tcl_GetString(objv[2]), NULL);
	    return TCL_ERROR;
	}
        break;

    case GETHANDLE: {
	int timeout = -1, nhandles = 1, result;
	Ns_DbHandle **handlesPtrPtr;

        Ns_ObjvSpec opts[] = {
            {"-timeout", Ns_ObjvInt,   &timeout, NULL},
            {"--",       Ns_ObjvBreak, NULL,       NULL},
            {NULL, NULL, NULL, NULL}
        };
        Ns_ObjvSpec args[] = {
            {"?pool", 	    Ns_ObjvString, &pool, NULL},
            {"?nhandles",   Ns_ObjvInt,    &nhandles,  NULL},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }

	/*
	 * Determine the pool and requested number of handles
	 * from the remaining args.
	 */

	if (pool == NULL) {
	    pool = Ns_DbPoolDefault(idataPtr->server);
            if (pool == NULL) {
                Tcl_SetResult(interp, "no defaultpool configured", TCL_STATIC);
                return TCL_ERROR;
            }
        }
        if (Ns_DbPoolAllowable(idataPtr->server, pool) == NS_FALSE) {
            Tcl_AppendResult(interp, "no access to pool: \"", pool, "\"",
			     NULL);
            return TCL_ERROR;
        }
	if (nhandles <= 0) {
            Tcl_AppendResult(interp, "invalid nhandles \"", nhandles,
                    "\": should be greater than 0.", NULL);
            return TCL_ERROR;
	}

    	/*
         * Allocate handles and enter them into Tcl.
         */

	if (nhandles == 1) {
    	    handlesPtrPtr = &handlePtr;
	} else {
	    handlesPtrPtr = ns_malloc(nhandles * sizeof(Ns_DbHandle *));
	}
	result = Ns_DbPoolTimedGetMultipleHandles(handlesPtrPtr, pool,
    	    	                                  nhandles, timeout);
    	if (result == NS_OK) {
  	    int i;

	    for (i = 0; i < nhandles; ++i) {
	        EnterDbHandle(idataPtr, interp, *(handlesPtrPtr + i));
            }
	}
	if (handlesPtrPtr != &handlePtr) {
	    ns_free(handlesPtrPtr);
	}
	if (result != NS_TIMEOUT && result != NS_OK) {
	  Ns_TclPrintfResult(interp,
			     "could not allocate %d handle%s from pool \"%s\"",
			     nhandles,
			     nhandles > 1 ? "s" : "",
			     pool);
	  return TCL_ERROR;
	}
	break;
    }

    case EXCEPTION:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "dbId");
            return TCL_ERROR;
        }
        if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[2]), &handlePtr, NULL) != TCL_OK) {
            return TCL_ERROR;
        }
        Tcl_AppendElement(interp, handlePtr->cExceptionCode);
        Tcl_AppendElement(interp, handlePtr->dsExceptionMsg.string);
        break;

    case POOLNAME:
    case PASSWORD:
    case USER:
    case DATASOURCE:
    case DISCONNECT:
    case DBTYPE:
    case DRIVER:
    case CANCEL:
    case BINDROW:
    case FLUSH:
    case RELEASEHANDLE:
    case RESETHANDLE:
    case CONNECTED:
    case SP_EXEC:
    case SP_GETPARAMS:
    case SP_RETURNCODE:

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
            Tcl_SetResult(interp, handlePtr->poolname, TCL_VOLATILE);
            break;

        case PASSWORD:
            Tcl_SetResult(interp, handlePtr->password, TCL_VOLATILE);
            break;

        case USER:
            Tcl_SetResult(interp, handlePtr->user, TCL_VOLATILE);
            break;

        case DATASOURCE:
      	    Tcl_SetResult(interp, handlePtr->datasource, TCL_STATIC);
            break;

        case DISCONNECT:
       	    NsDbDisconnect(handlePtr);
            break;

        case DBTYPE:
            Tcl_SetResult(interp, Ns_DbDriverDbType(handlePtr), TCL_STATIC);
            break;

        case DRIVER:
            Tcl_SetResult(interp, Ns_DbDriverName(handlePtr), TCL_STATIC);
            break;

        case CANCEL:
            if (Ns_DbCancel(handlePtr) != NS_OK) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case BINDROW:
            rowPtr = Ns_DbBindRow(handlePtr);
            if (rowPtr == NULL) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_STATIC);
            break;

        case ROWCOUNT:
            Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_DbGetRowCount(handlePtr)));
            break;

        case FLUSH:
            if (Ns_DbFlush(handlePtr) != NS_OK) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case RELEASEHANDLE:
	    Tcl_DeleteHashEntry(hPtr);
    	    Ns_DbPoolPutHandle(handlePtr);
            break;

        case RESETHANDLE:
	    if (Ns_DbResetHandle(handlePtr) != NS_OK) {
	      return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
	    }
	    Tcl_SetObjResult(interp, Tcl_NewIntObj(NS_OK));
            break;

        case CONNECTED:
      	    Tcl_SetObjResult(interp, Tcl_NewIntObj(handlePtr->connected));
            break;

        case SP_EXEC:
	    switch (Ns_DbSpExec(handlePtr)) {
	    case NS_DML:
	        Tcl_SetResult(interp, "NS_DML", TCL_STATIC);
	        break;
	    case NS_ROWS:
	        Tcl_SetResult(interp, "NS_ROWS", TCL_STATIC);
	        break;
	    default:
	        return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
	    }
            break;

        case SP_GETPARAMS:
	    rowPtr = Ns_DbSpGetParams(handlePtr);
	    if (rowPtr == NULL) {
	        return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
	    }
	    Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC);
            break;

        case SP_RETURNCODE:
	    if (Ns_DbSpReturnCode(handlePtr, tmpbuf, 32) != NS_OK) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
      	    }
	    Tcl_SetResult(interp, tmpbuf, TCL_VOLATILE);
            break;
        }
        break;

    case DML:
    case GETROW:
    case ONE_ROW:
    case ZERO_OR_ONE_ROW:
    case EXEC:
    case SELECT:
    case SP_START:
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
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case ONE_ROW:
            rowPtr = Ns_Db1Row(handlePtr, Tcl_GetString(objv[3]));
            if (rowPtr == NULL) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC);
            break;

        case ZERO_OR_ONE_ROW:
            rowPtr = Ns_Db0or1Row(handlePtr, Tcl_GetString(objv[3]), &nrows);
            if (rowPtr == NULL) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            if (nrows == 0) {
                Ns_SetFree(rowPtr);
            } else {
                Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_DYNAMIC);
            }
            break;

        case EXEC:
            switch (Ns_DbExec(handlePtr, Tcl_GetString(objv[3]))) {
            case NS_DML:
                Tcl_SetResult(interp, "NS_DML", TCL_STATIC);
                break;
            case NS_ROWS:
                Tcl_SetResult(interp, "NS_ROWS", TCL_STATIC);
                break;
            default:
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;

        case SELECT:
            rowPtr = Ns_DbSelect(handlePtr, Tcl_GetString(objv[3]));
            if (rowPtr == NULL) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            Ns_TclEnterSet(interp, rowPtr, NS_TCL_SET_STATIC);
            break;

        case SP_START:
	    if (Ns_DbSpStart(handlePtr, Tcl_GetString(objv[3])) != NS_OK) {
	        return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
	    }
	    Tcl_SetResult(interp, "0", TCL_STATIC);
            break;

        case INTERPRETSQLFILE:
            if (Ns_DbInterpretSqlFile(handlePtr, Tcl_GetString(objv[3])) != NS_OK) {
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;


    	case GETROW:
            if (Ns_TclGetSet2(interp, Tcl_GetString(objv[3]), &rowPtr) != TCL_OK) {
                return TCL_ERROR;
            }
            switch (Ns_DbGetRow(handlePtr, rowPtr)) {
            case NS_OK:
                Tcl_SetResult(interp, "1", TCL_STATIC);
                break;
            case NS_END_DATA:
                Tcl_SetResult(interp, "0", TCL_STATIC);
                break;
            default:
                return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
            }
            break;
        }
        break;

    case VERBOSE:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "dbId ?on|off?");
        }
	assert(handlePtr);
        if (objc == 4) {
            int verbose;
            if (Tcl_GetBoolean(interp, Tcl_GetString(objv[3]), &verbose) != TCL_OK) {
                return TCL_ERROR;
            }
            handlePtr->verbose = verbose;
        }
	Tcl_SetObjResult(interp, Tcl_NewIntObj(handlePtr->verbose));
        break;

    case SETEXCEPTION:
        if (objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "dbId code message");
        }
        if (strlen(Tcl_GetString(objv[3])) > 5) {
            Tcl_AppendResult(interp, "code \"", Tcl_GetString(objv[3]),
	        "\" more than 5 characters", NULL);
            return TCL_ERROR;
        }
        Ns_DbSetException(handlePtr, Tcl_GetString(objv[3]), Tcl_GetString(objv[4]));
        break;

    case SP_SETPARAM:
	{
	    char *arg5;

	    if (objc != 7) {
		Tcl_WrongNumArgs(interp, 2, objv, "dbId paramname type in|out value");
	    }
	    arg5 = Tcl_GetString(objv[5]);

	    if (!STREQ(arg5, "in") && !STREQ(arg5, "out")) {
		Tcl_SetResult(interp, "inout parameter of setparam must "
			      "be \"in\" or \"out\"", TCL_STATIC);
		return TCL_ERROR;
	    }
	    if (Ns_DbSpSetParam(handlePtr, Tcl_GetString(objv[3]), Tcl_GetString(objv[4]),
				arg5, Tcl_GetString(objv[6])) != NS_OK) {
		return DbFail(interp, handlePtr, Tcl_GetString(objv[1]));
	    } else {
		Tcl_SetResult(interp, "1", TCL_STATIC);
	    }
	    break;
	}
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 * DbErrorCodeCmd --
 *
 *      Get database exception code for the database handle.
 *
 * Results:
 *      Returns TCL_OK and database exception code is set as Tcl result
 *	or TCL_ERROR if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ErrorObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[], char cmd)
{
    InterpData *idataPtr = data;
    Ns_DbHandle *handle;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "dbId");
        return TCL_ERROR;
    }
    if (DbGetHandle(idataPtr, interp, Tcl_GetString(objv[1]), &handle, NULL) != TCL_OK) {
        return TCL_ERROR;
    }
    if (cmd == 'c') {
    	Tcl_SetResult(interp, handle->cExceptionCode, TCL_VOLATILE);
    } else {
    	Tcl_SetResult(interp, handle->dsExceptionMsg.string, TCL_VOLATILE);
    }
    return TCL_OK;
}

static int
DbErrorCodeObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return ErrorObjCmd(data, interp, objc, objv, 'c');
}

static int
DbErrorMsgObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return ErrorObjCmd(data, interp, objc, objv, 'm');
}


/*
 *----------------------------------------------------------------------
 * DbConfigPathCmd --
 *
 *      Get the database section name from the configuration file.
 *
 * Results:
 *      TCL_OK and the database section name is set as the Tcl result
 *	or TCL_ERROR if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
DbConfigPathObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    InterpData *idataPtr = data;
    char *section;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 0, objv, NULL);
    }
    section = Ns_ConfigGetPath(idataPtr->server, NULL, "db", NULL);
    Tcl_SetResult(interp, section, TCL_STATIC);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 * PoolDescriptionCmd --
 *
 *      Get the pool's description string.
 *
 * Results:
 *      Return TCL_OK and the pool's description string is set as the
 *	Tcl result string or TCL_ERROR if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
PoolDescriptionObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "poolname");
        return TCL_ERROR;
    }
    Tcl_SetResult(interp, Ns_DbPoolDescription(Tcl_GetString(objv[1])),TCL_STATIC);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 * QuoteListToListCmd --
 *
 *      Remove space, \ and ' characters in a string.
 *
 * Results:
 *      TCL_OK and set the stripped string as the Tcl result or TCL_ERROR
 *	if failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
QuoteListToListObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char       *quotelist;
    int         inquotes;
    Ns_DString  ds;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "quotelist");
        return TCL_ERROR;
    }
    quotelist = Tcl_GetString(objv[1]);
    inquotes = NS_FALSE;
    Ns_DStringInit(&ds);
    while (*quotelist != '\0') {
        if (isspace(UCHAR(*quotelist)) && inquotes == NS_FALSE) {
            if (ds.length != 0) {
                Tcl_AppendElement(interp, ds.string);
                Ns_DStringTrunc(&ds, 0);
            }
            while (isspace(UCHAR(*quotelist))) {
                quotelist++;
            }
        } else if (*quotelist == '\\' && (*(quotelist + 1) != '\0')) {
            Ns_DStringNAppend(&ds, quotelist + 1, 1);
            quotelist += 2;
        } else if (*quotelist == '\'') {
            if (inquotes) {
                /* Finish element */
                Tcl_AppendElement(interp, ds.string);
                Ns_DStringTrunc(&ds, 0);
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
        Tcl_AppendElement(interp, ds.string);
    }
    Ns_DStringFree(&ds);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * GetCsvCmd --
 *
 *	Implement the ns_getcvs command to read a line from a CSV file
 *	and parse the results into a Tcl list variable.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	One line is read for given open channel.
 *
 *----------------------------------------------------------------------
 */

static int
GetCsvObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    int             ncols, inquote, quoted, blank;
    char            c, *p, *delimiter = ",", *fileId, *varName;
    const char	   *result;
    Tcl_DString     line, cols, elem;
    Tcl_Channel	    chan;

    Ns_ObjvSpec opts[] = {
        {"-delimiter", Ns_ObjvString,   &delimiter, NULL},
        {"--",         Ns_ObjvBreak,    NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"fileId",     Ns_ObjvString, &fileId,   NULL},
        {"varName",    Ns_ObjvString, &varName,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (Ns_TclGetOpenChannel(interp, fileId, 0, 0, &chan) == TCL_ERROR) {
        return TCL_ERROR;
    }

    Tcl_DStringInit(&line);
    if (Tcl_Gets(chan, &line) < 0) {
	Tcl_DStringFree(&line);
    	if (!Tcl_Eof(chan)) {
	    Tcl_AppendResult(interp, "could not read from ", fileId, ": ", Tcl_PosixError(interp), NULL);
	    return TCL_ERROR;
	}
	Tcl_SetResult(interp, "-1", TCL_STATIC);
	return TCL_OK;
    }

    Tcl_DStringInit(&cols);
    Tcl_DStringInit(&elem);
    ncols = 0;
    inquote = 0;
    quoted = 0;
    blank = 1;
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
                    inquote = 0;
                    goto loopstart;
                }
            } else {
                Tcl_DStringAppend(&elem, &c, 1);
            }
        } else {
            if ((c == '\n') || (c == '\r')) {
                while ((c = *p++) != '\0') {
                    if ((c != '\n') && (c != '\r')) {
			--p;
                        break;
                    }
                }
                break;
            }
            if (c == '"') {
                inquote = 1;
                quoted = 1;
                blank = 0;
            } else if ((c == '\r') || (elem.length == 0 && isspace(UCHAR(c)))) {
                continue;
            } else if (strchr(delimiter,c) != NULL) {
                if (!quoted) {
                    Ns_StrTrimRight(elem.string);
                }
		Tcl_DStringAppendElement(&cols, elem.string);
                Tcl_DStringTrunc(&elem, 0);
                ncols++;
                quoted = 0;
            } else {
                blank = 0;
                Tcl_DStringAppend(&elem, &c, 1);
            }
        }
    }
    if (!quoted) {
        Ns_StrTrimRight(elem.string);
    }
    if (!blank) {
	Tcl_DStringAppendElement(&cols, elem.string);
        ncols++;
    }
    result = Tcl_SetVar(interp, varName, cols.string, TCL_LEAVE_ERR_MSG);
    Tcl_DStringFree(&line);
    Tcl_DStringFree(&cols);
    Tcl_DStringFree(&elem);
    if (result == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(ncols));

    return TCL_OK;
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
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
DbGetHandle(InterpData *idataPtr, Tcl_Interp *interp, char *id, Ns_DbHandle **handle,
	    Tcl_HashEntry **hPtrPtr)
{
    Tcl_HashEntry  *hPtr;

    hPtr = Tcl_FindHashEntry(&idataPtr->dbs, id);
    if (hPtr == NULL) {
	Tcl_AppendResult(interp, "invalid database id:  \"", id, "\"",
	    NULL);
	return TCL_ERROR;
    }
    *handle = (Ns_DbHandle *) Tcl_GetHashValue(hPtr);
    if (hPtrPtr != NULL) {
	*hPtrPtr = hPtr;
    }
    return TCL_OK;
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
EnterDbHandle(InterpData *idataPtr, Tcl_Interp *interp, Ns_DbHandle *handle)
{
    Tcl_HashEntry *hPtr;
    int            isNew, next;
    char	   buf[100];

    next = idataPtr->dbs.numEntries;
    do {
        snprintf(buf, sizeof(buf), "nsdb%x", next++);
        hPtr = Tcl_CreateHashEntry(&idataPtr->dbs, buf, &isNew);
    } while (!isNew);
    Tcl_AppendElement(interp, buf);
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
DbFail(Tcl_Interp *interp, Ns_DbHandle *handle, char *cmd)
{
    assert(handle);
    Tcl_AppendResult(interp, "Database operation \"", cmd, "\" failed", NULL);
    if (handle->cExceptionCode[0] != '\0') {
        Tcl_AppendResult(interp, " (exception ", handle->cExceptionCode, NULL);
        if (handle->dsExceptionMsg.length > 0) {
            Tcl_AppendResult(interp, ", \"", handle->dsExceptionMsg.string,
			     "\"", NULL);
        }
        Tcl_AppendResult(interp, ")", NULL);
    }
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
FreeData(ClientData arg, Tcl_Interp *interp)
{
    InterpData *idataPtr = arg;

    Tcl_DeleteHashTable(&idataPtr->dbs);
    ns_free(idataPtr);
}
