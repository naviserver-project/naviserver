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
    /* Fixed extension ID: preserve all legacy IDs, including DbFn_End,
     * whether deprecated callbacks are enabled or not. */
    DbFn_Batch = 24,
    DbFn_Bind = 25,
    DbFn_BindParameter = 26,
    DbFn_BindTyped = 27,
    DbFn_BindTypes = 28,
    DbFn_Stats = 29,
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

/* Optional bind vars in SQL statements, including explicit binary values.
 * Values are borrowed Tcl objects; NULL pointers
 * represent SQL NULL. Drivers must not retain these objects after returning. */
#define NS_DB_BIND_ABI 2
/* Optional marker factory for scalar and expanded slots. Return a new, unreferenced Tcl
 * string for a zero-based position. The format is fixed for this callback. */
#define NS_DB_BIND_PARAMETER_ABI 1
typedef Tcl_Obj *(Ns_DbBindParameterProc)(TCL_SIZE_T position);

typedef int (Ns_DbBindExecProc)(Tcl_Interp *, Ns_DbHandle *, const char *,
                              TCL_SIZE_T, Tcl_Obj *const *, Tcl_Obj *const *);
NS_EXTERN bool Ns_DbBindSupported(const Ns_DbHandle *);
NS_EXTERN int Ns_DbBindExec(Tcl_Interp *, Ns_DbHandle *, const char *, TCL_SIZE_T,
                            Tcl_Obj *const *, Tcl_Obj *const *);

/* Preferred callback for both text and explicit binary values. A NULL types
 * array means all text. Registering only DbFn_BindTyped is sufficient; core
 * retains DbFn_Bind for existing text-only drivers. Callback ABIs are unchanged. */
typedef enum {
    NS_DB_BIND_TEXT, NS_DB_BIND_BINARY, NS_DB_BIND_ARRAY
} Ns_DbBindValueType;
typedef int (Ns_DbBindTypedExecProc)(Tcl_Interp *, Ns_DbHandle *, const char *,
                                   TCL_SIZE_T, Tcl_Obj *const *, Tcl_Obj *const *,
                                   const Ns_DbBindValueType *);
NS_EXTERN int Ns_DbBindTypedExec(Tcl_Interp *, Ns_DbHandle *, const char *, TCL_SIZE_T,
                                 Tcl_Obj *const *, Tcl_Obj *const *, const Ns_DbBindValueType *);

/* ABI 2 adds an optional supported-type mask and prepared array access.
 * Absent a mask callback, typed drivers support text/binary, text-only drivers
 * support text, and neither can receive an unknown array representation. */
typedef unsigned (Ns_DbBindTypesProc)(const Ns_DbHandle *);
NS_EXTERN unsigned Ns_DbBindSupportedTypes(const Ns_DbHandle *) NS_GNUC_NONNULL(1);
NS_EXTERN int Ns_DbBindArrayGet(Tcl_Interp *, Tcl_Obj *, TCL_SIZE_T *, Tcl_Obj *const **, char *)
    NS_GNUC_NONNULL(1,2,3,4);

/* Optional, observational per-handle statistics. The callback returns a Tcl
 * dictionary in interp. A NULL database selects the driver's default scope.
 * Database names and the handle are borrowed for the duration of the call.
 * Shared scalar metrics belong in connection/statement, with meaningful peaks
 * under highwater at matching paths. Omit unavailable metrics and empty groups.
 * Driver-specific fields belong in backend/DRIVER. Callbacks must preserve
 * pending results, transactions and error diagnostics. See ns_db statistics
 * documentation for metric names, units and scope. */
#define NS_DB_STATS_ABI 1
typedef int (Ns_DbStatsProc)(Tcl_Interp *, Ns_DbHandle *, const char *);
NS_EXTERN int Ns_DbStats(Tcl_Interp *interp, Ns_DbHandle *handle, const char *database)
    NS_GNUC_NONNULL(1, 2);

