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
 * log.c --
 *
 *      Manage the global error log file.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


/*
 * The following define available flags bits.
 */

#define LOG_ROLL   0x01
#define LOG_EXPAND 0x02
#define LOG_USEC   0x04

/*
 * The following struct represents a log entry header as stored in
 * the per-thread cache. It is followed by a variable-length log
 * string as passed by the caller (format expanded).
 */

typedef struct LogEntry {
    Ns_LogSeverity   severity;  /* Entry's severity */
    Ns_Time          stamp;     /* Timestamp of the entry */
    int              offset;    /* Offset into the text buffer */
    int              length;    /* Length of the log message */
    struct LogEntry *nextPtr;   /* Next in the list of entries */
} LogEntry;

/*
 * The following struct represents one registered log callback.
 * Log callbacks are used to produce log lines into the log sinks
 * and are invoked one by one for every log entry in the cache.
 * Each callback (usually) maintain its own log sink.
 */

typedef struct LogClbk {
    Ns_LogFilter    *proc;    /* User-given function for generating logs */
    Ns_Callback     *free;    /* User-given function to free passed arg */
    void            *arg;     /* Argument passed to proc and free */
    int              refcnt;  /* Number of current consumers */
    struct LogClbk  *nextPtr; /* Maintains double linked list */
    struct LogClbk  *prevPtr;
} LogClbk;

/*
 * The following struct maintains per-thread cached log entries.
 * The cache is a simple dynamic string where variable-length
 * LogEntry'ies (see below) are appended, one after another.
 */

typedef struct LogCache {
    int         hold;         /* Flag: keep log entries in cache */
    int         count;        /* Number of entries held in the cache */
    time_t      gtime;        /* For GMT time calculation */
    time_t      ltime;        /* For local time calculations */
    char        gbuf[100];    /* Buffer for GMT time string rep */
    char        lbuf[100];    /* Buffer for local time string rep */
    LogEntry   *firstEntry;   /* First in the list of log entries */
    LogEntry   *currEntry;    /* Current in the list of log entries */
    Ns_DString  buffer;       /* The log entries cache text-cache */
} LogCache;

/*
 * Local functions defined in this file
 */

static int   LogOpen(void);
static void  LogAdd(Ns_LogSeverity sev, CONST char *fmt, va_list ap);
static void  LogFlush(LogCache *cachePtr, LogClbk *list, int cnt,
                      int trunc, int lock);
static char* LogTime(LogCache *cachePtr, Ns_Time *timePtr, int gmt);

static LogClbk* AddClbk(Ns_LogFilter *proc, void *arg, Ns_Callback *free);
static void     RemClbk(LogClbk *clbkPtr, int unlocked);

static char* SeverityName(Ns_LogSeverity sev, char *buf);

static LogCache* GetCache(void);
static Ns_TlsCleanup FreeCache;
static Tcl_PanicProc Panic;

static Ns_LogFilter LogToFile;
static Ns_LogFilter LogToTcl;
static Ns_LogFilter LogToDString;

/*
 * Static variables defined in this file
 */

static Ns_Tls       tls;
static Ns_Mutex     lock;
static Ns_Cond      cond;
static CONST char  *file;
static int          flags;
static int          maxback;
static int          maxlevel;
static LogClbk     *callbacks;
static CONST char  *logClbkAddr = "ns:logcallback";

/*
 * The following table defines which severity levels
 * are currently active.
 *
 * Be sure to keep the order in sync with the Ns_LogSeverity enum.
 */

static struct {
    char   *string;
    int     enabled;
} logConfig[] = {
    { "Notice",  NS_TRUE  },
    { "Warning", NS_TRUE  },
    { "Error",   NS_TRUE  },
    { "Fatal",   NS_TRUE  },
    { "Bug",     NS_TRUE  },
    { "Debug",   NS_FALSE },
    { "Dev",     NS_FALSE }
};

/*
 * The following table converts from severity string names to
 * an Ns_LogSeverity enum value.
 */

