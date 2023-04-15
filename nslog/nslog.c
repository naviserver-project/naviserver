/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 */

/*
 * nslog.c --
 *
 *    Implements access logging in the NCSA Common Log format.
 *
 */

#include "ns.h"
#include <ctype.h>  /* isspace */

#define LOG_COMBINED      0x01u
#define LOG_FMTTIME       0x02u
#define LOG_REQTIME       0x04u
#define LOG_PARTIALTIMES  0x08u
#define LOG_CHECKFORPROXY 0x10u
#define LOG_SUPPRESSQUERY 0x20u
#define LOG_THREADNAME    0x40u
#define LOG_MASKIP        0x80u

#if !defined(PIPE_BUF)
# define PIPE_BUF 512
#endif

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;


typedef struct {
    Ns_Mutex     lock;
    const char  *module;
    const char  *filename;
    const char  *rollfmt;
    const char  *extendedHeaders;
    const char **requestHeaders;
    const char **responseHeaders;
    const char  *driverPattern;
    TCL_SIZE_T   maxbackup;
    int          nrRequestHeaders;
    int          nrResponseHeaders;
    int          fd;
    unsigned int flags;
    int          maxlines;
    int          curlines;
    struct NS_SOCKADDR_STORAGE  ipv4maskStruct;
    struct sockaddr            *ipv4maskPtr;
#ifdef HAVE_IPV6
    struct NS_SOCKADDR_STORAGE  ipv6maskStruct;
    struct sockaddr            *ipv6maskPtr;
#endif
    Tcl_DString   buffer;
} Log;

/*
 * Local functions defined in this file
 */

static Ns_SchedProc    LogRollCallback;
static Ns_ShutdownProc LogCloseCallback;
static Ns_TraceProc    LogTrace;
static Ns_ArgProc      LogArg;
static Ns_TclTraceProc AddCmds;
static TCL_OBJCMDPROC_T  LogObjCmd;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;

static Ns_ReturnCode LogFlush(Log *logPtr, Tcl_DString *dsPtr);
static Ns_LogCallbackProc LogOpen;
static Ns_LogCallbackProc LogClose;
static Ns_LogCallbackProc LogRoll;

static void AppendEscaped(Tcl_DString *dsPtr, const char *toProcess)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ReturnCode ParseExtendedHeaders(Log *logPtr, const char *str)
    NS_GNUC_NONNULL(1);
