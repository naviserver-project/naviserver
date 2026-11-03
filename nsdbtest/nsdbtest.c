/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2005 Stephen Deasey.
 *
 *
 */

/*
 * nsdbtest.c --
 *
 *      Implements a stub nsdb driver for testing.
 */

#include "../nsdb/nsdb.h"

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

NS_EXPORT NsDb_DriverInitProc Ns_DbDriverInit;

/*
 * Local functions defined in this file.
 */

static const char    *DbType(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static Ns_ReturnCode  OpenDb(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static Ns_ReturnCode  CloseDb(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static Ns_Set        *BindRow(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static int            Exec(const Ns_DbHandle *handle, char *sql) NS_GNUC_NONNULL(1,2);
static int            GetRow(Ns_DbHandle *handle, Ns_Set *row) NS_GNUC_NONNULL(1,2);
static Ns_ReturnCode  Flush(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static Ns_ReturnCode  ResetHandle(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static Ns_DbStatsProc Stats NS_GNUC_NONNULL(1, 2);
static Ns_DbBindExecProc TextBind NS_GNUC_NONNULL(1, 2, 3);
static Ns_DbBindTypedExecProc TypedBind NS_GNUC_NONNULL(1, 2, 3);

/*
 * Local variables defined in this file.
 */

static const Ns_DbProc procs[] = {
    {DbFn_DbType,       (ns_funcptr_t)DbType},
    {DbFn_Name,         (ns_funcptr_t)DbType},
    {DbFn_OpenDb,       (ns_funcptr_t)OpenDb},
    {DbFn_CloseDb,      (ns_funcptr_t)CloseDb},
    {DbFn_BindRow,      (ns_funcptr_t)BindRow},
    {DbFn_Exec,         (ns_funcptr_t)Exec},
    {DbFn_GetRow,       (ns_funcptr_t)GetRow},
    {DbFn_Flush,        (ns_funcptr_t)Flush},
    {DbFn_Cancel,       (ns_funcptr_t)Flush},
    {DbFn_ResetHandle,  (ns_funcptr_t)ResetHandle},
    {(Ns_DbProcId)0, NULL}
};

static const char *const dbName = "nsdbtest";


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbDriverInit --
 *
 *      Register driver functions.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_DbDriverInit(const char *driver, const char *configPath)
{
    const char *mode = Ns_ConfigGetValue(configPath, "bindMode");
    const char *stats = Ns_ConfigGetValue(configPath, "statsMode");

    if (mode != NULL || stats != NULL) {
        Ns_DbProc callbacks[sizeof(procs) / sizeof(procs[0]) + 3u];
        size_t count = sizeof(procs) / sizeof(procs[0]) - 1u;

        memcpy(callbacks, procs, sizeof(procs));
        if (mode != NULL && (STREQ(mode, "text") || STREQ(mode, "both"))) {
            callbacks[count++] = (Ns_DbProc){DbFn_Bind, (ns_funcptr_t)TextBind};
        }
        if (mode != NULL && (STREQ(mode, "typed") || STREQ(mode, "both"))) {
            callbacks[count++] = (Ns_DbProc){DbFn_BindTyped, (ns_funcptr_t)TypedBind};
        }
        if (stats != NULL) {
            callbacks[count++] = (Ns_DbProc){DbFn_Stats, (ns_funcptr_t)Stats};
        }
        callbacks[count] = (Ns_DbProc){DbFn_End, NULL};
        return Ns_DbRegisterDriver(driver, callbacks);
    }
    return Ns_DbRegisterDriver(driver, procs);
}

/*
 *----------------------------------------------------------------------
 *
 * Stats --
 *
 *      Supply isolated statistics callback probes, without executing SQL.
 *
 * Results:
 *      A dictionary, an intentional driver error, or a malformed result,
 *      selected by the fixture datasource.
 *
 * Side effects:
 *      Set the Tcl result and, for the error probe, its error code.
 *
 *----------------------------------------------------------------------
 */

static int
Stats(Tcl_Interp *interp, Ns_DbHandle *handle, const char *database)
{
    Tcl_Obj *resultObj;

    if (STREQ(handle->datasource, "error")) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("statistics fixture error", TCL_INDEX_NONE));
        Tcl_SetErrorCode(interp, "NSDB", "STATS", "FIXTURE", (char *)0L);
        return TCL_ERROR;
    }
    if (STREQ(handle->datasource, "invalid")) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("not a dictionary", TCL_INDEX_NONE));
        return TCL_OK;
    }
    resultObj = Tcl_NewDictObj();
    {
        Tcl_Obj *connection = Tcl_NewDictObj(), *backend = Tcl_NewDictObj();
        Tcl_Obj *specific = Tcl_NewDictObj();

        Tcl_DictObjPut(NULL, connection, Tcl_NewStringObj("cacheHits", TCL_INDEX_NONE), Tcl_NewIntObj(17));
        Tcl_DictObjPut(NULL, specific, Tcl_NewStringObj("database", TCL_INDEX_NONE),
                       Tcl_NewStringObj(database != NULL ? database : "default", TCL_INDEX_NONE));
        Tcl_DictObjPut(NULL, backend, Tcl_NewStringObj("nsdbtest", TCL_INDEX_NONE), specific);
        Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("connection", TCL_INDEX_NONE), connection);
        Tcl_DictObjPut(NULL, resultObj, Tcl_NewStringObj("backend", TCL_INDEX_NONE), backend);
    }
    Tcl_SetObjResult(interp, resultObj);
    return TCL_OK;
}

/*
 * Test-only bind callbacks deliberately fail for the "callback" probe so Tcl
 * tests can identify which callback core selected. Ordinary probes return DML.
 */
static int
TextBind(Tcl_Interp *interp, Ns_DbHandle *UNUSED(handle), const char *sql,
         TCL_SIZE_T UNUSED(count), Tcl_Obj *const *UNUSED(names), Tcl_Obj *const *UNUSED(values))
{
    if (strncmp(sql, "callback", 8u) == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("text callback", TCL_INDEX_NONE));
        return NS_ERROR;
    }
    return NS_DML;
}

