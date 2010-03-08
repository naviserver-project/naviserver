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
 * Log filters are used to produce log lines into the log sinks
 * and are invoked one by one for every log entry in the cache.
 * Each filter (usually) maintain its own log sink.
 */

typedef struct LogFilter {
    Ns_LogFilter      *proc;    /* User-given function for generating logs */
    Ns_Callback       *free;    /* User-given function to free passed arg */
    void              *arg;     /* Argument passed to proc and free */
    int                refcnt;  /* Number of current consumers */
    struct LogFilter  *nextPtr; /* Maintains double linked list */
    struct LogFilter  *prevPtr;
} LogFilter;

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
static void  LogFlush(LogCache *cachePtr, LogFilter *list, int cnt,
                      int trunc, int lock);
static char* LogTime(LogCache *cachePtr, Ns_Time *timePtr, int gmt);

static int GetSeverityFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
                              Ns_LogSeverity *severityPtr);

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

static LogFilter   *filters;
static CONST char  *filterType = "ns:logfilter";
static CONST char  *severityType = "ns:logseverity";

/*
 * The following table defines which severity levels
 * are currently active. The order is important: keep
 * it in sync with the Ns_LogSeverity enum.
 *
 * "640 (slots) should be enough for enyone..."
 */

static struct {
    char   *string;
    int     enabled;
} severityConfig[640] = {
    { "Notice",  NS_TRUE  },
    { "Warning", NS_TRUE  },
    { "Error",   NS_TRUE  },
    { "Fatal",   NS_TRUE  },
    { "Bug",     NS_TRUE  },
    { "Debug",   NS_FALSE },
    { "Dev",     NS_FALSE }
};

static int severityCount = sizeof(severityConfig) / sizeof(severityConfig[0]);
static int severityIdx = 0;

