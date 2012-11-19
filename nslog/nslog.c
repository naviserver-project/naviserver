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

#define LOG_COMBINED      (1<<0)
#define LOG_FMTTIME       (1<<1)
#define LOG_REQTIME       (1<<2)
#define LOG_QUEUETIME     (1<<3)
#define LOG_CHECKFORPROXY (1<<4)
#define LOG_SUPPRESSQUERY (1<<5)

#if !defined(PIPE_BUF)
# define PIPE_BUF 512
#endif

NS_EXPORT int Ns_ModuleVersion = 1;

typedef struct {
    Ns_Mutex     lock;
    char        *module;
    char        *file;
    char        *rollfmt;
    CONST char **extheaders;
    int          numheaders;
    int          fd;
    int          flags;
    int          maxbackup;
    int          maxlines;
    int          curlines;
    Ns_DString   buffer;
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

static int LogFlush(Log *logPtr, Ns_DString *dsPtr);
static int LogOpen (Log *logPtr);
static int LogRoll (Log *logPtr);
static int LogClose(Log *logPtr);


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

NS_EXPORT int
Ns_ModuleInit(char *server, char *module)
{
    CONST char *path, *file;
    Log        *logPtr;
    Ns_DString  ds;
    static int  first = 1;

    /*
     * Register the info callbacks just once. This assumes we are
     * called w/o locking from within the server startup.
     */

    if (first) {
        first = 0;
        Ns_RegisterProcInfo((void *)LogRollCallback, "nslog:roll", LogArg);
        Ns_RegisterProcInfo((void *)LogCloseCallback, "nslog:close", LogArg);
        Ns_RegisterProcInfo((void *)LogTrace, "nslog:conntrace", LogArg);
        Ns_RegisterProcInfo((void *)AddCmds, "nslog:initinterp", LogArg);
    }

    Ns_DStringInit(&ds);

    logPtr = ns_calloc(1,sizeof(Log));
    logPtr->module = module;
    logPtr->fd = -1;
    Ns_MutexInit(&logPtr->lock);
    Ns_MutexSetName2(&logPtr->lock, "nslog", server);
    Ns_DStringInit(&logPtr->buffer);

    path = Ns_ConfigGetPath(server, module, NULL);

    /*
     * Determine the name of the log file
     */

    file = Ns_ConfigString(path, "file", "access.log");
    if (Ns_PathIsAbsolute(file)) {
        logPtr->file = ns_strdup(file);
    } else {
        /*
         * If log file is not given in absolute format, it's expected to
         * exist in the global logs directory if such exists or module
         * specific directory, which is created if necessary.
         */

        if (Ns_HomePathExists("logs", NULL)) {
            Ns_HomePath(&ds, "logs", "/", file, NULL);
        } else {
            Tcl_Obj *dirpath;
	    int status;

            Ns_DStringTrunc(&ds, 0);
            Ns_ModulePath(&ds, server, module, NULL, NULL);
            dirpath = Tcl_NewStringObj(ds.string, -1);
            Tcl_IncrRefCount(dirpath);
            status = Tcl_FSCreateDirectory(dirpath);
            Tcl_DecrRefCount(dirpath);
            if (status && Tcl_GetErrno() != EEXIST && Tcl_GetErrno() != EISDIR) {
                Ns_Log(Error,"nslog: create directory (%s) failed: '%s'",
                       ds.string, strerror(Tcl_GetErrno()));
                Ns_DStringFree(&ds);
                return NS_ERROR;
            }
            Ns_DStringTrunc(&ds, 0);
            Ns_ModulePath(&ds, server, module, file, NULL);
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
    if (Ns_ConfigBool(path, "logqueuetime", NS_FALSE)) {  // comment me
        logPtr->flags |= LOG_QUEUETIME;
    }
    if (Ns_ConfigBool(path, "suppressquery", NS_FALSE)) {
        logPtr->flags |= LOG_SUPPRESSQUERY;
    }
    if (Ns_ConfigBool(path, "checkforproxy", NS_FALSE)) {
        logPtr->flags |= LOG_CHECKFORPROXY;
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

    Ns_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, Ns_ConfigGetValue(path, "extendedheaders"), NULL);
    if (Tcl_SplitList(NULL, ds.string, &logPtr->numheaders,
                      &logPtr->extheaders) != TCL_OK) {
        Ns_Log(Error,"nslog: invalid %s/extendedHeaders parameter: '%s'",
               path, ds.string);
    }
    Ns_DStringFree(&ds);

    /*
     *  Open the log and register the trace
     */

    if (LogOpen(logPtr) != NS_OK) {
        return NS_ERROR;
    }

    Ns_RegisterServerTrace(server, LogTrace, logPtr);
    Ns_RegisterAtShutdown(LogCloseCallback, logPtr);
    Ns_TclRegisterTrace(server, AddCmds, logPtr, NS_TCL_TRACE_CREATE);

    return NS_OK;
}

static int
AddCmds(Tcl_Interp *interp, void *arg)
{
    Log *logPtr = arg;

    Tcl_CreateObjCommand(interp, "ns_accesslog", LogObjCmd, logPtr, NULL);
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
LogObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char        *strarg;
    CONST char **hdrs;
    int          status, intarg, cmd;
    Ns_DString   ds;
    Tcl_Obj     *path;
    Log         *logPtr = arg;

    enum {
        ROLLFMT, MAXBACKUP, MAXBUFFER, EXTHDRS,
        FLAGS, FILE, ROLL
    };
    static CONST char *subcmd[] = {
        "rollfmt", "maxbackup", "maxbuffer", "extendedheaders",
        "flags", "file", "roll", NULL
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg ...?");
        return TCL_ERROR;
    }
    status = Tcl_GetIndexFromObj(interp, objv[1], subcmd, "option", 0, &cmd);
    if (status != TCL_OK) {
        return TCL_ERROR;
    }

    switch (cmd) {
    case ROLLFMT:
        Ns_MutexLock(&logPtr->lock);
        if (objc > 2) {
            strarg = ns_strdup(Tcl_GetString(objv[2]));
            if (logPtr->rollfmt) {
                ns_free(logPtr->rollfmt);
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
        if (objc > 2) {
            strarg = Tcl_GetString(objv[2]);
            if (Tcl_SplitList(interp, strarg, &status, &hdrs) != TCL_OK) {
                return TCL_ERROR;
            }
        }
        Ns_MutexLock(&logPtr->lock);
        if (objc > 2) {
            if (logPtr->extheaders) {
                Tcl_Free((char*)logPtr->extheaders);
            }
            logPtr->extheaders = hdrs;
            logPtr->numheaders = status;
        }
        strarg = Tcl_Merge(logPtr->numheaders, logPtr->extheaders);
        Ns_MutexUnlock(&logPtr->lock);
        Tcl_SetResult(interp, strarg, TCL_DYNAMIC);
        break;

    case FLAGS:
        Ns_DStringInit(&ds);
        if (objc > 2) {
            status = 0;
            Ns_DStringAppend(&ds, Tcl_GetString(objv[2]));
            Ns_StrToLower(ds.string);
            if (strstr(ds.string, "logcombined")) {
                status |= LOG_COMBINED;
            }
            if (strstr(ds.string, "formattedtime")) {
                status |= LOG_FMTTIME;
            }
            if (strstr(ds.string, "logreqtime")) {
                status |= LOG_REQTIME;
            }
            if (strstr(ds.string, "logqueuetime")) {
                status |= LOG_QUEUETIME;
            }
            if (strstr(ds.string, "checkforproxy")) {
                status |= LOG_CHECKFORPROXY;
            }
            if (strstr(ds.string, "suppressquery")) {
                status |= LOG_SUPPRESSQUERY;
            }
            Ns_DStringTrunc(&ds, 0);
            Ns_MutexLock(&logPtr->lock);
            logPtr->flags = status;
            Ns_MutexUnlock(&logPtr->lock);
        } else {
            Ns_MutexLock(&logPtr->lock);
            status = logPtr->flags;
            Ns_MutexUnlock(&logPtr->lock);
        }
        if ((status & LOG_COMBINED)) {
            Ns_DStringAppend(&ds, "logcombined ");
        }
        if ((status & LOG_FMTTIME)) {
            Ns_DStringAppend(&ds, "formattedtime ");
        }
        if ((status & LOG_REQTIME)) {
            Ns_DStringAppend(&ds, "logreqtime ");
        }
        if ((status & LOG_QUEUETIME)) {
            Ns_DStringAppend(&ds, "logqueuetime ");
        }
        if ((status & LOG_CHECKFORPROXY)) {
            Ns_DStringAppend(&ds, "checkforproxy ");
        }
        if ((status & LOG_SUPPRESSQUERY)) {
            Ns_DStringAppend(&ds, "suppressquery ");
        }
        Tcl_AppendResult(interp, ds.string, NULL);
        Ns_DStringFree(&ds);
        break;

    case FILE:
        if (objc > 2) {
            Ns_DStringInit(&ds);
            strarg = Tcl_GetString(objv[2]);
            if (Ns_PathIsAbsolute(strarg) == NS_FALSE) {
                Ns_HomePath(&ds, strarg, NULL);
                strarg = ds.string;
            }
            Ns_MutexLock(&logPtr->lock);
            LogClose(logPtr);
            ns_free(logPtr->file);
            logPtr->file = ns_strdup(strarg);
            Ns_DStringFree(&ds);
            LogOpen(logPtr);
        } else {
            Ns_MutexLock(&logPtr->lock);
        }
        Tcl_SetResult(interp, logPtr->file, TCL_STATIC);
        Ns_MutexUnlock(&logPtr->lock);
        break;

    case ROLL:
        Ns_MutexLock(&logPtr->lock);
        if (objc == 2) {
            status = LogRoll(logPtr);
        } else if (objc > 2) {
            strarg = Tcl_GetString(objv[2]);
            if (Tcl_FSAccess(objv[2], F_OK) == 0) {
                status = Ns_RollFile(strarg, logPtr->maxbackup);
            } else {
                path = Tcl_NewStringObj(logPtr->file, -1);
                Tcl_IncrRefCount(path);
                status = Tcl_FSRenameFile(path, objv[2]);
                Tcl_DecrRefCount(path);
                if (status != 0) {
                    status = NS_ERROR;
                } else {
                    LogFlush(logPtr, &logPtr->buffer);
                    status = LogOpen(logPtr);
                }
            }
        }
        if (status != NS_OK) {
            Tcl_AppendResult(interp, "could not roll \"", logPtr->file,
                             "\": ", Tcl_PosixError(interp), NULL);
        }
        Ns_MutexUnlock(&logPtr->lock);
        if (status != NS_OK) {
            return TCL_ERROR;
        }
        break;
    }

    return TCL_OK;
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
    Log         *logPtr = arg;
    CONST char **h;
    char        *p, *user, buffer[PIPE_BUF], *bufferPtr = NULL;
    int          n, status, i, fd;
    size_t	 bufferSize;
    Ns_DString   ds;
    Ns_Time      now, reqTime;

    Ns_DStringInit(&ds);
    Ns_MutexLock(&logPtr->lock);

    /*
     * Compute the request's elapsed time
     */

    if ((logPtr->flags & LOG_REQTIME)) {
        Ns_GetTime(&now);
        Ns_DiffTime(&now, Ns_ConnStartTime(conn), &reqTime);
    }

    /*
     * Append the peer address. Watch for users coming
     * from proxy servers (if configured).
     */

    p = NULL;
    if ((logPtr->flags & LOG_CHECKFORPROXY)) {
        p = Ns_SetIGet(conn->headers, "X-Forwarded-For");
        if (p != NULL && !strcasecmp(p, "unknown")) {
            p = NULL;
        }
    }
    Ns_DStringAppend(&ds, p && *p ? p : Ns_ConnPeer(conn));

    /*
     * Append the authorized user, if any. Watch usernames
     * with embedded blanks; we must properly quote them.
     */

    user = Ns_ConnAuthUser(conn);
    if (user == NULL) {
        Ns_DStringAppend(&ds," - - ");
    } else {
        int quote = 0;
        for (p = user; *p && !quote; p++) {
            quote = isspace((unsigned char)*p);
        }
        if (quote) {
            Ns_DStringVarAppend(&ds, " - \"", user, "\" ", NULL);
        } else {
            Ns_DStringVarAppend(&ds," - ", user, " ", NULL);
        }
    }

    /*
     * Append a common log format time stamp including GMT offset
     */

    if (!(logPtr->flags & LOG_FMTTIME)) {
        Ns_DStringPrintf(&ds, "[%" PRIu64 "]", (int64_t) time(NULL));
    } else {
        char buf[41]; /* Big enough for Ns_LogTime(). */
        Ns_LogTime(buf);
        Ns_DStringAppend(&ds, buf);
    }

    /*
     * Append the request line plus query data (if configured)
     */

    if (conn->request != NULL) {
        if ((logPtr->flags & LOG_SUPPRESSQUERY)) {
            Ns_DStringVarAppend(&ds, " \"", conn->request->url, "\" ", NULL);
        } else {
            Ns_DStringVarAppend(&ds, " \"", conn->request->line, "\" ", NULL);
        }
    } else {
        Ns_DStringAppend(&ds," \"\" ");
    }

    /*
     * Construct and append the HTTP status code and bytes sent
     */

    n = Ns_ConnResponseStatus(conn);
    Ns_DStringPrintf(&ds, "%d %" TCL_LL_MODIFIER "d",
                     n ? n : 200, Ns_ConnContentSent(conn));

    /*
     * Append the referer and user-agent headers (if any)
     */

    if ((logPtr->flags & LOG_COMBINED)) {
        Ns_DStringAppend(&ds, " \"");
        p = Ns_SetIGet(conn->headers, "referer");
        if (p) {
            Ns_DStringAppend(&ds, p);
        }
        Ns_DStringAppend(&ds, "\" \"");
        p = Ns_SetIGet(conn->headers, "user-agent");
        if (p) {
            Ns_DStringAppend(&ds, p);
        }
        Ns_DStringAppend(&ds, "\"");
    }

    /*
     * Append the request's elapsed time and queue time (if enabled)
     */

    if ((logPtr->flags & LOG_REQTIME)) {
        Ns_DStringPrintf(&ds, " %" PRIu64 ".%06ld", (int64_t) reqTime.sec, reqTime.usec);
    }

    if ((logPtr->flags & LOG_QUEUETIME)) {
      	Ns_Time totalQueueTime;
        Ns_Time *acceptTimePtr  = Ns_ConnAcceptTime(conn);
        Ns_Time *dequeueTimePtr = Ns_ConnDequeueTime(conn);

	Ns_DiffTime(dequeueTimePtr, acceptTimePtr, &totalQueueTime);
        Ns_DStringPrintf(&ds, " %" PRIu64 ".%06ld", (int64_t) totalQueueTime.sec, totalQueueTime.usec);
    }

    /*
     * Append the extended headers (if any)
     */

    for (h = logPtr->extheaders; *h != NULL; h++) {
        p = Ns_SetIGet(conn->headers, *h);
        if (p == NULL) {
            p = "";
        }
        Ns_DStringVarAppend(&ds, " \"", p, "\"", NULL);
    }

    for (i=0; i<ds.length; i++) {
      /* 
       * Quick fix to disallow terminal escape characters in the log
       * file. See e.g. http://www.securityfocus.com/bid/37712/info
       */
      if (ds.string[i] == 0x1b) {
	ds.string[i] = 7; /* bell */
      }
    }

    /*
     * Append the trailing newline and optionally
     * flush the buffer
     */

    Ns_DStringAppend(&ds, "\n");

    if (logPtr->maxlines == 0) {
        bufferSize = ds.length ;
	if (bufferSize < PIPE_BUF) {
	  /* only those write() operations are guaranteed to be atomic */
	    bufferPtr = ds.string;
            status = NS_OK;
	} else {
	    status = LogFlush(logPtr, &ds);
	}
    } else {
        Ns_DStringNAppend(&logPtr->buffer, ds.string, ds.length);
        if (++logPtr->curlines > logPtr->maxlines) {
	    bufferSize = logPtr->buffer.length;
            if (bufferSize < PIPE_BUF) {
              /* only those write() are guaranteed to be atomic */
              /* in most cases, we will fall into the other branch */
	      memcpy(buffer, logPtr->buffer.string, bufferSize);  
	      bufferPtr = buffer;
	      Ns_DStringTrunc(&logPtr->buffer, 0);
              status = NS_OK;
	    } else {
	      status = LogFlush(logPtr, &logPtr->buffer);
	    }
            logPtr->curlines = 0;
        } else {
            status = NS_OK;
        }
    }
    ((void)(status)); /* ignore status */

    fd = logPtr->fd;
    Ns_MutexUnlock(&logPtr->lock);

    if (bufferPtr) {
        if (fd >= 0 && write(fd, bufferPtr, bufferSize) != bufferSize) {
	    Ns_Log(Error, "nslog: write() failed: '%s'", strerror(errno));
	}
    }
    Ns_DStringFree(&ds);
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

static int
LogOpen(Log *logPtr)
{
    int fd;

    fd = open(logPtr->file, O_APPEND|O_WRONLY|O_CREAT, 0644);
    if (fd == -1) {
        Ns_Log(Error,"nslog: error '%s' opening '%s'",
               strerror(errno), logPtr->file);
        return NS_ERROR;
    }
    if (logPtr->fd >= 0) {
        close(logPtr->fd);
    }

    logPtr->fd = fd;
    Ns_Log(Notice,"nslog: opened '%s'", logPtr->file);

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

static int
LogClose(Log *logPtr)
{
    int status = NS_OK;

    if (logPtr->fd >= 0) {
        status = LogFlush(logPtr, &logPtr->buffer);
        close(logPtr->fd);
        logPtr->fd = -1;
        Ns_DStringFree(&logPtr->buffer);
        Ns_Log(Notice,"nslog: closed '%s'", logPtr->file);
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

static int
LogFlush(Log *logPtr, Ns_DString *dsPtr)
{
    int   len = dsPtr->length;
    char *buf = dsPtr->string;

    if (len > 0) {
        if (logPtr->fd >= 0 && write(logPtr->fd, buf, len) != len) {
            Ns_Log(Error, "nslog: logging disabled: write() failed: '%s'",
                   strerror(errno));
            close(logPtr->fd);
            logPtr->fd = -1;
        }
        Ns_DStringTrunc(dsPtr, 0);
    }

    return (logPtr->fd == -1) ? NS_ERROR : NS_OK;
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

static int
LogRoll(Log *logPtr)
{
    int      status;
    Tcl_Obj *path, *newpath;

    LogClose(logPtr);

    path = Tcl_NewStringObj(logPtr->file, -1);
    Tcl_IncrRefCount(path);
    status = Tcl_FSAccess(path, F_OK);

    if (status == 0) {

        /*
         * We are already logging to some file
         */

        if (logPtr->rollfmt == NULL) {
            status = Ns_RollFile(logPtr->file, logPtr->maxbackup);
        } else {
            time_t      now = time(0);
            char        timeBuf[512];
            Ns_DString  ds;
            struct tm  *ptm = ns_localtime(&now);

            strftime(timeBuf, sizeof(timeBuf)-1, logPtr->rollfmt, ptm);
            Ns_DStringInit(&ds);
            Ns_DStringVarAppend(&ds, logPtr->file,".", timeBuf, NULL);
            newpath = Tcl_NewStringObj(ds.string, -1);
            Tcl_IncrRefCount(newpath);
            status = Tcl_FSAccess(newpath, F_OK);
            if (status == 0) {
                status = Ns_RollFile(ds.string, logPtr->maxbackup);
            } else if (Tcl_GetErrno() != ENOENT) {
                Ns_Log(Error, "nslog: access(%s, F_OK) failed: '%s'",
                       ds.string, strerror(Tcl_GetErrno()));
                status = NS_ERROR;
            }
            if (status == NS_OK && Tcl_FSRenameFile(path, newpath)) {
                Ns_Log(Error, "nslog: rename(%s,%s) failed: '%s'",
                       logPtr->file, ds.string, strerror(Tcl_GetErrno()));
                status = NS_ERROR;
            }
            Tcl_DecrRefCount(newpath);
            Ns_DStringFree(&ds);
            if (status == NS_OK) {
                status = Ns_PurgeFiles(logPtr->file, logPtr->maxbackup);
            }
        }
    }

    Tcl_DecrRefCount(path);

    return (status == NS_OK) ? LogOpen(logPtr) : NS_ERROR;
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
LogCallback(int(proc)(Log *), void *arg, char *desc)
{
    int  status;
    Log *logPtr = arg;

    Ns_MutexLock(&logPtr->lock);
    status =(*proc)(logPtr);
    Ns_MutexUnlock(&logPtr->lock);

    if (status != NS_OK) {
        Ns_Log(Error,"nslog: failed: %s '%s': '%s'", desc, logPtr->file,
               strerror(Tcl_GetErrno()));
    }
}

static void
LogCloseCallback(Ns_Time *toPtr, void *arg)
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
LogArg(Tcl_DString *dsPtr, void *arg)
{
    Log *logPtr = arg;

    Tcl_DStringAppendElement(dsPtr, logPtr->file);
}
