/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
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
 * nsdb.h --
 *
 *      Public types and function declarations for the nsdb module.
 *
 */

#ifndef NSDB_H
#define NSDB_H

#include "ns.h"

/*
 * The following are nsdb return codes.
 */

#define NS_DML  		  1
#define NS_ROWS 		  2
#define NS_END_DATA 		  4
#define NS_NO_DATA 		  8

/* 
 * The following enum defines known nsdb driver function ids.
 */

typedef enum {
    DbFn_Name,
    DbFn_DbType,
    DbFn_ServerInit,
    DbFn_OpenDb,
    DbFn_CloseDb,
    DbFn_DML,
    DbFn_Select,
    DbFn_GetRow,
    DbFn_Flush,
    DbFn_Cancel,
    DbFn_GetTableInfo,
    DbFn_TableList,
    DbFn_BestRowId,
    DbFn_Exec,
    DbFn_BindRow,
    DbFn_ResetHandle,
    DbFn_SpStart,
    DbFn_SpSetParam,
    DbFn_SpExec,
    DbFn_SpReturnCode,
    DbFn_SpGetParams,
    DbFn_GetRowCount,
    DbFn_End
} Ns_DbProcId;

/*
 * Database procedure structure used when registering
 * a driver. 
 */

typedef struct Ns_DbProc {
    Ns_DbProcId  id;
    Ns_Callback *func;
} Ns_DbProc;

/*
 * Database handle structure.
 */

typedef struct Ns_DbHandle {
    const char *driver;
    const char *datasource;
    const char *user;
    const char *password;
    void       *connection;
    const char *poolname;
    bool        connected;
    bool        verbose;
    Ns_Set     *row;
    char        cExceptionCode[6];
    Ns_DString  dsExceptionMsg;
    void       *context;
    void       *statement;
    int         fetchingRows;
} Ns_DbHandle;

/*
 * The following structure is no longer supported and only provided to
 * allow existing database modules to compile.  All of the TableInfo
 * routines now log an unsupported use error and return an error result.
 */

typedef struct {
    Ns_Set  *table;
    int      size;
    int      ncolumns;
    Ns_Set **columns;
} Ns_DbTableInfo;

NS_EXTERN Ns_LogSeverity Ns_LogSqlDebug;

/*
 * dbdrv.c:
 */

NS_EXTERN Ns_ReturnCode Ns_DbRegisterDriver(const char *driver, const Ns_DbProc *procs);
NS_EXTERN char         *Ns_DbDriverName(Ns_DbHandle *handle);
NS_EXTERN char         *Ns_DbDriverDbType(Ns_DbHandle *handle);
NS_EXTERN int           Ns_DbDML(Ns_DbHandle *handle, const char *sql);
NS_EXTERN Ns_Set       *Ns_DbSelect(Ns_DbHandle *handle, const char *sql);
NS_EXTERN int           Ns_DbExec(Ns_DbHandle *handle, const char *sql);
NS_EXTERN Ns_Set       *Ns_DbBindRow(Ns_DbHandle *handle);
NS_EXTERN int           Ns_DbGetRow(Ns_DbHandle *handle, Ns_Set *row);
NS_EXTERN int           Ns_DbGetRowCount(Ns_DbHandle *handle);
NS_EXTERN Ns_ReturnCode Ns_DbFlush(Ns_DbHandle *handle);
NS_EXTERN Ns_ReturnCode Ns_DbCancel(Ns_DbHandle *handle);
NS_EXTERN Ns_ReturnCode Ns_DbResetHandle(Ns_DbHandle *handle);
NS_EXTERN Ns_ReturnCode Ns_DbSpStart(Ns_DbHandle *handle, const char *procname);
NS_EXTERN Ns_ReturnCode Ns_DbSpSetParam(Ns_DbHandle *handle, const char *paramname,
					const char *paramtype, const char *inout, const char *value);
NS_EXTERN int           Ns_DbSpExec(Ns_DbHandle *handle);
NS_EXTERN Ns_ReturnCode Ns_DbSpReturnCode(Ns_DbHandle *handle, const char *returnCode, int bufsize);
NS_EXTERN Ns_Set       *Ns_DbSpGetParams(Ns_DbHandle *handle);

/*
 * dbinit.c:
 */

NS_EXTERN const char   *Ns_DbPoolDescription(const char *pool) NS_GNUC_NONNULL(1);
NS_EXTERN const char   *Ns_DbPoolDefault(const char *server) NS_GNUC_NONNULL(1);
NS_EXTERN const char   *Ns_DbPoolList(const char *server) NS_GNUC_NONNULL(1);
NS_EXTERN bool          Ns_DbPoolAllowable(const char *server, const char *pool) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void          Ns_DbPoolPutHandle(Ns_DbHandle *handle) NS_GNUC_NONNULL(1);
NS_EXTERN Ns_DbHandle  *Ns_DbPoolTimedGetHandle(const char *pool, const Ns_Time *wait)  NS_GNUC_NONNULL(1);
NS_EXTERN Ns_DbHandle  *Ns_DbPoolGetHandle(const char *pool) NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbPoolGetMultipleHandles(Ns_DbHandle **handles, 
						    const char *pool,
						    int nwant)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_ReturnCode Ns_DbPoolTimedGetMultipleHandles(Ns_DbHandle **handles, 
							 const char *pool,
							 int nwant, const Ns_Time *wait)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_ReturnCode Ns_DbBouncePool(const char *pool) NS_GNUC_NONNULL(1);
NS_EXTERN int Ns_DbPoolStats(Tcl_Interp *interp) NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Obj *Ns_DbListMinDurations(Tcl_Interp *interp, const char *server)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN int Ns_DbGetMinDuration(Tcl_Interp *interp, const char *pool, Ns_Time **minDuration)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
NS_EXTERN int Ns_DbSetMinDuration(Tcl_Interp *interp, const char *pool, const Ns_Time *minDuration)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * dbtcl.c:
 */

NS_EXTERN int Ns_TclDbGetHandle(Tcl_Interp *interp, const char *handleId,
				Ns_DbHandle **handlePtr);

/*
 * dbutil.c:
 */
    
NS_EXTERN void Ns_DbQuoteValue(Ns_DString *dsPtr, const char *chars) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *Ns_Db0or1Row(Ns_DbHandle *handle, const char *sql, int *nrows)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_Set *Ns_Db1Row(Ns_DbHandle *handle, const char *sql)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode Ns_DbInterpretSqlFile(Ns_DbHandle *handle, const char *filename)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void Ns_DbSetException(Ns_DbHandle *handle, const char *code, const char *msg) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

#endif /* NSDB_H */