static Tcl_HashTable severityTable; /* Map severity names to indexes for Tcl. */



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
    Tcl_HashEntry *hPtr;
    char           buf[20];
    int            i, new;

    Ns_MutexSetName(&lock, "ns:log");
    Ns_TlsAlloc(&tls, FreeCache);
    Tcl_InitHashTable(&severityTable, TCL_STRING_KEYS);

    Tcl_SetPanicProc(Panic);
    Ns_AddLogFilter(LogToFile, (void *) STDERR_FILENO, NULL);

    /*
     * Initialise the entire space with backwards-compatible integer keys.
     */

    for (i = Dev +1; i < severityCount; i++) {
        snprintf(buf, sizeof(buf), "%d", i);
        hPtr = Tcl_CreateHashEntry(&severityTable, buf, &new);
        Tcl_SetHashValue(hPtr, (ClientData)(intptr_t) i);
        severityConfig[i].string = Tcl_GetHashKey(&severityTable, hPtr);
        severityConfig[i].enabled = 0;
    }

    /*
     * Initialise the built-in severities and lower-case aliases.
     */

    for (i = 0; i < Dev +1; i++) {
        (void) Ns_CreateLogSeverity(severityConfig[i].string);

        strcpy(buf, severityConfig[i].string);
        hPtr = Tcl_CreateHashEntry(&severityTable, Ns_StrToLower(buf), &new);
        Tcl_SetHashValue(hPtr, (ClientData)(intptr_t) i);
    }
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

    severityConfig[Debug ].enabled = Ns_ConfigBool(path, "logdebug",  NS_FALSE);
    severityConfig[Dev   ].enabled = Ns_ConfigBool(path, "logdev",    NS_FALSE);
    severityConfig[Notice].enabled = Ns_ConfigBool(path, "lognotice", NS_TRUE);

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

    file = Ns_ConfigString(path, "serverlog", "nsd.log");
    if (!Ns_PathIsAbsolute(file)) {
        Ns_DStringInit(&ds);
        if (Ns_HomePathExists("logs", NULL)) {
            Ns_HomePath(&ds, "logs", file, NULL);
        } else {
            Ns_HomePath(&ds, file, NULL);
        }
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
 * Ns_CreateLogSeverity --
 *
 *      Create and return a new log severity with the given name, which
 *      will initially be dissabled (except for the built-ins).
 *
 * Results:
 *      The severity.
 *
 * Side effects:
 *      Sever will exit if max severities exceeded.
 *
 *----------------------------------------------------------------------
 */

Ns_LogSeverity
Ns_CreateLogSeverity(CONST char *name)
{
    Ns_LogSeverity  severity;
    Tcl_HashEntry  *hPtr;
    int             new;

    if (severityIdx >= severityCount) {
        Ns_Fatal("max log severities exceeded");
    }
    Ns_MutexLock(&lock);
    hPtr = Tcl_CreateHashEntry(&severityTable, name, &new);
    if (new) {
        severity = severityIdx++;
        Tcl_SetHashValue(hPtr, (ClientData)(intptr_t) severity);
        severityConfig[severity].string = Tcl_GetHashKey(&severityTable, hPtr);
    } else {
        severity = (int)(intptr_t) Tcl_GetHashValue(hPtr);
    }
    Ns_MutexUnlock(&lock);

    return severity;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogSeverityName --
 *
 *      Given a log severity, return a pointer to it's name.
 *
 * Results:
 *      The severity name.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Ns_LogSeverityName(Ns_LogSeverity severity)
{
    if (severity < severityCount) {
        return severityConfig[severity].string;
    }
    return "Unknown";
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogSeverityEnabled --
 *
 *      Return true if the given severity level is enabled.
 *
 * Results:
 *      NS_TRUE / NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_LogSeverityEnabled(Ns_LogSeverity severity)
{
    if (severity < severityCount) {
        return severityConfig[severity].enabled;
    }
    return NS_TRUE;
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
    Ns_VALog(severity, fmt, &ap);
    va_end(ap);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VALog --
 *
 *      Add an entry to the log cache if the severity is not surpressed.
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
    int       length, offset;
    LogCache *cachePtr;
    LogEntry *entryPtr = NULL;

    /*
     * Skip if logging for selected severity is disabled
     * or if severity level out of range(s).
     */

    if (!Ns_LogSeverityEnabled(severity)) {
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
    Ns_DStringVPrintf(&cachePtr->buffer, fmt, *vaPtr);
    length = Ns_DStringLength(&cachePtr->buffer) - offset;

    entryPtr->severity = severity;
    entryPtr->offset   = offset;
    entryPtr->length   = length;
    Ns_GetTime(&entryPtr->stamp);

    /*
     * Flush it out if not held
     */

    if (!cachePtr->hold || severity == Fatal) {
        LogFlush(cachePtr, filters, -1, 1, 1);
    }
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
Ns_AddLogFilter(Ns_LogFilter *proc, void *arg, Ns_Callback *free)
{
    LogFilter *filterPtr = ns_calloc(1, sizeof *filterPtr);

    Ns_MutexLock(&lock);

    if (filters != NULL) {
        filters->nextPtr = filterPtr;
        filterPtr->prevPtr = filters;
    } else {
        filterPtr->prevPtr = NULL;
    }

    filterPtr->nextPtr = NULL;
    filters = filterPtr;

    filterPtr->proc = proc;
    filterPtr->arg  = arg;
    filterPtr->free = free;

    Ns_MutexUnlock(&lock);
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
    LogFilter *filterPtr;

    Ns_MutexLock(&lock);
    filterPtr = filters;
    while(filterPtr != NULL) {
        if (filterPtr->proc == procPtr && filterPtr->arg == arg) {
            break;
        }
        filterPtr = filterPtr->prevPtr;
    }
    if (filterPtr != NULL) {
        while (filterPtr->refcnt > 0) {
            Ns_CondWait(&cond, &lock);
        }
        if (filterPtr->prevPtr != NULL) {
            filterPtr->prevPtr->nextPtr = filterPtr->nextPtr;
        }
        if (filterPtr->nextPtr != NULL) {
            filterPtr->nextPtr->prevPtr = filterPtr->prevPtr;
        } else {
            filters = filterPtr->prevPtr;
        }
        if (filterPtr->free != NULL && filterPtr->arg != NULL) {
            (*filterPtr->free)(filterPtr->arg);
        }
        ns_free(filterPtr);
    }
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Fatal, Panic --
 *
 *      Send a message to the server log with severity level Fatal,
 *      and then exit the nsd process cleanly or abort.
 *
 * Results:
 *      None; does not return.
 *
 * Side effects:
 *      The process will exit or enter the debugger.
 *
 *----------------------------------------------------------------------
 */

void
Ns_Fatal(CONST char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    Ns_VALog(Fatal, fmt, &ap);
    va_end(ap);

    _exit(1);
}

static void
Panic(CONST char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    Ns_VALog(Fatal, fmt, &ap);
    va_end(ap);

    abort();
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
    return strncpy(timeBuf, LogTime(GetCache(), &now, gmt), 41);
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
    if (GetSeverityFromObj(interp, objv[1], &severity) != TCL_OK) {
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
    int             count, opt, enabled, bool, i;
    Ns_DString      ds;
    Tcl_Obj        *objPtr;
    Ns_LogSeverity  severity;
    LogCache       *cachePtr = GetCache();
    LogFilter       filter, *filterPtr = &filter;
    void           *addr;
    Ns_TclCallback *cbPtr;

    static CONST char *opts[] = {
        "hold", "count", "get", "peek", "flush", "release",
        "truncate", "severity", "severities",
        "register", "unregister", NULL
    };
    enum {
        CHoldIdx, CCountIdx, CGetIdx, CPeekIdx, CFlushIdx, CReleaseIdx,
        CTruncIdx, CSeverityIdx, CSeveritiesIdx,
        CRegisterIdx, CUnregisterIdx
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
        cbPtr = Ns_TclNewCallback(interp, Ns_TclCallbackProc,
                                  objv[2], objc - 3, objv + 3);
        Ns_AddLogFilter(LogToTcl, cbPtr, Ns_TclFreeCallback);
        Ns_TclSetAddrObj(Tcl_GetObjResult(interp), filterType, cbPtr);
        break;

    case CUnregisterIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle");
            return TCL_ERROR;
        }
        if (Ns_TclGetAddrFromObj(interp, objv[2], filterType, &addr) != TCL_OK) {
            return TCL_ERROR;
        }
        cbPtr = addr;
        Ns_RemoveLogFilter(LogToTcl, cbPtr);
        break;

    case CHoldIdx:
        cachePtr->hold = 1;
        break;

    case CPeekIdx:
    case CGetIdx:
        memset(filterPtr, 0, sizeof *filterPtr);
        filterPtr->proc = LogToDString;
        filterPtr->arg  = &ds;
        Ns_DStringInit(&ds);
        LogFlush(cachePtr, filterPtr, -1, (opt == CGetIdx), 0);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_DStringValue(&ds), -1));
        Ns_DStringFree(&ds);
        break;

    case CReleaseIdx:
        cachePtr->hold = 0;
        /* FALLTHROUGH */

    case CFlushIdx:
        LogFlush(cachePtr, filters, -1, 1, 1);
        break;

    case CCountIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(cachePtr->count));
        break;

    case CTruncIdx:
        count = 0;
        if (objc > 2 && Tcl_GetIntFromObj(interp, objv[2], &count) != TCL_OK) {
            return TCL_ERROR;
        }
        memset(filterPtr, 0, sizeof *filterPtr);
        LogFlush(cachePtr, filterPtr, count, 1, 0);
        break;

    case CSeverityIdx:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "severity-level ?bool?");
            return TCL_ERROR;
        }
        if (GetSeverityFromObj(interp, objv[2], &severity) != TCL_OK) {
            if (objc == 3) {
                return TCL_ERROR;
            }
            if (severityIdx >= severityCount) {
                Tcl_SetResult(interp, "max log severities exceeded", TCL_STATIC);
                return TCL_ERROR;
            }
            Tcl_ResetResult(interp);
            severity = Ns_CreateLogSeverity(Tcl_GetString(objv[2]));
        }
        enabled = Ns_LogSeverityEnabled(severity);
        if (objc == 4 && severity != Fatal) {
            if (Tcl_GetBooleanFromObj(interp, objv[3], &bool) != TCL_OK) {
                return TCL_ERROR;
            }
            severityConfig[severity].enabled = bool;
        }
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(enabled));
        break;

    case CSeveritiesIdx:
        objPtr = Tcl_GetObjResult(interp);
        for (i = 0; i < severityIdx; i++) {
            if (Tcl_ListObjAppendElement(interp, objPtr,
                    Tcl_NewStringObj(severityConfig[i].string, -1)) != TCL_OK) {
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
    int fd, flags, status = NS_OK;

    flags = O_WRONLY | O_APPEND | O_CREAT;

#ifdef O_LARGEFILE
    flags |= O_LARGEFILE;
#endif

    fd = open(file, flags, 0644);
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
LogFlush(LogCache *cachePtr, LogFilter *listPtr, int count, int trunc, int locked)
{
    int       status, nentry = 0;
    char     *log;
    LogEntry *ePtr;
    LogFilter  *cPtr;

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
                     * skip invoking other registered filters. In such
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
            cachePtr->firstEntry = NULL;
            Ns_DStringSetLength(&cachePtr->buffer, 0);
        }
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

    /*
     * Add the log stamp
     */

    Ns_DStringAppend(dsPtr, LogTime(GetCache(), stamp, 0));
    if (flags & LOG_USEC) {
        Ns_DStringSetLength(dsPtr, Ns_DStringLength(dsPtr) - 1);
        Ns_DStringPrintf(dsPtr, ".%ld]", stamp->usec);
    }
    Ns_DStringPrintf(dsPtr, "[%d.%" PRIxPTR "][%s] %s: ",
                     Ns_InfoPid(), Ns_ThreadId(), Ns_ThreadGetName(),
                     Ns_LogSeverityName(severity));
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
    int        ret, fd = (intptr_t) arg;
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
 *      caller should skip invoking other filters.
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
    char            c;
    void           *logfile = (void *)STDERR_FILENO;
    Tcl_Obj        *stamp;
    Ns_DString      ds;
    Tcl_Interp     *interp;
    Ns_TclCallback *cbPtr = (Ns_TclCallback *)arg;

    if (severity == Fatal) {
        return NS_OK;
    }

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

    Ns_DStringVarAppend(&ds, cbPtr->script, " ", Ns_LogSeverityName(severity), NULL);
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

    LogFlush(cachePtr, filters, -1, 1, 1);
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
 * GetSeverityFromObj --
 *
 *      Get the severity level from the Tcl object, possibly setting
 *      it's internal rep.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Error message left in Tcl interp on error.
 *
 *----------------------------------------------------------------------
 */

static int
GetSeverityFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Ns_LogSeverity *severityPtr)
{
    Tcl_HashEntry *hPtr;
    int            i;

    if (Ns_TclGetOpaqueFromObj(objPtr, severityType,
                               (void **) severityPtr) != TCL_OK) {
        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(&severityTable, Tcl_GetString(objPtr));
        Ns_MutexUnlock(&lock);

        if (hPtr != NULL) {
            *severityPtr = (int)(intptr_t) Tcl_GetHashValue(hPtr);
        } else {
            /*
             * Check for a legacy integer severity.
             */
            if (Tcl_GetIntFromObj(NULL, objPtr, &i) == TCL_OK
                    && i < severityCount) {
                *severityPtr = i;
            } else {
                Tcl_AppendResult(interp, "unknown severity: \"",
                                 Tcl_GetString(objPtr),
                                 "\": should be one of: ", NULL);
                for (i = 0; i < severityIdx; i++) {
                    Tcl_AppendResult(interp, severityConfig[i].string, " ", NULL);
                }
                return TCL_ERROR;
            }
        }
        /*
         * Stash the severity for future speedy lookup.
         */
        Ns_TclSetOpaqueObj(objPtr, severityType, (void *)(intptr_t) *severityPtr);
    }

    return TCL_OK;
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