static struct {
    char           *string;
    Ns_LogSeverity  severity;
} severityTable[] = {
    { "notice",  Notice  }, { "Notice",  Notice  },
    { "warning", Warning }, { "Warning", Warning },
    { "error",   Error   }, { "Error",   Error   },
    { "fatal",   Fatal   }, { "Fatal",   Fatal   },
    { "bug",     Bug     }, { "Bug",     Bug     },
    { "debug",   Debug   }, { "Debug",   Debug   },
    { "dev",     Dev     }, { "Dev",     Dev     },
    { NULL, 0 }
};



/*
 *----------------------------------------------------------------------
 *
 * NsInitLog --
 *
 *      Initialize the log API and TLS slot.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsInitLog(void)
{
    Ns_MutexSetName(&lock, "ns:log");
    Ns_TlsAlloc(&tls, FreeCache);
    Tcl_SetPanicProc(Panic);
    AddClbk(LogToFile, (void*)STDERR_FILENO, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsConfigLog --
 *
 *      Config the logging interface.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on config file.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigLog(void)
{
    Ns_DString  ds;
    CONST char *path = NS_CONFIG_PARAMETERS;

    logConfig[Debug ].enabled = Ns_ConfigBool(path, "logdebug",  NS_FALSE);
    logConfig[Dev   ].enabled = Ns_ConfigBool(path, "logdev",    NS_FALSE);
    logConfig[Notice].enabled = Ns_ConfigBool(path, "lognotice", NS_TRUE);

    if (Ns_ConfigBool(path, "logroll", NS_TRUE)) {
        flags |= LOG_ROLL;
    }
    if (Ns_ConfigBool(path, "logusec", NS_FALSE)) {
        flags |= LOG_USEC;
    }
    if (Ns_ConfigBool(path, "logexpanded", NS_FALSE)) {
        flags |= LOG_EXPAND;
    }

    maxback  = Ns_ConfigIntRange(path, "logmaxbackup", 10, 0, 999);
    maxlevel = Ns_ConfigInt(path, "logmaxlevel", INT_MAX);

    file = Ns_ConfigString(path, "serverlog", "logs/nsd.log");
    if (!Ns_PathIsAbsolute(file)) {
        Ns_DStringInit(&ds);
        Ns_HomePath(&ds, file, NULL);
        file = Ns_DStringExport(&ds);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoErrorLog --
 *
 *      Returns the filename of the log file.
 *
 * Results:
 *      Log file name or NULL if none.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoErrorLog(void)
{
    return (char*) file;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogRoll --
 *
 *      Signal handler for SIG_HUP which will roll the files.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      Will rename the log file and reopen it.
 *
 *----------------------------------------------------------------------
 */