static int
TypedBind(Tcl_Interp *interp, Ns_DbHandle *UNUSED(handle), const char *sql,
          TCL_SIZE_T UNUSED(count), Tcl_Obj *const *UNUSED(names), Tcl_Obj *const *UNUSED(values),
          const Ns_DbBindValueType *types)
{
    if (strncmp(sql, "callback", 8u) == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("typed callback", TCL_INDEX_NONE));
        return NS_ERROR;
    }
    if (strcmp(sql, "null-types") == 0 && types != NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("expected default text types", TCL_INDEX_NONE));
        return NS_ERROR;
    }
    return NS_DML;
}



/*
 *----------------------------------------------------------------------
 *
 * DbType --
 *
 *      Return a string which identifies the driver type and name.
 *
 * Results:
 *      Database type/name.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static const char *
DbType(Ns_DbHandle *UNUSED(handle))
{
    return dbName;
}


/*
 *----------------------------------------------------------------------
 *
 * OpenDb --
 *
 *      Open a connection.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
OpenDb(Ns_DbHandle *UNUSED(handle))
{
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * CloseDb --
 *
 *      Close an open connection.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CloseDb(Ns_DbHandle *UNUSED(handle))
{
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * BindRow --
 *
 *      Retrieve the column names of the current result.
 *
 * Results:
 *      An Ns_Set which keys are the names of columns, or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_Set *
BindRow(Ns_DbHandle *handle)
{
    (void)Ns_SetPutSz(handle->row, "column1", 7, NULL, 0);
    handle->fetchingRows = NS_FALSE;

    return handle->row;
}


/*
 *----------------------------------------------------------------------
 *
 * Exec --
 *
 *      Execute an SQL statement.
 *
 * Results:
 *      NS_ROWS, NS_DML or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Exec(const Ns_DbHandle *handle, char *sql)
{
    int result;

    if (handle->verbose) {
        Ns_Log(Notice, "nsdbtest(%s): Querying '%s'", handle->driver, sql);
    }

    if (STRIEQ(sql, "rows")) {
        result = (int)NS_ROWS;
    } else if (STRIEQ(sql, "dml")) {
        result = (int)NS_DML;
    } else {
        result = (int)NS_ERROR;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GetRow --
 *
 *      Fill in the given Ns_Set with values for each column of the
 *      current row.
 *
 * Results:
 *      NS_OK, NS_END_DATA or NS_ERROR.
 *
 * Side effects:
 *      Current tuple updated.
 *
 *----------------------------------------------------------------------
 */

static int
GetRow(Ns_DbHandle *UNUSED(handle), Ns_Set *row)
{
    int result;

    if (Ns_SetValue(row, 0) == NULL) {
        Ns_SetPutValueSz(row, 0u, "ok", 2);
        result = (int)NS_OK;
    } else {
        result = (int)NS_END_DATA;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Flush --
 *
 *      Flush unfetched rows.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
Flush(Ns_DbHandle *UNUSED(handle))
{
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ResetHandle --
 *
 *      Reset connection ready for next command.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Any active transaction will be rolled back.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
ResetHandle(Ns_DbHandle *UNUSED(handle))
{
    return NS_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