static void
AppendExtHeaders(Tcl_DString *dsPtr, const char **argv, const Ns_Set *set)
    NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Module initialization routine.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      Log file is opened, trace routine is registered, and, if
 *      configured, log file roll signal and scheduled procedures
 *      are registered.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    const char   *path, *file;
    Log          *logPtr;
    Tcl_DString   ds;
    static bool   first = NS_TRUE;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(module != NULL);

    /*
     * Sanity check to provide a meaningful error instead of a
     * crash. Currently, we do not allow one to register this module globally.
     */
    if (server == NULL) {
        Ns_Fatal("Module %s: requires a concrete server (cannot be used as a global module)",
                 module);
    }

    /*
     * Register the info callbacks just once. This assumes we are
     * called w/o locking from within the server startup.
     */

    if (first) {
        first = NS_FALSE;
        Ns_RegisterProcInfo((ns_funcptr_t)LogRollCallback, "nslog:roll", LogArg);
        Ns_RegisterProcInfo((ns_funcptr_t)LogCloseCallback, "nslog:close", LogArg);
        Ns_RegisterProcInfo((ns_funcptr_t)LogTrace, "nslog:conntrace", LogArg);
        Ns_RegisterProcInfo((ns_funcptr_t)AddCmds, "nslog:initinterp", LogArg);
    }

    Tcl_DStringInit(&ds);

    logPtr = ns_calloc(1u, sizeof(Log));
    logPtr->module = module;
    logPtr->fd = NS_INVALID_FD;
    Ns_MutexInit(&logPtr->lock);
    Ns_MutexSetName2(&logPtr->lock, "nslog", server);
    Tcl_DStringInit(&logPtr->buffer);

    path = Ns_ConfigSectionPath(NULL, server, module, (char *)0L);

    /*
     * Determine the name of the log file
     */

    file = Ns_ConfigString(path, "file", "access.log");
    if (Ns_PathIsAbsolute(file) == NS_TRUE) {
        logPtr->filename = ns_strdup(file);
    } else {
        /*
         * If log file is not given in absolute format, it is expected to
         * exist in the global logs directory if such exists or module
         * specific directory, which is created if necessary.
         */

        if (Ns_HomePathExists("logs", (char *)0L)) {
            (void) Ns_HomePath(&ds, "logs", "/", file, (char *)0L);
        } else {
            Tcl_Obj *dirpath;
            int      rc;

            Tcl_DStringSetLength(&ds, 0);
            (void) Ns_ModulePath(&ds, server, module, (char *)0L);
            dirpath = Tcl_NewStringObj(ds.string, TCL_INDEX_NONE);
            Tcl_IncrRefCount(dirpath);
            rc = Tcl_FSCreateDirectory(dirpath);
            Tcl_DecrRefCount(dirpath);
            if (rc != TCL_OK && Tcl_GetErrno() != EEXIST && Tcl_GetErrno() != EISDIR) {
                Ns_Log(Error, "nslog: create directory (%s) failed: '%s'",
                       ds.string, strerror(Tcl_GetErrno()));
                Tcl_DStringFree(&ds);
                return NS_ERROR;
            }
            Tcl_DStringSetLength(&ds, 0);
            (void) Ns_ModulePath(&ds, server, module, file, (char *)0L);
        }
        logPtr->filename = Ns_DStringExport(&ds);
    }

    /*
     * Get other parameters from configuration file
     */

    logPtr->rollfmt = ns_strcopy(Ns_ConfigGetValue(path, "rollfmt"));
    logPtr->maxbackup = (TCL_SIZE_T)Ns_ConfigIntRange(path, "maxbackup", 100, 1, INT_MAX);
    logPtr->maxlines = Ns_ConfigIntRange(path, "maxbuffer", 0, 0, INT_MAX);
    if (Ns_ConfigBool(path, "formattedtime", NS_TRUE)) {
        logPtr->flags |= LOG_FMTTIME;
    }
    if (Ns_ConfigBool(path, "logcombined", NS_TRUE)) {
        logPtr->flags |= LOG_COMBINED;
    }
    if (Ns_ConfigBool(path, "logreqtime", NS_FALSE)) {
        logPtr->flags |= LOG_REQTIME;
    }
    if (Ns_ConfigBool(path, "logpartialtimes", NS_FALSE)) {
        logPtr->flags |= LOG_PARTIALTIMES;
    }
    if (Ns_ConfigBool(path, "logthreadname", NS_FALSE)) {
        logPtr->flags |= LOG_THREADNAME;
    }
    if (Ns_ConfigBool(path, "suppressquery", NS_FALSE)) {
        logPtr->flags |= LOG_SUPPRESSQUERY;
    }
    if (Ns_ConfigBool(path, "checkforproxy", NS_FALSE)) {
        Ns_Log(Warning, "parameter checkforproxy of module nslog is deprecated; "
               "use global parameter reversproxymode instead");
        logPtr->flags |= LOG_CHECKFORPROXY;
    }

    logPtr->driverPattern = ns_strcopy(Ns_ConfigString(path, "driver", NULL));

    logPtr->ipv4maskPtr = NULL;
#ifdef HAVE_IPV6
    logPtr->ipv6maskPtr = NULL;
#endif
    if (Ns_ConfigBool(path, "masklogaddr", NS_FALSE)) {
        const char* maskString;
        const char *default_ipv4MaskString = "255.255.255.0";
#ifdef HAVE_IPV6
        const char *default_ipv6MaskString = "ff:ff:ff:ff::";
#endif
        logPtr->flags |= LOG_MASKIP;

#ifdef HAVE_IPV6
        maskString = Ns_ConfigGetValue(path, "maskipv6");
        if (maskString == NULL) {
            maskString = default_ipv6MaskString;
        }

        if (ns_inet_pton((struct sockaddr *)&logPtr->ipv6maskStruct, maskString) == 1) {
            logPtr->ipv6maskPtr = (struct sockaddr *)&logPtr->ipv6maskStruct;
        }
#endif
        maskString = Ns_ConfigGetValue(path, "maskipv4");
        if (maskString == NULL) {
            maskString = default_ipv4MaskString;
        }
        if (ns_inet_pton((struct sockaddr *)&logPtr->ipv4maskStruct, maskString) == 1) {
            logPtr->ipv4maskPtr = (struct sockaddr *)&logPtr->ipv4maskStruct;
        }
    }

    /*
     *  Schedule various log roll and shutdown options.
     */

    if (Ns_ConfigBool(path, "rolllog", NS_TRUE)) {
        int hour = Ns_ConfigIntRange(path, "rollhour", 0, 0, 23);

        Ns_ScheduleDaily(LogRollCallback, logPtr,
                         0, hour, 0, NULL);
    }
    if (Ns_ConfigBool(path, "rollonsignal", NS_FALSE)) {
        Ns_RegisterAtSignal((Ns_Callback *)(ns_funcptr_t)LogRollCallback, logPtr);
    }

    /*
     * Parse extended headers; it is just a list of names
     */
    (void)ParseExtendedHeaders(logPtr, Ns_ConfigGetValue(path, "extendedheaders"));

    /*
     *  Open the log and register the trace
     */

    if (LogOpen(logPtr) != NS_OK) {
        return NS_ERROR;
    }

    Ns_RegisterServerTrace(server, LogTrace, logPtr);
    Ns_RegisterAtShutdown(LogCloseCallback, logPtr);
    result = Ns_TclRegisterTrace(server, AddCmds, logPtr, NS_TCL_TRACE_CREATE);

    return result;
}

