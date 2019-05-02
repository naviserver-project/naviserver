/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1(the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis,WITHOUT WARRANTY OF ANY KIND,either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright(C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively,the contents of this file may be used under the terms
 * of the GNU General Public License(the "GPL"),in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License,indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above,a recipient may use your
 * version of this file under either the License or the GPL.
 *
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

NS_EXPORT const int Ns_ModuleVersion = 1;

typedef struct {
    Ns_Mutex     lock;
    const char  *module;
    const char  *file;
    const char  *rollfmt;
    const char **extheaders;
    int          numheaders;
    int          fd;
    unsigned int flags;
    int          maxbackup;
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

static Ns_Callback     LogRollCallback;
static Ns_ShutdownProc LogCloseCallback;
static Ns_TraceProc    LogTrace;
static Ns_ArgProc      LogArg;
static Ns_TclTraceProc AddCmds;
static Tcl_ObjCmdProc  LogObjCmd;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;

static Ns_ReturnCode LogFlush(Log *logPtr, Tcl_DString *dsPtr);
static Ns_ReturnCode LogOpen (Log *logPtr);
static Ns_ReturnCode LogRoll (Log *logPtr);
static Ns_ReturnCode LogClose(Log *logPtr);

static void AppendEscaped(Tcl_DString *dsPtr, const char *toProcess)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);




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

    path = Ns_ConfigGetPath(server, module, (char *)0L);

    /*
     * Determine the name of the log file
     */

    file = Ns_ConfigString(path, "file", "access.log");
    if (Ns_PathIsAbsolute(file) == NS_TRUE) {
        logPtr->file = ns_strdup(file);
    } else {
        /*
         * If log file is not given in absolute format, it's expected to
         * exist in the global logs directory if such exists or module
         * specific directory, which is created if necessary.
         */

        if (Ns_HomePathExists("logs", (char *)0L)) {
            (void) Ns_HomePath(&ds, "logs", "/", file, (char *)0L);
        } else {
            Tcl_Obj *dirpath;
            int rc;

            Tcl_DStringSetLength(&ds, 0);
            (void) Ns_ModulePath(&ds, server, module, (char *)0L);
            dirpath = Tcl_NewStringObj(ds.string, -1);
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
        logPtr->file = Ns_DStringExport(&ds);
    }

    /*
     * Get other parameters from configuration file
     */

    logPtr->rollfmt = ns_strcopy(Ns_ConfigGetValue(path, "rollfmt"));
    logPtr->maxbackup = Ns_ConfigIntRange(path, "maxbackup", 100, 1, INT_MAX);
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
        logPtr->flags |= LOG_CHECKFORPROXY;
    }

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
        Ns_ScheduleDaily((Ns_SchedProc *) LogRollCallback, logPtr,
                         0, hour, 0, NULL);
    }
    if (Ns_ConfigBool(path, "rollonsignal", NS_FALSE)) {
        Ns_RegisterAtSignal(LogRollCallback, logPtr);
    }

    /*
     * Parse extended headers; it is just a list of names
     */

    Tcl_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, Ns_ConfigGetValue(path, "extendedheaders"), (char *)0L);
    if (Tcl_SplitList(NULL, ds.string, &logPtr->numheaders,
                      &logPtr->extheaders) != TCL_OK) {
        Ns_Log(Error, "nslog: invalid %s/extendedHeaders parameter: '%s'",
               path, ds.string);
    }
    Tcl_DStringFree(&ds);

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

