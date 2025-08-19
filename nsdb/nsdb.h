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
 * nsdb.h --
 *
 *      Public types and function declarations for the nsdb module.
 *
 */

#ifndef NSDB_H
#define NSDB_H

#include "ns.h"

/*
 * Nsdb return codes, extending NaviServer return codes.
 */
typedef enum {
    NS_DML =              ( 1),
    NS_ROWS =             ( 2),
    NS_END_DATA =         ( 4),
    NS_NO_DATA =          ( 8),
    NSDB_OK =             (NS_OK), /* success */
    NSDB_ERROR =          (NS_ERROR) /* error */
} NsDb_ReturnCode;

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
    DbFn_Exec,
    DbFn_BindRow,
    DbFn_ResetHandle,
    DbFn_SpStart,
    DbFn_SpSetParam,
    DbFn_SpExec,
    DbFn_SpReturnCode,
    DbFn_SpGetParams,
    DbFn_GetRowCount,
    DbFn_Version,
#ifdef NS_WITH_DEPRECATED
    DbFn_GetTableInfo,
    DbFn_TableList,
    DbFn_BestRowId,
#endif
    DbFn_End,
} Ns_DbProcId;

/*
 * Database procedure structure used when registering
 * a driver.
 */

typedef struct Ns_DbProc {
    Ns_DbProcId  id;
    ns_funcptr_t func;
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
    bool        verbose; /* Was previously used for general verbosity, then unused, now verboseError */
    Ns_Set     *row;
    char        cExceptionCode[6];
    Tcl_DString dsExceptionMsg;
    void       *context;
    void       *statement;
    bool        fetchingRows;
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

#ifndef NS_DBTCL_C
#ifdef __MINGW32__
NS_EXTERN Ns_LogSeverity Ns_LogSqlDebug;
#else 
extern NS_IMPORT Ns_LogSeverity Ns_LogSqlDebug;
#endif
#endif

typedef Ns_ReturnCode (NsDb_DriverInitProc)(const char *driver, const char *configPath);
NS_EXTERN const char *NS_EMPTY_STRING;


/*
 * dbdrv.c:
 */

NS_EXTERN Ns_ReturnCode Ns_DbRegisterDriver(const char *driver, const Ns_DbProc *procs);
NS_EXTERN char         *Ns_DbDriverName(Ns_DbHandle *handle);
NS_EXTERN char         *Ns_DbDriverDbType(Ns_DbHandle *handle);
NS_EXTERN Tcl_Obj      *Ns_DbDriverVersionInfo(Ns_DbHandle *handle)       NS_GNUC_NONNULL(1);
NS_EXTERN int           Ns_DbDML(Ns_DbHandle *handle, const char *sql)    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_Set       *Ns_DbSelect(Ns_DbHandle *handle, const char *sql) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN int           Ns_DbExec(Ns_DbHandle *handle, const char *sql)   NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_Set       *Ns_DbBindRow(Ns_DbHandle *handle)                 NS_GNUC_NONNULL(1);
NS_EXTERN int           Ns_DbGetRow(Ns_DbHandle *handle, Ns_Set *row)     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN int           Ns_DbGetRowCount(Ns_DbHandle *handle)             NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbFlush(Ns_DbHandle *handle)                   NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbCancel(Ns_DbHandle *handle)                  NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbResetHandle(Ns_DbHandle *handle)             NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbSpStart(Ns_DbHandle *handle, const char *procname) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_ReturnCode Ns_DbSpSetParam(Ns_DbHandle *handle, const char *paramname,
                                        const char *paramtype, const char *direction, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
NS_EXTERN int           Ns_DbSpExec(Ns_DbHandle *handle)                  NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbSpReturnCode(Ns_DbHandle *handle, const char *returnCode, int bufsize)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_Set       *Ns_DbSpGetParams(Ns_DbHandle *handle)             NS_GNUC_NONNULL(1);

/*
 * dbinit.c:
 */

NS_EXTERN const char   *Ns_DbPoolDescription(const char *pool)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char   *Ns_DbPoolDefault(const char *server)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char   *Ns_DbPoolList(const char *server)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool          Ns_DbPoolAllowable(const char *server, const char *pool)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void          Ns_DbPoolPutHandle(Ns_DbHandle *handle)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_DbHandle  *Ns_DbPoolTimedGetHandle(const char *pool, const Ns_Time *wait)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_DbHandle  *Ns_DbPoolGetHandle(const char *pool)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode Ns_DbPoolGetMultipleHandles(Ns_DbHandle **handles,
                                                    const char *pool, int nwant)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_ReturnCode Ns_DbPoolTimedGetMultipleHandles(Ns_DbHandle **handles,
                                                         const char *pool,
                                                         int nwant, const Ns_Time *wait)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

Ns_ReturnCode Ns_DbPoolCurrentHandles(int *countPtr, const char *pool)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

bool NsDbGetActive(Ns_DbHandle *handle) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

void NsDbSetActive(const char *context, Ns_DbHandle *handle, bool active)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode Ns_DbBouncePool(const char *pool)
    NS_GNUC_NONNULL(1);

NS_EXTERN int Ns_DbPoolStats(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Obj *Ns_DbListMinDurations(Tcl_Interp *interp, const char *server)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int Ns_DbGetMinDuration(Tcl_Interp *interp, const char *pool, Ns_Time **minDuration)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

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

NS_EXTERN void Ns_DbQuoteValue(Tcl_DString *dsPtr, const char *chars)
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

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
