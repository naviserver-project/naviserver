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
 * Copyright (C) 2005 Stephen Deasey.
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
 *
 */

/*
 * nsdbtest.c --
 *
 *      Implements a stub nsdb driver for testing.
 */

#include "../nsdb/nsdb.h"

NS_EXPORT const int Ns_ModuleVersion = 1;


/*
 * Local functions defined in this file.
 */

static char   *DbType(Ns_DbHandle *handle);
static int     OpenDb(Ns_DbHandle *handle);
static int     CloseDb(Ns_DbHandle *handle);
static Ns_Set *BindRow(Ns_DbHandle *handle);
static int     Exec(const Ns_DbHandle *handle, char *sql);
static int     GetRow(Ns_DbHandle *handle, const Ns_Set *row);
static int     Flush(Ns_DbHandle *handle);
static int     ResetHandle(Ns_DbHandle *handle);

/*
 * Local variables defined in this file.
 */

static const Ns_DbProc procs[] = {
    {DbFn_DbType,       (Ns_Callback *)DbType},
    {DbFn_Name,         (Ns_Callback *)DbType},
    {DbFn_OpenDb,       (Ns_Callback *)OpenDb},
    {DbFn_CloseDb,      (Ns_Callback *)CloseDb},
    {DbFn_BindRow,      (Ns_Callback *)BindRow},
    {DbFn_Exec,         (Ns_Callback *)Exec},
    {DbFn_GetRow,       (Ns_Callback *)GetRow},
    {DbFn_Flush,        (Ns_Callback *)Flush},
    {DbFn_Cancel,       (Ns_Callback *)Flush},
    {DbFn_ResetHandle,  (Ns_Callback *)ResetHandle},
    {0, NULL}
};

static char *dbName = "nsdbtest";


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

NS_EXPORT int
Ns_DbDriverInit(char *driver, const char *UNUSED(configPath))
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

static char *
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

static int
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

static int
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
 *      An Ns_Set whos keys are the names of columns, or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_Set *
BindRow(Ns_DbHandle *handle)
{
    Ns_SetPut(handle->row, "column1", NULL);
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
    if (handle->verbose != 0) {
        Ns_Log(Notice, "nsdbtest(%s): Querying '%s'", handle->driver, sql);
    }

    if (STRIEQ(sql, "rows")) {
        return NS_ROWS;
    } else if (STRIEQ(sql, "dml")) {
        return NS_DML;
    }
    return NS_ERROR;
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
 *      Current tupple updated.
 *
 *----------------------------------------------------------------------
 */

static int
GetRow(Ns_DbHandle *UNUSED(handle), const Ns_Set *row)
{
    Ns_SetPutValue(row, 0U, "ok");

    return NS_END_DATA;
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

static int
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

static int
ResetHandle(Ns_DbHandle *UNUSED(handle))
{
    return NS_OK;
}