static Ns_ReturnCode
AddCmds(Tcl_Interp *interp, const void *arg)
{
    const Log *logPtr = arg;

    TCL_CREATEOBJCOMMAND(interp, "ns_accesslog", LogObjCmd, (ClientData)logPtr, NULL);
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ParseExtendedHeaders --
 *
 *      Parse a string specifying the extended parameters.
 *      The string might be:
 *
 *       - a Tcl list of plain request header fields, like e.g.
 *         {Referer X-Forwarded-For}
 *
 *       - a Tcl list of header fields with tags to denote request or response
 *          header fields, like e.g. {req:Referer response:Content-Type}
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Updating fields in logPtr
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ParseExtendedHeaders(Log *logPtr, const char *str)
{
    Ns_ReturnCode result = NS_OK;

    NS_NONNULL_ASSERT(logPtr != NULL);

    if (str != NULL) {
        int          argc;
        const char **argv;

        if (Tcl_SplitList(NULL, str, &argc, &argv) != TCL_OK) {
            Ns_Log(Error, "nslog: invalid 'extendedHeaders' parameter: '%s'", str);
            result = NS_ERROR;

        } else {
            int i, tagged = 0;

            if (logPtr->extendedHeaders != NULL) {
                ns_free((char *)logPtr->extendedHeaders);
            }
            if (logPtr->requestHeaders != NULL) {
                Tcl_Free((char *) logPtr->requestHeaders);
            }
            if (logPtr->responseHeaders != NULL) {
                Tcl_Free((char *)logPtr->responseHeaders);
            }
            logPtr->extendedHeaders = ns_strdup(str);

            for (i = 0; i < argc; i++) {
                const char *fieldName = argv[i];

                if (strchr(fieldName, ':') != NULL) {
                    tagged ++;
                }
            }
            if (tagged == 0) {
                logPtr->requestHeaders = (const char **)argv;
                logPtr->nrRequestHeaders = argc;
                logPtr->responseHeaders = NULL;
                logPtr->nrResponseHeaders = 0;
            } else {
                Tcl_DString requestHeaderFields, responseHeaderFields;

                Tcl_DStringInit(&requestHeaderFields);
                Tcl_DStringInit(&responseHeaderFields);

                for (i = 0; i < argc; i++) {
                    const char *fieldName = argv[i];
                    char       *suffix = strchr(fieldName, ':');

                    if (suffix != NULL) {
                        *suffix = '\0';
                        suffix ++;
                        if (strncmp(fieldName, "request", 3) == 0) {
                            Tcl_DStringAppendElement(&requestHeaderFields, suffix);
                        } else if (strncmp(fieldName, "response", 3) == 0) {
                            Tcl_DStringAppendElement(&responseHeaderFields, suffix);
                        } else {
                            Ns_Log(Error, "nslog: ignore invalid entry prefix '%s' in extendedHeaders parameter",
                                   fieldName);
                        }
                    } else {
                        /*
                         * No prefix, assume request header field
                         */
                        Tcl_DStringAppendElement(&requestHeaderFields, suffix);
                    }
                }
                (void) Tcl_SplitList(NULL, requestHeaderFields.string,
                                     &logPtr->nrRequestHeaders,
                                     &logPtr->requestHeaders);
                (void) Tcl_SplitList(NULL, responseHeaderFields.string,
                                     &logPtr->nrResponseHeaders,
                                     &logPtr->responseHeaders);

                Tcl_DStringFree(&requestHeaderFields);
                Tcl_DStringFree(&responseHeaderFields);
                Tcl_Free((char*)argv);
            }
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * LogObjCmd --
 *
 *      Implements "ns_accesslog".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Depends on command.
 *
 *----------------------------------------------------------------------
 */

static int
LogObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    const char    *strarg;
    int            rc, cmd, result = TCL_OK;
    Tcl_DString    ds;
    Log           *logPtr = clientData;

    enum {
        ROLLFMT, MAXBACKUP, MAXBUFFER, EXTHDRS,
        FLAGS, FILE, ROLL
    };
    static const char *const subcmd[] = {
        "rollfmt", "maxbackup", "maxbuffer", "extendedheaders",
        "flags", "file", "roll", NULL
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg ...?");
        return TCL_ERROR;
    }
    rc = Tcl_GetIndexFromObj(interp, objv[1], subcmd, "option", 0, &cmd);
    if (rc != TCL_OK) {
        return TCL_ERROR;
    }

    switch (cmd) {
    case ROLLFMT:
        Ns_MutexLock(&logPtr->lock);
        if (objc > 2) {
            strarg = ns_strdup(Tcl_GetString(objv[2]));
            if (logPtr->rollfmt != NULL) {
                ns_free((char *)logPtr->rollfmt);
            }
            logPtr->rollfmt = strarg;
        }
        strarg = logPtr->rollfmt;
        Ns_MutexUnlock(&logPtr->lock);
        if (strarg != NULL) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(strarg, TCL_INDEX_NONE));
        }
        break;

    case MAXBACKUP:
        {
            int intarg = 0;

            if (objc > 2) {
                if (Tcl_GetIntFromObj(interp, objv[2], &intarg) != TCL_OK) {
                    result = TCL_ERROR;
                } else {
                    if (intarg < 1) {
                        intarg = 100;
                    }
                }
            }
            if (result == TCL_OK) {
                Ns_MutexLock(&logPtr->lock);
                if (objc > 2) {
                    logPtr->maxbackup = (TCL_SIZE_T)intarg;
                } else {
                    intarg = (int)logPtr->maxbackup;
                }
                Ns_MutexUnlock(&logPtr->lock);
                Tcl_SetObjResult(interp, Tcl_NewIntObj(intarg));
            }
        }
        break;

    case MAXBUFFER:
        {
            int intarg = 0;

            if (objc > 2) {
                if (Tcl_GetIntFromObj(interp, objv[2], &intarg) != TCL_OK) {
                    result = TCL_ERROR;
                } else {
                    if (intarg < 0) {
                        intarg = 0;
                    }
                }
            }
            if (result == TCL_OK) {
                Ns_MutexLock(&logPtr->lock);
                if (objc > 2) {
                    logPtr->maxlines = intarg;
                } else {
                    intarg = logPtr->maxlines;
                }
                Ns_MutexUnlock(&logPtr->lock);
                Tcl_SetObjResult(interp, Tcl_NewIntObj(intarg));
            }
        }
        break;

    case EXTHDRS:
        {
            Ns_MutexLock(&logPtr->lock);
            if (objc > 2) {
                result = ParseExtendedHeaders(logPtr, Tcl_GetString(objv[2]));
            }
            if (result == TCL_OK) {
                Tcl_SetObjResult(interp, Tcl_NewStringObj(logPtr->extendedHeaders, TCL_INDEX_NONE));
            } else {
                Ns_TclPrintfResult(interp, "invalid value: %s",
                                   Tcl_GetString(objv[2]));
            }
            Ns_MutexUnlock(&logPtr->lock);
        }
        break;

    case FLAGS:
        {
            unsigned int flags;

            Tcl_DStringInit(&ds);
            if (objc > 2) {
                flags = 0u;
                Tcl_DStringAppend(&ds, Tcl_GetString(objv[2]), TCL_INDEX_NONE);
                Ns_StrToLower(ds.string);
                if (strstr(ds.string, "logcombined")) {
                    flags |= LOG_COMBINED;
                }
                if (strstr(ds.string, "formattedtime")) {
                    flags |= LOG_FMTTIME;
                }
                if (strstr(ds.string, "logreqtime")) {
                    flags |= LOG_REQTIME;
                }
                if (strstr(ds.string, "logpartialtimes")) {
                    flags |= LOG_PARTIALTIMES;
                }
                if (strstr(ds.string, "checkforproxy")) {
                    flags |= LOG_CHECKFORPROXY;
                }
                if (strstr(ds.string, "suppressquery")) {
                    flags |= LOG_SUPPRESSQUERY;
                }
                Tcl_DStringSetLength(&ds, 0);
                Ns_MutexLock(&logPtr->lock);
                logPtr->flags = flags;
                Ns_MutexUnlock(&logPtr->lock);
            } else {
                Ns_MutexLock(&logPtr->lock);
                flags = logPtr->flags;
                Ns_MutexUnlock(&logPtr->lock);
            }
            if ((flags & LOG_COMBINED)) {
                Tcl_DStringAppend(&ds, "logcombined ", TCL_INDEX_NONE);
            }
            if ((flags & LOG_FMTTIME)) {
                Tcl_DStringAppend(&ds, "formattedtime ", TCL_INDEX_NONE);
            }
            if ((flags & LOG_REQTIME)) {
                Tcl_DStringAppend(&ds, "logreqtime ", TCL_INDEX_NONE);
            }
            if ((flags & LOG_PARTIALTIMES)) {
                Tcl_DStringAppend(&ds, "logpartialtimes ", TCL_INDEX_NONE);
            }
            if ((flags & LOG_CHECKFORPROXY)) {
                Tcl_DStringAppend(&ds, "checkforproxy ", TCL_INDEX_NONE);
            }
            if ((flags & LOG_SUPPRESSQUERY)) {
                Tcl_DStringAppend(&ds, "suppressquery ", TCL_INDEX_NONE);
            }
            Tcl_DStringResult(interp, &ds);
        }
        break;

    case FILE:
        if (objc > 2) {
            Tcl_DStringInit(&ds);
            strarg = Tcl_GetString(objv[2]);
            if (Ns_PathIsAbsolute(strarg) == NS_FALSE) {
                Ns_HomePath(&ds, strarg, (char *)0L);
                strarg = ds.string;
            }
            Ns_MutexLock(&logPtr->lock);
            LogClose(logPtr);
            ns_free((char *)logPtr->filename);
            logPtr->filename = ns_strdup(strarg);
            Tcl_DStringFree(&ds);
            LogOpen(logPtr);
        } else {
            Ns_MutexLock(&logPtr->lock);
        }
        Tcl_SetObjResult(interp, Tcl_NewStringObj(logPtr->filename, TCL_INDEX_NONE));
        Ns_MutexUnlock(&logPtr->lock);
        break;

    case ROLL:
        {
            Ns_ReturnCode status = NS_ERROR;

            Ns_MutexLock(&logPtr->lock);
            if (objc == 2) {
                status = LogRoll(logPtr);
            } else if (objc > 2) {
                strarg = Tcl_GetString(objv[2]);
                if (Tcl_FSAccess(objv[2], F_OK) == 0) {
                    status = Ns_RollFile(strarg, logPtr->maxbackup);
                } else {
                    Tcl_Obj *path = Tcl_NewStringObj(logPtr->filename, TCL_INDEX_NONE);

                    Tcl_IncrRefCount(path);
                    rc = Tcl_FSRenameFile(path, objv[2]);
                    Tcl_DecrRefCount(path);
                    if (rc != 0) {
                        status = NS_ERROR;
                    } else {
                        LogFlush(logPtr, &logPtr->buffer);
                        status = LogOpen(logPtr);
                    }
                }
            }
            if (status != NS_OK) {
                Ns_TclPrintfResult(interp, "could not roll \"%s\": %s",
                                   logPtr->filename, Tcl_PosixError(interp));
            }
            Ns_MutexUnlock(&logPtr->lock);
            if (status != NS_OK) {
                result = TCL_ERROR;
            }
        }
        break;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * AppendEscaped --
 *
 *      Append a string with escaped characters
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      updated dstring
 *
 *----------------------------------------------------------------------
 */

static void
AppendEscaped(Tcl_DString *dsPtr, const char *toProcess)
{
    const char *breakChar;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(toProcess != NULL);

    do {
        breakChar = strpbrk(toProcess, "\r\n\t\\\"");
        if (breakChar == NULL) {
            /*
             * No break-char found, append all and stop
             */
            Tcl_DStringAppend(dsPtr, toProcess, TCL_INDEX_NONE);
        } else {
            /*
             * Append the break-char free prefix
             */
            Tcl_DStringAppend(dsPtr, toProcess, (TCL_SIZE_T)(breakChar - toProcess));

            /*
             * Escape the break-char
             */
            switch (*breakChar) {
            case '\n':
                Tcl_DStringAppend(dsPtr, "\\n", 2);
                break;
            case '\r':
                Tcl_DStringAppend(dsPtr, "\\r", 2);
                break;
            case '\t':
                Tcl_DStringAppend(dsPtr, "\\t", 2);
                break;
            case '"':
                Tcl_DStringAppend(dsPtr, "\\\"", 2);
                break;
            case '\\':
                Tcl_DStringAppend(dsPtr, "\\\\", 2);
                break;
            default:
                /*should not happen */ assert(0);
                break;
            }

            /*
             * Check for further protected characters after the break char.
             */
            toProcess = breakChar + 1;
        }
    } while (breakChar != NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * AppendExtHeaders --
 *
 *      Append named extended header fields from provided set to log entry.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Append to Tcl_DString
 *
 *----------------------------------------------------------------------
 */

static void
AppendExtHeaders(Tcl_DString *dsPtr, const char **argv, const Ns_Set *set)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (set != NULL && argv != NULL) {
        const char **h;

        for (h = argv; *h != NULL; h++) {
            const char *p;

            Tcl_DStringAppend(dsPtr, " \"", 2);
            p = Ns_SetIGet(set, *h);
            if (p != NULL) {
                AppendEscaped(dsPtr, p);
            }
            Tcl_DStringAppend(dsPtr, "\"", 1);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * LogTrace --
 *
 *      Trace routine for appending the log with the current
 *      connection results.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Entry is appended to the open log.
 *
 *----------------------------------------------------------------------
 */

static void
LogTrace(void *arg, Ns_Conn *conn)
{
    Log          *logPtr = arg;
    const char   *user, *p, *driverName;
    char          buffer[PIPE_BUF], *bufferPtr = NULL;
    int           n;
    Ns_ReturnCode status;
    size_t        bufferSize = 0u;
    Tcl_DString   ds, *dsPtr = &ds;
    char          ipString[NS_IPADDR_SIZE];
    struct NS_SOCKADDR_STORAGE  ipStruct, maskedStruct;
    struct sockaddr            *maskPtr = NULL,
        *ipPtr     = (struct sockaddr *)&ipStruct,
        *maskedPtr = (struct sockaddr *)&maskedStruct;

    driverName = Ns_ConnDriverName(conn);
    Ns_Log(Debug, "nslog called with driver pattern '%s' via driver '%s' req: %s",
           logPtr->driverPattern, driverName, conn->request.line);

    if (logPtr->driverPattern != NULL
        && Tcl_StringMatch(driverName, logPtr->driverPattern) == 0
        ) {
        /*
         * This is not for us.
         */
        return;
    }

    Tcl_DStringInit(dsPtr);
    Ns_MutexLock(&logPtr->lock);

    /*
     * Append the peer address.
     */
    if ((logPtr->flags & LOG_CHECKFORPROXY) != 0u) {
        /*
         * This branch is deprecated and kept only for backward
         * compatibility (added Dec 2020).
         */
        p = Ns_ConnForwardedPeerAddr(conn);
        if (*p == '\0') {
            p = Ns_ConnPeerAddr(conn);
        }
    } else {
        p = Ns_ConnConfiguredPeerAddr(conn);
    }

    /*
     * Check if the actual IP address can be converted to internal format (this
     * should be always possible).
     */
    if ((logPtr->flags |= LOG_MASKIP)
        && (ns_inet_pton(ipPtr, p) == 1)
        ) {

        /*
         * Depending on the class of the IP address, use the appropriate mask.
         */
        if (ipPtr->sa_family == AF_INET) {
            maskPtr = logPtr->ipv4maskPtr;
        }
#ifdef HAVE_IPV6
        if (ipPtr->sa_family == AF_INET6) {
            maskPtr = logPtr->ipv6maskPtr;
        }
#endif
        /*
         * If the mask is non-null, the IP "anonymizing" was configured.
         */
        if (maskPtr != NULL) {
            Ns_SockaddrMask(ipPtr, maskPtr, maskedPtr);
            ns_inet_ntop(maskedPtr, ipString, NS_IPADDR_SIZE);
            p = ipString;
        }
    }

    Tcl_DStringAppend(dsPtr, p, TCL_INDEX_NONE);

    /*
     * Append the thread name, if requested.
     * This eases to link access-log with error-log entries
     */
    Tcl_DStringAppend(dsPtr, " ", 1);
    if ((logPtr->flags & LOG_THREADNAME) != 0) {
        Tcl_DStringAppend(dsPtr, Ns_ThreadGetName(), TCL_INDEX_NONE);
        Tcl_DStringAppend(dsPtr, " ", 1);
    } else {
        Tcl_DStringAppend(dsPtr, "- ", 2);
    }

    /*
     * Append the authorized user, if any. Watch usernames
     * with embedded blanks; we must properly quote them.
     */

    user = Ns_ConnAuthUser(conn);
    if (user == NULL) {
        Tcl_DStringAppend(dsPtr, "- ", 2);
    } else {
        int quote = 0;

        for (p = user; *p && !quote; p++) {
            quote = (CHARTYPE(space, *p) != 0);
        }
        if (quote != 0) {
            Tcl_DStringAppend(dsPtr, "\"", 1);
            Tcl_DStringAppend(dsPtr, user, TCL_INDEX_NONE);
            Tcl_DStringAppend(dsPtr, "\" ", 2);
        } else {
            Tcl_DStringAppend(dsPtr, user, TCL_INDEX_NONE);
            Tcl_DStringAppend(dsPtr, " ", 1);
        }
    }

    /*
     * Append a common log format timestamp including GMT offset
     */

    if (!(logPtr->flags & LOG_FMTTIME)) {
        Ns_DStringPrintf(dsPtr, "[%" PRId64 "]", (int64_t) time(NULL));
    } else {
        char buf[41]; /* Big enough for Ns_LogTime(). */

        Ns_LogTime(buf);
        Tcl_DStringAppend(dsPtr, buf, TCL_INDEX_NONE);
    }

    /*
     * Append the request line plus query data (if configured)
     */

    if (likely(conn->request.line != NULL)) {
        const char *string = (logPtr->flags & LOG_SUPPRESSQUERY) ?
            conn->request.url :
            conn->request.line;

        Tcl_DStringAppend(dsPtr, " \"", 2);
        if (likely(string != NULL)) {
            AppendEscaped(dsPtr, string);
        }
        Tcl_DStringAppend(dsPtr, "\" ", 2);

    } else {
        Tcl_DStringAppend(dsPtr, " \"\" ", 4);
    }

    /*
     * Construct and append the HTTP status code and bytes sent
     */

    n = Ns_ConnResponseStatus(conn);
    Ns_DStringPrintf(dsPtr, "%d %" PRIdz, (n != 0) ? n : 200, Ns_ConnContentSent(conn));

    /*
     * Append the referrer (using the misspelled header field "Referer") and
     * user-agent headers (if any)
     */

    if ((logPtr->flags & LOG_COMBINED)) {

        Tcl_DStringAppend(dsPtr, " \"", 2);
        p = Ns_SetIGet(conn->headers, "referer");
        if (p != NULL) {
            AppendEscaped(dsPtr, p);
        }
        Tcl_DStringAppend(dsPtr, "\" \"", 3);
        p = Ns_SetIGet(conn->headers, "user-agent");
        if (p != NULL) {
            AppendEscaped(dsPtr, p);
        }
        Tcl_DStringAppend(dsPtr, "\"", 1);
    }

    /*
     * Append the request's elapsed time and queue time (if enabled)
     */

    if ((logPtr->flags & LOG_REQTIME) != 0u) {
        Ns_Time reqTime, now;
        Ns_GetTime(&now);
        Ns_DiffTime(&now, Ns_ConnStartTime(conn), &reqTime);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &reqTime);

    }

    if ((logPtr->flags & LOG_PARTIALTIMES) != 0u) {
        Ns_Time  acceptTime, queueTime, filterTime, runTime;
        Ns_Time *startTimePtr =  Ns_ConnStartTime(conn);

        Ns_ConnTimeSpans(conn, &acceptTime, &queueTime, &filterTime, &runTime);

        Tcl_DStringAppend(dsPtr, " \"", 2);
        Ns_DStringAppendTime(dsPtr, startTimePtr);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &acceptTime);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &queueTime);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &filterTime);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &runTime);
        Tcl_DStringAppend(dsPtr, "\"", 1);
    }

    /*
     * Append the extended headers (if any)
     */
    AppendExtHeaders(dsPtr, logPtr->requestHeaders, conn->headers);
    AppendExtHeaders(dsPtr, logPtr->responseHeaders, conn->outputheaders);

    {
        TCL_SIZE_T l;
        for (l = 0; l < dsPtr->length; l++) {
            /*
             * Quick fix to disallow terminal escape characters in the log
             * file. See e.g. http://www.securityfocus.com/bid/37712/info
             */
            if (unlikely(dsPtr->string[l] == 0x1b)) {
                dsPtr->string[l] = 7; /* bell */
            }
        }
    }

    Ns_Log(Ns_LogAccessDebug, "%s", dsPtr->string);

    /*
     * Append the trailing newline and optionally
     * flush the buffer
     */

    Tcl_DStringAppend(dsPtr, "\n", 1);

    if (logPtr->maxlines == 0) {
        bufferSize = (size_t)dsPtr->length;
        if (bufferSize < PIPE_BUF) {
          /*
           * Only ns_write() operations < PIPE_BUF are guaranteed to be atomic
           */
            bufferPtr = dsPtr->string;
            status = NS_OK;
        } else {
            status = LogFlush(logPtr, dsPtr);
        }
    } else {
        Tcl_DStringAppend(&logPtr->buffer, dsPtr->string, dsPtr->length);
        if (++logPtr->curlines > logPtr->maxlines) {
            bufferSize = (size_t)logPtr->buffer.length;
            if (bufferSize < PIPE_BUF) {
                /*
                 * Only ns_write() operations < PIPE_BUF are guaranteed to be
                 * atomic.  In most cases, the other branch is used.
                 */
              memcpy(buffer, logPtr->buffer.string, bufferSize);
              bufferPtr = buffer;
              Tcl_DStringSetLength(&logPtr->buffer, 0);
              status = NS_OK;
            } else {
              status = LogFlush(logPtr, &logPtr->buffer);
            }
            logPtr->curlines = 0;
        } else {
            status = NS_OK;
        }
    }
    Ns_MutexUnlock(&logPtr->lock);
    (void)(status); /* ignore status */

    if (likely(bufferPtr != NULL) && likely(logPtr->fd >= 0) && likely(bufferSize > 0)) {
        (void)NsAsyncWrite(logPtr->fd, bufferPtr, bufferSize);
    }

    Tcl_DStringFree(dsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * LogOpen --
 *
 *      Open the access log, closing previous log if opened.
 *      Assume caller is holding the log mutex.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Log re-opened.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogOpen(void *arg)
{
    int           fd;
    Ns_ReturnCode status;
    Log          *logPtr = (Log *)arg;

    fd = ns_open(logPtr->filename, O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd == NS_INVALID_FD) {
        Ns_Log(Error, "nslog: error '%s' opening '%s'",
               strerror(errno), logPtr->filename);
        status = NS_ERROR;
    } else {
        status = NS_OK;
        if (logPtr->fd >= 0) {
            ns_close(logPtr->fd);
        }

        logPtr->fd = fd;
        Ns_Log(Notice, "nslog: opened '%s'", logPtr->filename);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LogClose --
 *
 *      Flush and/or close the log.
 *      Assume caller is holding the log mutex.
 *
 * Results:
 *      NS_TRUE or NS_FALSE if log was closed.
 *
 * Side effects:
 *      Buffer entries, if any, are flushed.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogClose(void *arg)
{
    Ns_ReturnCode status = NS_OK;
    Log *logPtr = (Log *)arg;

    if (logPtr->fd >= 0) {
        status = LogFlush(logPtr, &logPtr->buffer);
        ns_close(logPtr->fd);
        logPtr->fd = NS_INVALID_FD;
        Tcl_DStringFree(&logPtr->buffer);
        Ns_Log(Notice, "nslog: closed '%s'", logPtr->filename);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LogFlush --
 *
 *      Flush a log buffer to the open log file.
 *      Assume caller is holding the log mutex.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will disable the log on error.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogFlush(Log *logPtr, Tcl_DString *dsPtr)
{
    TCL_SIZE_T len = dsPtr->length;
    char      *buf = dsPtr->string;

    if (len > 0) {
        if (logPtr->fd >= 0 && ns_write(logPtr->fd, buf, (size_t)len) != (ssize_t)len) {
            Ns_Log(Error, "nslog: logging disabled: ns_write() failed: '%s'",
                   strerror(errno));
            ns_close(logPtr->fd);
            logPtr->fd = NS_INVALID_FD;
        }
        Tcl_DStringSetLength(dsPtr, 0);
    }

    return (logPtr->fd == NS_INVALID_FD) ? NS_ERROR : NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogRoll --
 *
 *      Roll and re-open the access log.  This procedure is scheduled
 *      and/or registered at signal catching.
 *
 *      Assume caller is holding the log mutex.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Files are rolled to new names.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogRoll(void *arg)
{
    Ns_ReturnCode status;
    Log          *logPtr = (Log *)arg;

    status = Ns_RollFileCondFmt(LogOpen, LogClose, logPtr,
                                logPtr->filename,
                                logPtr->rollfmt,
                                logPtr->maxbackup);

    //if (status == NS_OK) {
    //    status = LogOpen(logPtr);
    //}

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LogCloseCallback, LogRollCallback -
 *
 *      Close or roll the log.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See LogClose and LogRoll.
 *
 *----------------------------------------------------------------------
 */

static void
LogCallbackProc(Ns_LogCallbackProc proc, void *arg, const char *desc)
{
    int  status;
    Log *logPtr = arg;

    Ns_MutexLock(&logPtr->lock);
    status =(*proc)(logPtr);
    Ns_MutexUnlock(&logPtr->lock);

    if (status != NS_OK) {
        Ns_Log(Error, "nslog: failed: %s '%s': '%s'", desc, logPtr->filename,
               strerror(Tcl_GetErrno()));
    }
}

static void
LogCloseCallback(const Ns_Time *toPtr, void *arg)
{
    if (toPtr == NULL) {
        LogCallbackProc(LogClose, arg, "close");
    }
}

static void
LogRollCallback(void *arg, int UNUSED(id))
{
    LogCallbackProc(LogRoll, arg, "roll");
}


/*
 *----------------------------------------------------------------------
 *
 * LogArg --
 *
 *      Copy log filename as argument for callback introspection queries.
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
LogArg(Tcl_DString *dsPtr, const void *arg)
{
    const Log *logPtr = arg;

    Tcl_DStringAppendElement(dsPtr, logPtr->filename);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