static int
AddCmds(Tcl_Interp *interp, const void *arg)
{
    const Log *logPtr = arg;

    Tcl_CreateObjCommand(interp, "ns_accesslog", LogObjCmd, (ClientData)logPtr, NULL);
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogObjCmd --
 *
 *      Implement the ns_accesslog command.
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
LogObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const char    *strarg, **hdrs;
    int            rc, intarg, cmd;
    Tcl_DString     ds;
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
            Tcl_SetObjResult(interp, Tcl_NewStringObj(strarg, -1));
        }
        break;

    case MAXBACKUP:
        if (objc > 2) {
            if (Tcl_GetIntFromObj(interp, objv[2], &intarg) != TCL_OK) {
                return TCL_ERROR;
            }
            if (intarg < 1) {
                intarg = 100;
            }
        }
        Ns_MutexLock(&logPtr->lock);
        if (objc > 2) {
            logPtr->maxbackup = intarg;
        } else {
            intarg = logPtr->maxbackup;
        }
        Ns_MutexUnlock(&logPtr->lock);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(intarg));
        break;

    case MAXBUFFER:
        if (objc > 2) {
            if (Tcl_GetIntFromObj(interp, objv[2], &intarg) != TCL_OK) {
                return TCL_ERROR;
            }
            if (intarg < 0) {
                intarg = 0;
            }
        }
        Ns_MutexLock(&logPtr->lock);
        if (objc > 2) {
            logPtr->maxlines = intarg;
        } else {
            intarg = logPtr->maxlines;
        }
        Ns_MutexUnlock(&logPtr->lock);
        Tcl_SetObjResult(interp, Tcl_NewIntObj(intarg));
        break;

    case EXTHDRS:
        {
            int n;
            if (objc > 2) {
                strarg = Tcl_GetString(objv[2]);
                if (Tcl_SplitList(interp, strarg, &n, &hdrs) != TCL_OK) {
                    return TCL_ERROR;
                }
            }
            Ns_MutexLock(&logPtr->lock);
            if (objc > 2) {
                if (logPtr->extheaders != NULL) {
                    Tcl_Free((char*)logPtr->extheaders);
                }
                logPtr->extheaders = hdrs;
                logPtr->numheaders = n;
            }
            strarg = Tcl_Merge(logPtr->numheaders, logPtr->extheaders);
            Ns_MutexUnlock(&logPtr->lock);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(strarg, -1));
        }
        break;

    case FLAGS:
        {
            unsigned int flags;

            Tcl_DStringInit(&ds);
            if (objc > 2) {
                flags = 0u;
                Tcl_DStringAppend(&ds, Tcl_GetString(objv[2]), -1);
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
                Tcl_DStringAppend(&ds, "logcombined ", -1);
            }
            if ((flags & LOG_FMTTIME)) {
                Tcl_DStringAppend(&ds, "formattedtime ", -1);
            }
            if ((flags & LOG_REQTIME)) {
                Tcl_DStringAppend(&ds, "logreqtime ", -1);
            }
            if ((flags & LOG_PARTIALTIMES)) {
                Tcl_DStringAppend(&ds, "logpartialtimes ", -1);
            }
            if ((flags & LOG_CHECKFORPROXY)) {
                Tcl_DStringAppend(&ds, "checkforproxy ", -1);
            }
            if ((flags & LOG_SUPPRESSQUERY)) {
                Tcl_DStringAppend(&ds, "suppressquery ", -1);
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
            ns_free((char *)logPtr->file);
            logPtr->file = ns_strdup(strarg);
            Tcl_DStringFree(&ds);
            LogOpen(logPtr);
        } else {
            Ns_MutexLock(&logPtr->lock);
        }
        Tcl_SetObjResult(interp, Tcl_NewStringObj(logPtr->file, -1));
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
                    Tcl_Obj *path = Tcl_NewStringObj(logPtr->file, -1);

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
                                   logPtr->file, Tcl_PosixError(interp));
            }
            Ns_MutexUnlock(&logPtr->lock);
            if (status != NS_OK) {
                return TCL_ERROR;
            }
        }
        break;
    }

    return TCL_OK;
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
            Tcl_DStringAppend(dsPtr, toProcess, -1);
        } else {
            /*
             * Append the break-char free prefix
             */
            Tcl_DStringAppend(dsPtr, toProcess, (int)(breakChar - toProcess));

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
                Tcl_DStringAppend(dsPtr, "\\\\",2);
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
    const char  **h, *user, *p;
    char          buffer[PIPE_BUF], *bufferPtr = NULL;
    int           n, i;
    Ns_ReturnCode status;
    size_t        bufferSize = 0u;
    Tcl_DString   ds, *dsPtr = &ds;
    char          ipString[NS_IPADDR_SIZE];
    struct NS_SOCKADDR_STORAGE  ipStruct, maskedStruct;
    struct sockaddr            *maskPtr = NULL,
        *ipPtr     = (struct sockaddr *)&ipStruct,
        *maskedPtr = (struct sockaddr *)&maskedStruct;


    Tcl_DStringInit(dsPtr);
    Ns_MutexLock(&logPtr->lock);

    /*
     * Append the peer address. Watch for users coming
     * from proxy servers (if configured).
     */

    if ((logPtr->flags & LOG_CHECKFORPROXY) != 0u) {
        p = Ns_SetIGet(conn->headers, "X-Forwarded-For");
        if (p != NULL && !strcasecmp(p, "unknown")) {
            p = NULL;
        }
        if (p == NULL) {
            p = Ns_ConnPeerAddr(conn);
        }
    } else {
        p = Ns_ConnPeerAddr(conn);
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

    Tcl_DStringAppend(dsPtr, p, -1);

    /*
     * Append the thread name, if requested.
     * This eases to link access-log with error-log entries
     */
    Tcl_DStringAppend(dsPtr, " ", 1);
    if ((logPtr->flags & LOG_THREADNAME) != 0) {
        Tcl_DStringAppend(dsPtr, Ns_ThreadGetName(), -1);
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
            Tcl_DStringAppend(dsPtr, user, -1);
            Tcl_DStringAppend(dsPtr, "\" ", 2);
        } else {
            Tcl_DStringAppend(dsPtr, user, -1);
            Tcl_DStringAppend(dsPtr, " ", 1);
        }
    }

    /*
     * Append a common log format time stamp including GMT offset
     */

    if (!(logPtr->flags & LOG_FMTTIME)) {
        Ns_DStringPrintf(dsPtr, "[%" PRId64 "]", (int64_t) time(NULL));
    } else {
        char buf[41]; /* Big enough for Ns_LogTime(). */

        Ns_LogTime(buf);
        Tcl_DStringAppend(dsPtr, buf, -1);
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
     * Append the referer and user-agent headers (if any)
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

    for (h = logPtr->extheaders; *h != NULL; h++) {
        Tcl_DStringAppend(dsPtr, " \"", 2);
        p = Ns_SetIGet(conn->headers, *h);
        if (p != NULL) {
            AppendEscaped(dsPtr, p);
        }
        Tcl_DStringAppend(dsPtr, "\"", 1);
    }

    for (i = 0; i < dsPtr->length; i++) {
        /*
         * Quick fix to disallow terminal escape characters in the log
         * file. See e.g. http://www.securityfocus.com/bid/37712/info
         */
        if (unlikely(dsPtr->string[i] == 0x1b)) {
            dsPtr->string[i] = 7; /* bell */
        }
    }

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
LogOpen(Log *logPtr)
{
    int fd;

    fd = ns_open(logPtr->file, O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd == NS_INVALID_FD) {
        Ns_Log(Error, "nslog: error '%s' opening '%s'",
               strerror(errno), logPtr->file);
        return NS_ERROR;
    }
    if (logPtr->fd >= 0) {
        ns_close(logPtr->fd);
    }

    logPtr->fd = fd;
    Ns_Log(Notice, "nslog: opened '%s'", logPtr->file);

    return NS_OK;
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
 *      Buffer entries,if any,are flushed.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogClose(Log *logPtr)
{
    Ns_ReturnCode status = NS_OK;

    if (logPtr->fd >= 0) {
        status = LogFlush(logPtr, &logPtr->buffer);
        ns_close(logPtr->fd);
        logPtr->fd = NS_INVALID_FD;
        Tcl_DStringFree(&logPtr->buffer);
        Ns_Log(Notice, "nslog: closed '%s'", logPtr->file);
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
    int   len = dsPtr->length;
    char *buf = dsPtr->string;

    if (len > 0) {
        if (logPtr->fd >= 0 && ns_write(logPtr->fd, buf, (size_t)len) != len) {
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
LogRoll(Log *logPtr)
{
    Ns_ReturnCode status = NS_OK;
    Tcl_Obj      *pathObj;

    NsAsyncWriterQueueDisable(NS_FALSE);

    (void)LogClose(logPtr);

    pathObj = Tcl_NewStringObj(logPtr->file, -1);
    Tcl_IncrRefCount(pathObj);

    if (Tcl_FSAccess(pathObj, F_OK) == 0) {
        /*
         * We are already logging to some file
         */
        status = Ns_RollFileFmt(pathObj,
                                logPtr->rollfmt,
                                logPtr->maxbackup);
    }

    Tcl_DecrRefCount(pathObj);

    if (status == NS_OK) {
        status = LogOpen(logPtr);
    }
    NsAsyncWriterQueueEnable();

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
LogCallback(Ns_ReturnCode(proc)(Log *), void *arg, const char *desc)
{
    int  status;
    Log *logPtr = arg;

    Ns_MutexLock(&logPtr->lock);
    status =(*proc)(logPtr);
    Ns_MutexUnlock(&logPtr->lock);

    if (status != NS_OK) {
        Ns_Log(Error, "nslog: failed: %s '%s': '%s'", desc, logPtr->file,
               strerror(Tcl_GetErrno()));
    }
}

static void
LogCloseCallback(const Ns_Time *toPtr, void *arg)
{
    if (toPtr == NULL) {
        LogCallback(LogClose, arg, "close");
    }
}

static void
LogRollCallback(void *arg)
{
    LogCallback(LogRoll, arg, "roll");
}


/*
 *----------------------------------------------------------------------
 *
 * LogArg --
 *
 *      Copy log file as argument for callback introspection queries.
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

    Tcl_DStringAppendElement(dsPtr, logPtr->file);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