int
Ns_LogRoll(void)
{
    if (file != NULL) {
        if (access(file, F_OK) == 0) {
            Ns_RollFile(file, maxback);
        }
        Ns_Log(Notice, "log: re-opening log file '%s'", file);
        if (LogOpen() != NS_OK) {
            return NS_ERROR;
        }
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Log --
 *
 *      Send a message to the server log.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_Log(Ns_LogSeverity severity, CONST char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    LogAdd(severity, fmt, ap);
    va_end(ap);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_VALog --
 *
 *      Send a message to the server log (varargs interface)
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_VALog(Ns_LogSeverity severity, CONST char *fmt, va_list *vaPtr)
{
    LogAdd(severity, fmt, *vaPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Fatal --
 *
 *      Send a message to the server log with severity level Fatal,
 *      and then exit the nsd process cleanly.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The process will exit.
 *
 *----------------------------------------------------------------------
 */

void
Ns_Fatal(CONST char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    LogAdd(Fatal, fmt, ap);
    va_end(ap);

    _exit(1);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogTime2, Ns_LogTime --
 *
 *      Copy a local or GMT date and time string useful for common log
 *      format enties (e.g., nslog).
 *
 * Results:
 *      Pointer to given buffer.
 *
 * Side effects:
 *      Will put data into timeBuf, which must be at least 41 bytes
 *      long.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_LogTime(char *timeBuf)
{
    return Ns_LogTime2(timeBuf, 1);
}

char *
Ns_LogTime2(char *timeBuf, int gmt)
{
    Ns_Time now;

    Ns_GetTime(&now);
    return strcpy(timeBuf, LogTime(GetCache(), &now, gmt));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetLogFlushProc, Ns_SetNsLogProc --
 *
 *      Deprecated (and disabled) calls.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will exit the server.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetLogFlushProc(Ns_LogFlushProc *procPtr)
{
    Ns_Fatal("Ns_SetLogFlushProc: deprecated, use Ns_AddLogFilter() instead");
}

void
Ns_SetNsLogProc(Ns_LogProc *procPtr)
{
    Ns_Fatal("Ns_SetNsLogProc: deprecated, use Ns_AddLogFilter() instead");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AddLogFilter --
 *
 *      Adds one user-given log filter.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_AddLogFilter(Ns_LogFilter *procPtr, void *arg, Ns_Callback *freePtr)
{
    AddClbk(procPtr, arg, freePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_RemoveLogFilter --
 *
 *      Removes user-given log filter.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RemoveLogFilter(Ns_LogFilter *procPtr, void *arg)
{
    LogClbk *clbkPtr;

    Ns_MutexLock(&lock);
    clbkPtr = callbacks;
    while(clbkPtr != NULL) {
        if (clbkPtr->proc == procPtr && clbkPtr->arg == arg) {
            break;
        }
        clbkPtr = clbkPtr->prevPtr;
    }
    if (clbkPtr != NULL) {
        RemClbk(clbkPtr, 1);
    }
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsLogOpen --
 *
 *      Open the log file. Adjust configurable parameters, too.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Configures this module to use the newly opened log file.
 *      If LogRoll is turned on in the config file, then it registers
 *      a signal callback.
 *
 *----------------------------------------------------------------------
 */

void
NsLogOpen(void)
{
    /*
     * Open the log and schedule the signal roll.
     */

    if (LogOpen() != NS_OK) {
        Ns_Fatal("log: failed to open server log '%s': '%s'",
                 file, strerror(errno));
    }
    if (flags & LOG_ROLL) {
        Ns_RegisterAtSignal((Ns_Callback *) Ns_LogRoll, NULL);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLogRollObjCmd --
 *
 *      Implements ns_logroll command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclLogRollObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                   Tcl_Obj *CONST objv[])
{
    if (Ns_LogRoll() != NS_OK) {
        Tcl_SetResult(interp, "could not roll server log", TCL_STATIC);
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLogCtlObjCmd --
 *
 *      Implements ns_logctl command to manage per-thread log caching.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclLogCtlObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                  Tcl_Obj *CONST objv[])
{
    int             count, opt, i, bool;
    Ns_DString      ds;
    Ns_LogSeverity  severity;
    LogCache       *cachePtr = GetCache();
    LogClbk         clbk, *clbkPtr = &clbk;

    static CONST char *opts[] = {
        "hold", "count", "get", "peek", "flush", "release",
        "truncate", "severity", "register", "unregister", NULL
    };
    enum {
        CHoldIdx, CCountIdx, CGetIdx, CPeekIdx, CFlushIdx, CReleaseIdx,
        CTruncIdx, CSeverityIdx, CRegisterIdx, CUnregisterIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case CRegisterIdx:
        if (objc < 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "script ?arg?");
            return TCL_ERROR;
        }
        clbkPtr = AddClbk(LogToTcl,
                          Ns_TclNewCallback(interp, Ns_TclCallbackProc,
                                            objv[2], objc - 3, objv + 3),
                          Ns_TclFreeCallback);
        Ns_TclSetAddrObj(Tcl_GetObjResult(interp), logClbkAddr,(void*)clbkPtr);
        break;

    case CUnregisterIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle");
            return TCL_ERROR;
        }
        if (Ns_TclGetAddrFromObj(interp, objv[2], logClbkAddr,
                                 (void *)&clbkPtr) != TCL_OK) {
            return TCL_ERROR;
        }
        RemClbk(clbkPtr, 0);
        break;

    case CHoldIdx:
        cachePtr->hold = 1;
        break;

    case CPeekIdx:
        memset(clbkPtr, 0, sizeof(LogClbk));
        clbkPtr->proc = LogToDString;
        clbkPtr->arg  = (void*)&ds;
        Ns_DStringInit(&ds);
        LogFlush(cachePtr, clbkPtr, -1, 0, 0);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DStringValue(&ds),-1));
        Ns_DStringFree(&ds);
        break;

    case CGetIdx:
        memset(clbkPtr, 0, sizeof(LogClbk));
        clbkPtr->proc = LogToDString;
        clbkPtr->arg  = (void*)&ds;
        Ns_DStringInit(&ds);
        LogFlush(cachePtr, clbkPtr, -1, 1, 0);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DStringValue(&ds),-1));
        Ns_DStringFree(&ds);
        break;

    case CReleaseIdx:
        cachePtr->hold = 0;
        /* FALLTHROUGH */

    case CFlushIdx:
        LogFlush(cachePtr, callbacks, -1, 1, 1);
        break;

    case CCountIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(cachePtr->count));
        break;

    case CTruncIdx:
        count = 0;
        if (objc > 2 && Tcl_GetIntFromObj(interp, objv[2], &count) != TCL_OK) {
            return TCL_ERROR;
        }
        memset(clbkPtr, 0, sizeof(LogClbk));
        LogFlush(cachePtr, clbkPtr, count, 1, 0);
        break;

    case CSeverityIdx:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "severity-level ?bool?");
            return TCL_ERROR;
        }
        if (Tcl_GetIndexFromObjStruct(interp, objv[2], severityTable,
                                      sizeof(severityTable[0]), "severity",
                                      TCL_EXACT, &i) != TCL_OK) {
            return TCL_ERROR;
        }
        severity = severityTable[i].severity;
        if (objc == 4) {
            if (Tcl_GetBooleanFromObj(interp, objv[3], &bool) != TCL_OK) {
                return TCL_ERROR;
            }
            logConfig[severity].enabled = bool;
        } else {
            bool = logConfig[severity].enabled;
        }
        Tcl_SetObjResult(interp,Tcl_NewBooleanObj(bool));
        break;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLogObjCmd --
 *
 *      Implements ns_log as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclLogObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
               Tcl_Obj *CONST objv[])
{
    Ns_LogSeverity severity;
    Ns_DString     ds;
    int            i;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "severity string ?string ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObjStruct(NULL, objv[1], severityTable,
                                  sizeof(severityTable[0]), "severity",
                                  TCL_EXACT, &i) == TCL_OK) {
        severity = severityTable[i].severity;
    } else if (Tcl_GetIntFromObj(NULL, objv[1], &i) == TCL_OK) {
        severity = i;
    } else {
        Tcl_AppendResult(interp, "unknown severity: \"",
                         Tcl_GetString(objv[1]),
                         "\": should be notice, warning, error, "
                         "fatal, bug, debug, dev or integer value", NULL);
        return TCL_ERROR;
    }

    if (objc == 3) {
        Ns_Log(severity, "%s", Tcl_GetString(objv[2]));
    } else {
        Ns_DStringInit(&ds);
        for (i = 2; i < objc; ++i) {
            Ns_DStringVarAppend(&ds, Tcl_GetString(objv[i]),
                                i < (objc-1) ? " " : NULL, NULL);
        }
        Ns_Log(severity, "%s", Ns_DStringValue(&ds));
        Ns_DStringFree(&ds);
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogAdd --
 *
 *      Add an entry to the log cache if the severity is not surpressed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May write immediately or later through cache.
 *
 *----------------------------------------------------------------------
 */

static void
LogAdd(Ns_LogSeverity severity, CONST char *fmt, va_list ap)
{
    int       length, offset;
    LogCache *cachePtr;
    LogEntry *entryPtr = NULL;

    /*
     * Skip if logging for selected severity is disabled
     * or if severity level out of range(s).
     */

    if ((maxlevel && severity > maxlevel)
        || severity > (sizeof(logConfig)/sizeof(logConfig[0]))
        || logConfig[severity].enabled == 0) {
        return;
    }

    cachePtr = GetCache();

    /*
     * Append new or reuse log entry record.
     */

    if (cachePtr->currEntry != NULL) {
        entryPtr = cachePtr->currEntry->nextPtr;
    } else {
        entryPtr = cachePtr->firstEntry;
    }
    if (entryPtr == NULL) {
        entryPtr = ns_malloc(sizeof(LogEntry));
        entryPtr->nextPtr = NULL;
        if (cachePtr->currEntry != NULL) {
            cachePtr->currEntry->nextPtr = entryPtr;
        } else {
            cachePtr->firstEntry = entryPtr;
        }
    }

    cachePtr->currEntry = entryPtr;
    cachePtr->count++;

    offset = Ns_DStringLength(&cachePtr->buffer);
    Ns_DStringVPrintf(&cachePtr->buffer, fmt, ap);
    length = Ns_DStringLength(&cachePtr->buffer) - offset;

    entryPtr->severity = severity;
    entryPtr->offset   = offset;
    entryPtr->length   = length;
    Ns_GetTime(&entryPtr->stamp);

    /*
     * Flush it out if not held
     */

    if (!cachePtr->hold) {
        LogFlush(cachePtr, callbacks, -1, 1, 1);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * LogFlush --
 *
 *      Flush per-thread log cache, optionally truncating the
 *      cache to some given count of log entries.
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
LogFlush(LogCache *cachePtr, LogClbk *listPtr, int count, int trunc, int locked)
{
    int       status, nentry = 0;
    char     *log;
    LogEntry *ePtr;
    LogClbk  *cPtr;

    ePtr = cachePtr->firstEntry;
    while (ePtr != NULL && cachePtr->currEntry) {
        log = Ns_DStringValue(&cachePtr->buffer) + ePtr->offset;
        if (locked) {
            Ns_MutexLock(&lock);
        }
        cPtr = listPtr;
        while (cPtr != NULL) {
            if (cPtr->proc != NULL) {
                if (locked) {
                    cPtr->refcnt++;
                    Ns_MutexUnlock(&lock);
                }
                status = (*cPtr->proc)(cPtr->arg, ePtr->severity,
                                       &ePtr->stamp, log, ePtr->length);
                if (locked) {
                    Ns_MutexLock(&lock);
                    cPtr->refcnt--;
                    Ns_CondBroadcast(&cond);
                }
                if (status == NS_ERROR) {
                    /*
                     * Callback signalized error. Per definition we will
                     * skip invoking other registered callbacks. In such
                     * case we must assure that the current log entry
                     * eventually gets written into some log sink, so we
                     * use the default logfile sink.
                     */
                    LogToFile((void*)STDERR_FILENO, ePtr->severity,
                              &ePtr->stamp, log, ePtr->length);
                    break;
                }
            }
            cPtr = cPtr->prevPtr;
        }
        if (locked) {
            Ns_MutexUnlock(&lock);
        }
        nentry++;
        if ((count > 0 && nentry >= count) || ePtr == cachePtr->currEntry) {
            break;
        }
        ePtr = ePtr->nextPtr;
    }

    if (trunc) {
        if (count > 0) {
            int length = ePtr ? ePtr->offset + ePtr->length : 0;
            cachePtr->count = length ? nentry : 0;
            cachePtr->currEntry = ePtr;
            Ns_DStringSetLength(&cachePtr->buffer, length);
        } else {
            cachePtr->count = 0;
            cachePtr->currEntry = NULL;
            Ns_DStringSetLength(&cachePtr->buffer, 0);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * LogOpen --
 *
 *      Open the log file name specified in the 'logFile' global. If
 *      it's successfully opened, make that file the sink for stdout
 *      and stderr too.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LogOpen(void)
{
    int fd, status = NS_OK;

    fd = open(file, O_WRONLY|O_APPEND|O_CREAT|O_LARGEFILE, 0644);
    if (fd == -1) {
    	Ns_Log(Error, "log: failed to re-open log file '%s': '%s'",
               file, strerror(errno));
        status = NS_ERROR;
    } else {

        /*
         * Route stderr to the file
         */

        if (fd != STDERR_FILENO && dup2(fd, STDERR_FILENO) == -1) {
            fprintf(stdout, "dup2(%s, STDERR_FILENO) failed: %s\n",
                    file, strerror(errno));
            status = NS_ERROR;
        }

        /*
         * Route stdout to the file
         */

        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            Ns_Log(Error, "log: failed to route stdout to file: '%s'",
                   strerror(errno));
            status = NS_ERROR;
        }

        /*
         * Clean up dangling 'open' reference to the fd
         */

        if (fd != STDERR_FILENO && fd != STDOUT_FILENO) {
            close(fd);
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * LogTime --
 *
 *      Returns formatted local or gmt time from per-thread cache
 *
 * Results:
 *      Pointer to per-thread buffer.
 *
 * Side effects:
 *      The per-thread cache is updated with second resolution.
 *
 *----------------------------------------------------------------------
 */

static char *
LogTime(LogCache *cachePtr, Ns_Time *timePtr, int gmt)
{
    time_t    *tp;
    struct tm *ptm;
    int        gmtoff, n, sign;
    char      *bp;

    if (gmt) {
        tp = &cachePtr->gtime;
        bp = cachePtr->gbuf;
    } else {
        tp = &cachePtr->ltime;
        bp = cachePtr->lbuf;
    }
    if (*tp != timePtr->sec) {
        *tp = timePtr->sec;
        ptm = ns_localtime(&timePtr->sec);
        n = strftime(bp, 32, "[%d/%b/%Y:%H:%M:%S", ptm);
        if (!gmt) {
            bp[n++] = ']';
            bp[n] = '\0';
        } else {
#ifdef HAVE_TM_GMTOFF
            gmtoff = ptm->tm_gmtoff / 60;
#else
            gmtoff = -timezone / 60;
            if (daylight && ptm->tm_isdst) {
                gmtoff += 60;
            }
#endif
            if (gmtoff < 0) {
                sign = '-';
                gmtoff *= -1;
            } else {
                sign = '+';
            }
            sprintf(bp + n, " %c%02d%02d]", sign, gmtoff/60, gmtoff%60);
        }
    }

    return bp;
}


/*
 *----------------------------------------------------------------------
 *
 * GetCache --
 *
 *      Get the per-thread LogCache struct.
 *
 * Results:
 *      Pointer to per-thread struct.
 *
 * Side effects:
 *      Memory for struct is allocated on first call.
 *
 *----------------------------------------------------------------------
 */

static LogCache *
GetCache(void)
{
    LogCache *cachePtr;

    cachePtr = Ns_TlsGet(&tls);
    if (cachePtr == NULL) {
        cachePtr = ns_calloc(1, sizeof(LogCache));
        Ns_DStringInit(&cachePtr->buffer);
        Ns_TlsSet(&tls, cachePtr);
    }

    return cachePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeCache --
 *
 *      TLS cleanup callback to destory per-thread Cache struct.
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
FreeCache(void *arg)
{
    LogCache *cachePtr = (LogCache *)arg;
    LogEntry *entryPtr, *tmpPtr;

    LogFlush(cachePtr, callbacks, -1, 1, 1);
    entryPtr = cachePtr->firstEntry;
    while (entryPtr != NULL) {
        tmpPtr = entryPtr->nextPtr;
        ns_free(entryPtr);
        entryPtr = tmpPtr;
    }
    Ns_DStringFree(&cachePtr->buffer);
    ns_free(cachePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Panic --
 *
 *      Tcl_PanicProc callback which sends a message to the server log
 *      with severity level Fatal, and then kills the process immediately.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The process dies, possibly entering the debugger.
 *
 *----------------------------------------------------------------------
 */

static void
Panic(CONST char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    LogAdd(Fatal, fmt, ap);
    va_end(ap);

    abort();
}


/*
 *----------------------------------------------------------------------
 *
 * AddClbk --
 *
 *      Adds one user-given callback to list of registered callbacks.
 *      The callback is placed at the tail of the linked list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static LogClbk *
AddClbk(Ns_LogFilter *proc, void *arg, Ns_Callback *free)
{
    LogClbk *clbkPtr = (LogClbk *)ns_calloc(1, sizeof(LogClbk));

    Ns_MutexLock(&lock);

    if (callbacks != NULL) {
        callbacks->nextPtr = clbkPtr;
        clbkPtr->prevPtr = callbacks;
    } else {
        clbkPtr->prevPtr = NULL;
    }

    clbkPtr->nextPtr = NULL;
    callbacks = clbkPtr;

    clbkPtr->proc = proc;
    clbkPtr->arg  = arg;
    clbkPtr->free = free;

    Ns_MutexUnlock(&lock);

    return clbkPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * RemClbk --
 *
 *      Removes one user-given callback from the list of registered
 *      callbacks.
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
RemClbk(LogClbk *clbkPtr, int unlocked)
{
    if (!unlocked) {
        Ns_MutexLock(&lock);
    }
    while (clbkPtr->refcnt > 0) {
        Ns_CondWait(&cond, &lock);
    }
    if (clbkPtr->prevPtr != NULL) {
        clbkPtr->prevPtr->nextPtr = clbkPtr->nextPtr;
    }
    if (clbkPtr->nextPtr != NULL) {
        clbkPtr->nextPtr->prevPtr = clbkPtr->prevPtr;
    } else {
        callbacks = clbkPtr->prevPtr;
    }
    if (clbkPtr->free != NULL && clbkPtr->arg != NULL) {
        (*clbkPtr->free)(clbkPtr->arg);
    }

    ns_free(clbkPtr);

    if (!unlocked) {
        Ns_MutexUnlock(&lock);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * LogToDString --
 *
 *      Callback to write the log line to the passed dynamic string.
 *
 * Results:
 *      Standard NS result code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LogToDString(void *arg, Ns_LogSeverity severity, Ns_Time *stamp,
            char *msg, int len)
{
    Ns_DString *dsPtr  = (Ns_DString *)arg;
    char       sevname[32];

    /*
     * Add the log stamp
     */

    Ns_DStringAppend(dsPtr, LogTime(GetCache(), stamp, 0));
    if (flags & LOG_USEC) {
        Ns_DStringSetLength(dsPtr, Ns_DStringLength(dsPtr) - 1);
        Ns_DStringPrintf(dsPtr, ".%ld]", stamp->usec);
    }
    Ns_DStringPrintf(dsPtr, "[%d.%lu][%s] %s: ", Ns_InfoPid(),
                     (unsigned long) Ns_ThreadId(), Ns_ThreadGetName(),
                     SeverityName(severity, sevname));
    if (flags & LOG_EXPAND) {
        Ns_DStringAppend(dsPtr, "\n    ");
    }

    /*
     * Add the log message
     */

    if (len == -1) {
        len = strlen(msg);
    }
    Ns_DStringNAppend(dsPtr, msg, len);
    Ns_DStringNAppend(dsPtr, "\n", 1);
    if (flags & LOG_EXPAND) {
        Ns_DStringNAppend(dsPtr, "\n", 1);
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogToFile --
 *
 *      Callback to write the log line to the passed file descriptor.
 *
 * Results:
 *      Standard NS result code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
LogToFile(void *arg, Ns_LogSeverity severity, Ns_Time *stamp,
          char *msg, int len)
{
    int        ret, fd = (int)arg;
    Ns_DString ds;

    Ns_DStringInit(&ds);
    LogToDString((void*)&ds, severity, stamp, msg, len);
    ret = write(fd, Ns_DStringValue(&ds), (size_t)Ns_DStringLength(&ds));
    Ns_DStringFree(&ds);

    return ret < 0 ? NS_ERROR : NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * LogToTcl --
 *
 *      Callback to pass the log information to Tcl.
 *
 *      This function may return NS_ERROR in which case the
 *      caller should skip invoking other registered callbacks.
 *      In such case, a log-entry with the error message is
 *      produced in the default log sink (log file).
 *      For all other cases, caller should continue with the
 *      next registered callback.
 *
 * Results:
 *      Standard NS result code.
 *
 * Side effects:
 *      This call deliberately does not use Ns_TclEvalCallback(),
 *      as if the Tcl code throws error, that one will invoke
 *      Ns_TclLogError() and will deadlock in the log code.
 *
 *----------------------------------------------------------------------
 */

static int
LogToTcl(void *arg, Ns_LogSeverity severity, Ns_Time *stampPtr,
         char *msg, int len)
{
    int             ii, ret;
    char            c, sevname[32];
    void           *logfile = (void *)STDERR_FILENO;
    Tcl_Obj        *stamp;
    Ns_DString      ds;
    Tcl_Interp     *interp;
    Ns_TclCallback *cbPtr = (Ns_TclCallback *)arg;

    interp = Ns_TclAllocateInterp(cbPtr->server);
    if (interp == NULL) {
        char *err = "LogToTcl: can't get interpreter";
        LogToFile(logfile, Error, stampPtr, err, -1);
        return NS_ERROR;
    }

    Ns_DStringInit(&ds);
    stamp = Tcl_NewObj();
    Ns_TclSetTimeObj(stamp, stampPtr);

    /*
     * Construct args for passing to the callback script:
     *
     *      callback severity timestamp log ?arg...?
     *
     * The script may contain blanks therefore append
     * as regular string instead of as list element.
     * Other arguments are appended to it as elements.
     */

    Ns_DStringAppend(&ds, cbPtr->script);
    Ns_DStringAppendElement(&ds, SeverityName(severity, sevname));
    Ns_DStringAppendElement(&ds, Tcl_GetString(stamp));
    Tcl_DecrRefCount(stamp);
    c = *(msg + len);
    *(msg + len) = 0;
    Ns_DStringAppendElement(&ds, msg);
    *(msg + len) = c;
    for (ii = 0; ii < cbPtr->argc; ii++) {
        Ns_DStringAppendElement(&ds, cbPtr->argv[ii]);
    }
    ret = Tcl_EvalEx(interp, Ns_DStringValue(&ds), Ns_DStringLength(&ds), 0);
    if (ret == TCL_ERROR) {

        /*
         * Error in Tcl callback is always logged to file.
         */

        Ns_DStringSetLength(&ds, 0);
        Ns_DStringAppend(&ds, "LogToTcl: ");
        Ns_DStringAppend(&ds, Tcl_GetStringResult(interp));
        LogToFile(logfile, Error, stampPtr, Ns_DStringValue(&ds),
                  Ns_DStringLength(&ds));
    }
    Ns_DStringFree(&ds);
    Ns_TclDeAllocateInterp(interp);

    return (ret == TCL_ERROR) ? NS_ERROR: NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SeverityName --
 *
 *      Returns string representation of the log severity
 *
 * Results:
 *      Pointer to a string with the string rep.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char*
SeverityName(Ns_LogSeverity severity, char *buf)
{
    char *severityStr;

    if (severity < (sizeof(logConfig) / sizeof(logConfig[0]))) {
        severityStr = logConfig[severity].string;
    } else {
        severityStr = buf;
        sprintf(buf, "Level%d", severity);
    }

    return severityStr;
}

