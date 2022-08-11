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
 * dbutil.c --
 *
 *      Utility db routines.
 */

#include "db.h"

/*
 * The following constants are defined for this file.
 */

#define NS_SQLERRORCODE "NSINT" /* SQL error code for NaviServer exceptions. */


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbQuoteValue --
 *
 *      Add single quotes around an SQL string value if necessary.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Copy of the string, modified if needed, is placed in the
 *      given Ns_DString.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DbQuoteValue(Ns_DString *dsPtr, const char *chars)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(chars != NULL);

    while (*chars != '\0') {
        if (*chars == '\'') {
            Ns_DStringNAppend(dsPtr, "'", 1);
        }
        Ns_DStringNAppend(dsPtr, chars, 1);
        ++chars;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Db0or1Row --
 *
 *      Send an SQL statement which should return either no rows or
 *      exactly one row.
 *
 * Results:
 *      Pointer to new Ns_Set which must be eventually freed.  The
 *      set includes the names of the columns and, if a row was
 *      fetched, the values for the row.  On error, returns NULL.
 *
 * Side effects:
 *      Given nrows pointer is set to 0 or 1 to indicate if a row
 *      was actually returned.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_Db0or1Row(Ns_DbHandle *handle, const char *sql, int *nrows)
{
    Ns_Set *row;

    NS_NONNULL_ASSERT(handle != NULL);
    NS_NONNULL_ASSERT(sql != NULL);
    NS_NONNULL_ASSERT(nrows != NULL);

    row = Ns_DbSelect(handle, sql);
    if (row != NULL) {
        bool success = NS_TRUE;

        if (Ns_DbGetRow(handle, row) == NS_END_DATA) {
            *nrows = 0;
        } else {
            switch (Ns_DbGetRow(handle, row)) {
                case NS_END_DATA:
                    *nrows = 1;
                    break;

                case NS_OK:
                    Ns_DbSetException(handle, NS_SQLERRORCODE,
                        "Query returned more than one row.");
                    (void) Ns_DbFlush(handle);
                    NS_FALL_THROUGH; /* fall through */
                case NS_ERROR:
                    NS_FALL_THROUGH; /* fall through */
                default:
                    success = NS_FALSE;
                    row = NULL;
                    break;
            }
        }
        if (success) {
#ifdef NS_SET_DEBUG
            Ns_Log(Notice, "Ns_Db0or1Row Ns_SetCopy %p", (void*)row);
#endif
            row = Ns_SetCopy(row);
        }
    }

    return row;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Db1Row --
 *
 *      Send a SQL statement which is expected to return exactly 1 row.
 *
 * Results:
 *      Pointer to Ns_Set with row data or NULL on error.  Set must
 *      eventually be freed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_Db1Row(Ns_DbHandle *handle, const char *sql)
{
    Ns_Set         *row;
    int             nrows;

    NS_NONNULL_ASSERT(handle != NULL);
    NS_NONNULL_ASSERT(sql != NULL);

    row = Ns_Db0or1Row(handle, sql, &nrows);
    if (row != NULL) {
        if (nrows != 1) {
            Ns_DbSetException(handle, NS_SQLERRORCODE,
                "Query did not return a row.");
            row = NULL;
        }
    }

    return row;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbInterpretSqlFile --
 *
 *      Parse DML statements from an SQL file and send them to the
 *      database for execution.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Stops on first error.  Transaction protection is provided for
 *      Illustra and "\n-- comments are handled correctly.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_DbInterpretSqlFile(Ns_DbHandle *handle, const char *filename)
{
    FILE           *fp;
    Ns_DString      dsSql;
    int             i, inquote;
    Ns_ReturnCode   status;
    char            c, lastc;

    NS_NONNULL_ASSERT(handle != NULL);
    NS_NONNULL_ASSERT(filename != NULL);

    fp = fopen(filename,
               "rt"
#ifdef NS_FOPEN_SUPPORTS_MODE_E
               "e"
#endif
               );
    if (fp == NULL) {
        Ns_DbSetException(handle, NS_SQLERRORCODE,
            "Could not read file");
        return NS_ERROR;
    }

    Ns_DStringInit(&dsSql);
    status = NS_OK;
    inquote = 0;
    c = '\n';
    while ((i = getc(fp)) != EOF) {
        lastc = c;
        c = (char) i;
 loopstart:
        if (inquote != 0) {
            if (c != '\'') {
                Ns_DStringNAppend(&dsSql, &c, 1);
            } else {
              i = getc(fp);
                if (i == EOF) {
                    break;
                }
                lastc = c;
                c = (char) i;
                if (c == '\'') {
                    Ns_DStringNAppend(&dsSql, "''", 2);
                    continue;
                } else {
                    Ns_DStringNAppend(&dsSql, "'", 1);
                    inquote = 0;
                    goto loopstart;
                }
            }
        } else {
            /* Check to see if it is a comment */
            if ((c == '-') && (lastc == '\n')) {
                i = getc(fp);
                if (i == EOF) {
                    break;
                }
                lastc = c;
                c = (char) i;
                if (c != '-') {
                    Ns_DStringNAppend(&dsSql, "-", 1);
                    goto loopstart;
                }
                while ((i = getc(fp)) != EOF) {
                    /*lastc = c; never used */
                    c = (char) i;
                    if (c == '\n') {
                        break;
                    }
                }
            } else if (c == ';') {
                if (Ns_DbExec(handle, dsSql.string) == NS_ERROR) {
                    status = NS_ERROR;
                    break;
                }
                Ns_DStringSetLength(&dsSql, 0);
            } else {
                Ns_DStringNAppend(&dsSql, &c, 1);
                if (c == '\'') {
                    inquote = 1;
                }
            }
        }
    }
    fclose(fp);

    /*
     * If dstring contains anything but whitespace, return error
     */
    if (status != NS_ERROR) {
        const char *p;

        for (p = dsSql.string; *p != '\0'; p++) {
            if (CHARTYPE(space, *p) == 0) {
                Ns_DbSetException(handle, NS_SQLERRORCODE,
                    "File ends with unterminated SQL");
                status = NS_ERROR;
            }
        }
    }
    Ns_DStringFree(&dsSql);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DbSetException --
 *
 *      Set the stored SQL exception code and message in the handle.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Code and message are updated.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DbSetException(Ns_DbHandle *handle, const char *code, const char *msg)
{
    NS_NONNULL_ASSERT(handle != NULL);
    NS_NONNULL_ASSERT(code != NULL);
    NS_NONNULL_ASSERT(msg != NULL);

    handle->cExceptionCode[0] = '\0';
    strncat(handle->cExceptionCode, code, sizeof(handle->cExceptionCode) - 1);
    Ns_DStringFree(&(handle->dsExceptionMsg));
    Ns_DStringAppend(&(handle->dsExceptionMsg), msg);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