NS_EXTERN Ns_ReturnCode Ns_DbRegisterDriver(const char *driver, const Ns_DbProc *procs);
NS_EXTERN char         *Ns_DbDriverName(Ns_DbHandle *handle);
NS_EXTERN char         *Ns_DbDriverDbType(Ns_DbHandle *handle);
NS_EXTERN Tcl_Obj      *Ns_DbDriverVersionInfo(Ns_DbHandle *handle)       NS_GNUC_NONNULL(1);
NS_EXTERN int           Ns_DbDML(Ns_DbHandle *handle, const char *sql)    NS_GNUC_NONNULL(1,2);
NS_EXTERN Ns_Set       *Ns_DbSelect(Ns_DbHandle *handle, const char *sql) NS_GNUC_NONNULL(1,2);
NS_EXTERN int           Ns_DbExec(Ns_DbHandle *handle, const char *sql)   NS_GNUC_NONNULL(1,2);
NS_EXTERN Ns_Set       *Ns_DbBindRow(Ns_DbHandle *handle)                 NS_GNUC_NONNULL(1);
NS_EXTERN int           Ns_DbGetRow(Ns_DbHandle *handle, Ns_Set *row)     NS_GNUC_NONNULL(1,2);
NS_EXTERN int           Ns_DbGetRowCount(Ns_DbHandle *handle)             NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbFlush(Ns_DbHandle *handle)                   NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbCancel(Ns_DbHandle *handle)                  NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbResetHandle(Ns_DbHandle *handle)             NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbSpStart(Ns_DbHandle *handle, const char *procname) NS_GNUC_NONNULL(1,2);
NS_EXTERN Ns_ReturnCode Ns_DbSpSetParam(Ns_DbHandle *handle, const char *paramname,
                                        const char *paramtype, const char *direction, const char *value)
    NS_GNUC_NONNULL(1,2,3,4,5);
NS_EXTERN int           Ns_DbSpExec(Ns_DbHandle *handle)                  NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_DbSpReturnCode(Ns_DbHandle *handle, const char *returnCode, int bufsize)
    NS_GNUC_NONNULL(1,2);
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
    NS_GNUC_NONNULL(1,2);

NS_EXTERN void          Ns_DbPoolPutHandle(Ns_DbHandle *handle)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_DbHandle  *Ns_DbPoolTimedGetHandle(const char *pool, const Ns_Time *wait)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_DbHandle  *Ns_DbPoolGetHandle(const char *pool)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode Ns_DbPoolGetMultipleHandles(Ns_DbHandle **handles,
                                                    const char *pool, int nwant)
    NS_GNUC_NONNULL(1,2);
NS_EXTERN Ns_ReturnCode Ns_DbPoolTimedGetMultipleHandles(Ns_DbHandle **handles,
                                                         const char *pool,
                                                         int nwant, const Ns_Time *wait)
    NS_GNUC_NONNULL(1,2);

Ns_ReturnCode Ns_DbPoolCurrentHandles(int *countPtr, const char *pool)
    NS_GNUC_NONNULL(1,2);

bool NsDbGetActive(Ns_DbHandle *handle) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

void NsDbSetActive(const char *context, Ns_DbHandle *handle, bool active)
    NS_GNUC_NONNULL(1,2);

NS_EXTERN Ns_ReturnCode Ns_DbBouncePool(const char *pool)
    NS_GNUC_NONNULL(1);

NS_EXTERN int Ns_DbPoolStats(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Obj *Ns_DbListMinDurations(Tcl_Interp *interp, const char *server)
    NS_GNUC_NONNULL(1,2);

NS_EXTERN int Ns_DbGetMinDuration(Tcl_Interp *interp, const char *pool, Ns_Time **minDuration)
    NS_GNUC_NONNULL(2,3);

NS_EXTERN int Ns_DbSetMinDuration(Tcl_Interp *interp, const char *pool, const Ns_Time *minDuration)
    NS_GNUC_NONNULL(1,2,3);

/*
 * dbtcl.c:
 */

NS_EXTERN int Ns_TclDbGetHandle(Tcl_Interp *interp, const char *handleId,
                                Ns_DbHandle **handlePtr);

/*
 * dbutil.c:
 */

NS_EXTERN void Ns_DbQuoteValue(Tcl_DString *dsPtr, const char *chars)
    NS_GNUC_NONNULL(1,2);

NS_EXTERN Ns_Set *Ns_Db0or1Row(Ns_DbHandle *handle, const char *sql, int *nrows)
    NS_GNUC_NONNULL(1,2,3);

NS_EXTERN Ns_Set *Ns_Db1Row(Ns_DbHandle *handle, const char *sql)
    NS_GNUC_NONNULL(1,2);

NS_EXTERN Ns_ReturnCode Ns_DbInterpretSqlFile(Ns_DbHandle *handle, const char *filename)
    NS_GNUC_NONNULL(1,2);

NS_EXTERN void Ns_DbSetException(Ns_DbHandle *handle, const char *code, const char *msg)
    NS_GNUC_NONNULL(1,2,3);

#include "batch.h"

#endif /* NSDB_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
