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
static int            Exec(const Ns_DbHandle *handle, char *sql) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int            GetRow(Ns_DbHandle *handle, Ns_Set *row) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Ns_ReturnCode  Flush(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
static Ns_ReturnCode  ResetHandle(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);

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
Ns_DbDriverInit(const char *driver, const char *UNUSED(configPath))
{
    return Ns_DbRegisterDriver(driver, &procs[0]);
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
