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

#define COLOR_BUFFER_SIZE 255u
#define TIME_BUFFER_SIZE 100u

/*
 * The following define available flags bits.
 */

#define LOG_ROLL      0x01u
#define LOG_EXPAND    0x02u
#define LOG_SEC       0x04u
#define LOG_USEC      0x08u
#define LOG_USEC_DIFF 0x10u
#define LOG_THREAD    0x20u
#define LOG_COLORIZE  0x40u

/*
 * The following struct represents a log entry header as stored in the
 * per-thread cache. It is followed by a variable-length log string as
 * passed by the caller (format expanded).
 */

typedef struct LogEntry {
    Ns_LogSeverity   severity;  /* Entry's severity */
    Ns_Time          stamp;     /* Timestamp of the entry */
    size_t           offset;    /* Offset into the text buffer */
    size_t           length;    /* Length of the log message */
    struct LogEntry *nextPtr;   /* Next in the list of entries */
} LogEntry;

/*
 * The following struct represents one registered log callback.
 * Log filters are used to produce log lines into the log sinks
 * and are invoked one by one for every log entry in the cache.
 * Each filter (usually) maintain its own log sink.
 */

typedef struct LogFilter {
    Ns_LogFilter      *proc;        /* User-given function for generating logs */
    Ns_FreeProc       *freeArgProc; /* User-given function to free passed arg */
    void              *arg;         /* Argument passed to proc and free */
    int                refcnt;      /* Number of current consumers */
    struct LogFilter  *nextPtr;     /* Maintains double linked list */
    struct LogFilter  *prevPtr;
} LogFilter;

/*
 * The following struct maintains per-thread cached log entries.
 * The cache is a simple dynamic string where variable-length
 * LogEntry'ies (see below) are appended, one after another.
 */

typedef struct LogCache {
    bool        hold;         /* Flag: keep log entries in cache */
    bool        finalizing;   /* Flag: log is finalizing, no more ops allowed */
    int         count;        /* Number of entries held in the cache */
    time_t      gtime;        /* For GMT time calculation */
    time_t      ltime;        /* For local time calculations */
    char        gbuf[TIME_BUFFER_SIZE];    /* Buffer for GMT time string rep */
    char        lbuf[TIME_BUFFER_SIZE];    /* Buffer for local time string rep */
    size_t      gbufSize;
    size_t      lbufSize;
    LogEntry   *firstEntry;   /* First in the list of log entries */
    LogEntry   *currentEntry; /* Current in the list of log entries */
    Ns_DString  buffer;       /* The log entries cache text-cache */
} LogCache;

static LogEntry *LogEntryGet(LogCache *cachePtr) NS_GNUC_NONNULL(1);
static void LogEntryFree(LogCache *cachePtr, LogEntry *logEntryPtr)  NS_GNUC_NONNULL(1)  NS_GNUC_NONNULL(2);

#if !defined(NS_THREAD_LOCAL)
static void LogEntriesFree(void *arg);
#endif

/*
 * Local functions defined in this file
 */

static Ns_TlsCleanup FreeCache;
static Tcl_PanicProc Panic;

static Ns_LogFilter LogToFile;
static Ns_LogFilter LogToTcl;
static Ns_LogFilter LogToDString;

static Tcl_ObjCmdProc NsLogCtlSeverityObjCmd;

static LogCache* GetCache(void)
    NS_GNUC_RETURNS_NONNULL;

static int GetSeverityFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
                              void **addrPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void  LogFlush(LogCache *cachePtr, LogFilter *listPtr, int count,
                      bool trunc, bool locked)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_LogCallbackProc LogOpen;
static Ns_LogCallbackProc LogClose;

static char* LogTime(LogCache *cachePtr, const Ns_Time *timePtr, bool gmt)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Tcl_Obj *LogStats(void);

static char *LogSeverityColor(char *buffer, Ns_LogSeverity severity)
    NS_GNUC_NONNULL(1);

static int ObjvTableLookup(const char *path, const char *param, Ns_ObjvTable *tablePtr, int *idxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);



/*
 * Static variables defined in this file
 */

static Ns_Tls       tls;
#if !defined(NS_THREAD_LOCAL)
static Ns_Tls       tlsEntry;
#endif
static Ns_Mutex     lock;
static Ns_Cond      cond;

static bool         logOpenCalled = NS_FALSE;
static const char  *logfileName = NULL;
static const char  *rollfmt = NULL;
static unsigned int flags = 0u;
static int          maxbackup;

static LogFilter   *filters;
static const char  *const filterType = "ns:logfilter";
static const char  *const severityType = "ns:logseverity";

static const unsigned char LOG_COLOREND[]   = { 0x1bu, UCHAR('['), UCHAR('0'), UCHAR('m'), 0u };
static const unsigned char LOG_COLORSTART[] = { 0x1bu, UCHAR('['), 0u };

typedef enum {
    COLOR_BLACK   = 30u,
    COLOR_RED     = 31u,
    COLOR_GREEN   = 32u,
    COLOR_YELLOW  = 33u,
    COLOR_BLUE    = 34u,
    COLOR_MAGENTA = 35u,
    COLOR_CYAN    = 36u,
    COLOR_GRAY    = 37u,
    COLOR_DEFAULT = 39u
} LogColor;

typedef enum {
    COLOR_NORMAL = 0u,
    COLOR_BRIGHT = 1u
} LogColorIntensity;

static LogColor prefixColor = COLOR_GREEN;
static LogColorIntensity prefixIntensity = COLOR_NORMAL;

static Ns_ObjvTable colors[] = {
    {"black",    COLOR_BLACK},
    {"red",      COLOR_RED},
    {"green",    COLOR_GREEN},
    {"yellow",   COLOR_YELLOW},
    {"blue",     COLOR_BLUE},
    {"magenta",  COLOR_MAGENTA},
    {"cyan",     COLOR_CYAN},
    {"gray",     COLOR_GRAY},
    {"default",  COLOR_DEFAULT},
    {NULL,       0u}
};

static Ns_ObjvTable intensities[] = {
    {"normal",   COLOR_NORMAL},
    {"bright",   COLOR_BRIGHT},
    {NULL,       0u}
};

/*
 * The following table defines which severity levels
 * are currently active. The order is important: keep
 * it in sync with the Ns_LogSeverity enum.
 *
 * "640 (slots) should be enough for everyone..."
 */

static struct {
    const char       *label;
    bool              enabled;
    long              count;
    LogColor          color;
    LogColorIntensity intensity;
} severityConfig[640] = {
    { "Notice",  NS_TRUE,  0, COLOR_DEFAULT, COLOR_NORMAL },
    { "Warning", NS_TRUE,  0, COLOR_DEFAULT, COLOR_BRIGHT },
    { "Error",   NS_TRUE,  0, COLOR_RED,     COLOR_BRIGHT },
    { "Fatal",   NS_TRUE,  0, COLOR_RED,     COLOR_BRIGHT },
    { "Bug",     NS_TRUE,  0, COLOR_RED,     COLOR_BRIGHT },
    { "Debug",   NS_FALSE, 0, COLOR_BLUE,    COLOR_NORMAL },
    { "Dev",     NS_FALSE, 0, COLOR_GREEN,   COLOR_NORMAL }
};

static const Ns_LogSeverity severityMaxCount = (Ns_LogSeverity)(sizeof(severityConfig) / sizeof(severityConfig[0]));
static Ns_LogSeverity severityIdx = 0;

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
    char           buf[21];
    int            isNew;
    Ns_LogSeverity i;

    Ns_MutexSetName(&lock, "ns:log");
    Ns_TlsAlloc(&tls, FreeCache);
#if !defined(NS_THREAD_LOCAL)
    Ns_TlsAlloc(&tlsEntry, LogEntriesFree);
#endif
    Tcl_InitHashTable(&severityTable, TCL_STRING_KEYS);

    Tcl_SetPanicProc(Panic);
    Ns_AddLogFilter(LogToFile, INT2PTR(STDERR_FILENO), NULL);

    /*
     * Initialize the entire space with backwards-compatible integer keys.
     */
    for (i = PredefinedLogSeveritiesCount; i < severityMaxCount; i++) {
        (void) ns_uint32toa(buf, (uint32_t)i);
        hPtr = Tcl_CreateHashEntry(&severityTable, buf, &isNew);
        Tcl_SetHashValue(hPtr, INT2PTR(i));
        severityConfig[i].label = Tcl_GetHashKey(&severityTable, hPtr);
        severityConfig[i].enabled = NS_FALSE;
    }

    /*
     * Initialize the built-in severities and lowercase aliases.
     */
    for (i = 0; i < PredefinedLogSeveritiesCount; i++) {
        size_t labelLength;

        (void) Ns_CreateLogSeverity(severityConfig[i].label);
        labelLength = strlen(severityConfig[i].label);
        if (labelLength < sizeof(buf)) {
            memcpy(buf, severityConfig[i].label, labelLength + 1u);
        } else {
            memcpy(buf, severityConfig[i].label, sizeof(buf) - 1u);
            buf[sizeof(buf) - 1u] = '\0';
        }
        hPtr = Tcl_CreateHashEntry(&severityTable, Ns_StrToLower(buf), &isNew);
        Tcl_SetHashValue(hPtr, INT2PTR(i));
    }
}



/*
 *----------------------------------------------------------------------
 *
 * ObjvTableLookup --
 *
 *      Lookup a value from an Ns_ObjvTable and return its associated
 *      value in the last parameter, if the lookup was successful.
 *
 * Results:
 *      Tcl return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ObjvTableLookup(const char *path, const char *param, Ns_ObjvTable *tablePtr, int *idxPtr)
{
    size_t       len;
    int          result, pos = 1;
    const char  *valueString;

    NS_NONNULL_ASSERT(path != NULL);
    NS_NONNULL_ASSERT(param != NULL);
    NS_NONNULL_ASSERT(tablePtr != NULL);
    NS_NONNULL_ASSERT(idxPtr != NULL);

    valueString = Ns_ConfigString(path, param, NS_EMPTY_STRING);
    assert(valueString != NULL);

    len = strlen(valueString);
    if (len > 0u) {
        Ns_ObjvSpec  spec;
        Tcl_Obj     *objPtr = Tcl_NewStringObj(valueString, (int)len);

        spec.arg  = tablePtr;
        spec.dest = idxPtr;
        result = Ns_ObjvIndex(&spec, NULL, &pos, &objPtr);

        if (unlikely(result != TCL_OK)) {
            Ns_DString ds, *dsPtr = &ds;

            Ns_DStringInit(dsPtr);
            while (tablePtr->key != NULL) {
                Ns_DStringNAppend(dsPtr, tablePtr->key, -1);
                Ns_DStringNAppend(dsPtr, " ", 1);
                tablePtr++;
            }
            Ns_DStringSetLength(dsPtr, Ns_DStringLength(dsPtr) - 1);
            Ns_Log(Warning, "ignoring invalid value '%s' for parameter '%s'; "
                   "possible values are: %s",
                   valueString, param, Ns_DStringValue(dsPtr));
            Ns_DStringFree(dsPtr);
        }
        Tcl_DecrRefCount(objPtr);

    } else {
        result = TCL_ERROR;
    }

    return result;
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
 *      Depends on configuration file.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigLog(void)
{
    Ns_DString  ds;
    const char *path = NS_GLOBAL_CONFIG_PARAMETERS;
    Ns_Set     *set  = Ns_ConfigCreateSection(path);

    severityConfig[Debug ].enabled = Ns_ConfigBool(path, "logdebug",  NS_FALSE);
    severityConfig[Dev   ].enabled = Ns_ConfigBool(path, "logdev",    NS_FALSE);
    severityConfig[Notice].enabled = Ns_ConfigBool(path, "lognotice", NS_TRUE);

    if (Ns_ConfigBool(path, "logroll", NS_TRUE) == NS_TRUE) {
        flags |= LOG_ROLL;
    }
    if (Ns_ConfigBool(path, "logsec", NS_TRUE) == NS_TRUE) {
        flags |= LOG_SEC;
    }
    if (Ns_ConfigBool(path, "logusec", NS_FALSE) == NS_TRUE) {
        flags |= LOG_USEC;
    }
    if (Ns_ConfigBool(path, "logusecdiff", NS_FALSE) == NS_TRUE) {
        flags |= LOG_USEC_DIFF;
    }
    if (Ns_ConfigBool(path, "logexpanded", NS_FALSE) == NS_TRUE) {
        flags |= LOG_EXPAND;
    }
    if (Ns_ConfigBool(path, "logthread", NS_TRUE) == NS_TRUE) {
        flags |= LOG_THREAD;
    }
    if (Ns_ConfigBool(path, "logcolorize", NS_FALSE) == NS_TRUE) {
        flags |= LOG_COLORIZE;
    }
    if ((flags & LOG_COLORIZE) != 0u) {
        int result, idx;

        result = ObjvTableLookup(path, "logprefixcolor", colors, &idx);
        if (likely(result == TCL_OK)) {
            prefixColor = (LogColor)idx;
        }
        result = ObjvTableLookup(path, "logprefixintensity", intensities, &idx);
        if (likely(result == TCL_OK)) {
            prefixIntensity = (LogColorIntensity)idx;
        }
    } else {
        /*
         * Just refer to these values to mark these as used.
         */
        (void) Ns_ConfigString(path, "logprefixcolor", NS_EMPTY_STRING);
        (void) Ns_ConfigString(path, "logprefixintensity", NS_EMPTY_STRING);
    }

    maxbackup = Ns_ConfigIntRange(path, "logmaxbackup", 10, 0, 999);

    logfileName = ns_strcopy(Ns_ConfigString(path, "serverlog", "nsd.log"));
    if (Ns_PathIsAbsolute(logfileName) == NS_FALSE) {
        int length;

        Ns_DStringInit(&ds);
        if (Ns_HomePathExists("logs", (char *)0L)) {
            (void)Ns_HomePath(&ds, "logs", logfileName, (char *)0L);
        } else {
            (void)Ns_HomePath(&ds, logfileName, (char *)0L);
        }
        length = ds.length;
        ns_free((void*)logfileName);
        logfileName = Ns_DStringExport(&ds);
        Ns_SetUpdateSz(set, "serverlog", 9, logfileName, length);
    }

    rollfmt = ns_strcopy(Ns_ConfigString(path, "logrollfmt", NS_EMPTY_STRING));

}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoErrorLog --
 *
 *      Returns the filename of the log file.
 *
 * Results:
 *      Log filename or NULL if none.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_InfoErrorLog(void)
{
    return logfileName;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CreateLogSeverity --
 *
 *      Create and return a new log severity with the given name,
 *      which will initially be disabled (except for the built-ins).
 *
 * Results:
 *      The severity.
 *
 * Side effects:
 *      Server will exit if max severities exceeded.
 *
 *----------------------------------------------------------------------
 */

Ns_LogSeverity
Ns_CreateLogSeverity(const char *name)
{
    Ns_LogSeverity  severity;
    Tcl_HashEntry  *hPtr;
    int             isNew = 0;

    NS_NONNULL_ASSERT(name != NULL);

    if (severityIdx >= severityMaxCount) {
        Ns_Fatal("max log severities exceeded");
    }
    Ns_MutexLock(&lock);
    hPtr = Tcl_CreateHashEntry(&severityTable, name, &isNew);
    if (isNew != 0) {
        /*
         * Create new severity.
         */
        severity = severityIdx++;
        Tcl_SetHashValue(hPtr, INT2PTR(severity));
        severityConfig[severity].label = Tcl_GetHashKey(&severityTable, hPtr);
        if (severity > Dev) {
            /*
             * For the lower severities, we have already defaults; initialize
             * just the higher ones.
             */
            severityConfig[severity].enabled = NS_FALSE;
            /*
             * Initialize new severity with default colors.
             */
            severityConfig[severity].color     = COLOR_DEFAULT;
            severityConfig[severity].intensity = COLOR_NORMAL;
        }
    } else {
        severity = PTR2INT(Tcl_GetHashValue(hPtr));
    }
    Ns_MutexUnlock(&lock);

    return severity;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogSeverityName --
 *
 *      Given a log severity, return a pointer to its name.
 *
 * Results:
 *      The severity name.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_LogSeverityName(Ns_LogSeverity severity)
{
    const char *result;

    if (severity < severityMaxCount) {
        result = severityConfig[severity].label;
    } else {
        result = "Unknown";
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * LogSeverityColor --
 *
 *      Given a log severity, set color in terminal
 *
 * Results:
 *      A print string for setting the color in the terminal.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
LogSeverityColor(char *buffer, Ns_LogSeverity severity)
{
    if (severity < severityMaxCount) {
        snprintf(buffer, COLOR_BUFFER_SIZE, "%s%d;%dm", LOG_COLORSTART,
                severityConfig[severity].intensity,
                severityConfig[severity].color);
    } else {
        snprintf(buffer, COLOR_BUFFER_SIZE, "%s0m", LOG_COLORSTART);
    }
    return buffer;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogSeverityEnabled --
 *
 *      Return true if the given severity level is enabled.
 *
 * Results:
 *      Boolean
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_LogSeverityEnabled(Ns_LogSeverity severity)
{
    bool result;

    if (likely(severity < severityMaxCount)) {
        result = severityConfig[severity].enabled;
    } else {
        result = NS_TRUE;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogSeveritySetEnabled --
 *
 *      Allow from the C-API switching of enabled state on and off
 *
 * Results:
 *      Previous enabled state.
 *
 * Side effects:
 *      Updating severityConfig
 *
 *----------------------------------------------------------------------
 */
bool
Ns_LogSeveritySetEnabled(Ns_LogSeverity severity, bool enabled)
{
    bool result;

    if (likely(severity < severityMaxCount)) {
        result = severityConfig[severity].enabled;
        severityConfig[severity].enabled = enabled;
    } else {
        result = NS_FALSE;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * LogStats --
 *
 *      Return a Tcl list containing the labels and counts for all
 *      severities.  The function should be probably be guarded by a
 *      lock, but we have just single word operations and potentially
 *      incorrect counts are not fatal.
 *
 * Results:
 *      Tcl list
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
LogStats(void)
{
    Ns_LogSeverity s;
    Tcl_Obj *listObj;

    listObj = Tcl_NewListObj(0, NULL);
    for (s = 0; s < severityIdx; s++) {
        (void)Tcl_ListObjAppendElement(NULL, listObj, Tcl_NewStringObj(severityConfig[s].label, -1));
        (void)Tcl_ListObjAppendElement(NULL, listObj, Tcl_NewLongObj(severityConfig[s].count));
    }
    return listObj;
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
Ns_Log(Ns_LogSeverity severity, const char *fmt, ...)
{
    va_list ap;

    NS_NONNULL_ASSERT(fmt != NULL);

    va_start(ap, fmt);
    Ns_VALog(severity, fmt, ap);
    va_end(ap);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VALog --
 *
 *      Add an entry to the log cache if the severity is not suppressed.
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
Ns_VALog(Ns_LogSeverity severity, const char *fmt, va_list apSrc)
{
    LogCache *cachePtr;

    NS_NONNULL_ASSERT(fmt != NULL);

    /*
     * Skip if logging for selected severity is disabled
     * or if severity level out of range(s).
     */

    if (Ns_LogSeverityEnabled(severity)) {
        size_t    length, offset;
        LogEntry *entryPtr;
        /*
         * Track usage to provide statistics.  The next line can lead
         * potentially to data races (dirty reads).
         */
        severityConfig[severity].count ++;

        /*
         * Append new or reuse log entry record.
         */
        cachePtr = GetCache();
        if (cachePtr->finalizing) {
            Tcl_DString ds;

            fprintf(stderr, "Log cache is already finalized, ignore logging attempt, message:\n");
            Tcl_DStringInit(&ds);
            Ns_DStringVPrintf(&ds, fmt, apSrc);
            fprintf(stderr, "%s", ds.string);
            Tcl_DStringFree(&ds);
            return;
        }
        if (cachePtr->currentEntry != NULL) {
            entryPtr = cachePtr->currentEntry->nextPtr;
        } else {
            entryPtr = cachePtr->firstEntry;
        }
        if (entryPtr == NULL) {
            entryPtr = LogEntryGet(cachePtr);
            entryPtr->nextPtr = NULL;
            if (cachePtr->currentEntry != NULL) {
                cachePtr->currentEntry->nextPtr = entryPtr;
            } else {
                cachePtr->firstEntry = entryPtr;
            }
        }

        cachePtr->currentEntry = entryPtr;
        cachePtr->count++;

        offset = (size_t)Ns_DStringLength(&cachePtr->buffer);
        Ns_DStringVPrintf(&cachePtr->buffer, fmt, apSrc);
        length = (size_t)Ns_DStringLength(&cachePtr->buffer) - offset;

        entryPtr->severity = severity;
        entryPtr->offset   = offset;
        entryPtr->length   = length;
        Ns_GetTime(&entryPtr->stamp);

        /*
         * Flush it out if not held or severity is "Fatal"
         */
        if (!cachePtr->hold || severity == Fatal) {
            LogFlush(cachePtr, filters, -1, NS_TRUE, NS_FALSE);
        }
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
Ns_AddLogFilter(Ns_LogFilter *procPtr, void *arg, Ns_FreeProc *freeProc)
{
    LogFilter *filterPtr = ns_calloc(1u, sizeof *filterPtr);

    NS_NONNULL_ASSERT(procPtr != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    Ns_MutexLock(&lock);

    if (filters != NULL) {
        filters->nextPtr = filterPtr;
        filterPtr->prevPtr = filters;
    } else {
        filterPtr->prevPtr = NULL;
    }

    filterPtr->nextPtr = NULL;
    filters = filterPtr;

    filterPtr->proc = procPtr;
    filterPtr->arg  = arg;
    filterPtr->freeArgProc = freeProc;

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
Ns_RemoveLogFilter(Ns_LogFilter *procPtr, void *const arg)
{
    LogFilter *filterPtr;

    NS_NONNULL_ASSERT(procPtr != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

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
        if (filterPtr->freeArgProc != NULL && filterPtr->arg != NULL) {
            (*filterPtr->freeArgProc)(filterPtr->arg);
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
Ns_Fatal(const char *fmt, ...)
{
    va_list ap;

    NS_NONNULL_ASSERT(fmt != NULL);

    va_start(ap, fmt);
    Ns_VALog(Fatal, fmt, ap);
    va_end(ap);

    if (nsconf.state.pipefd[1] != 0) {
        /*
         * Tell the parent process, that something went wrong.
         */
        if (ns_write(nsconf.state.pipefd[1], "F", 1) < 1) {
            /*
             * In case, the write did not work, do nothing. Ignoring
             * the result can lead to warnings due to
             * warn_unused_result.
             */
            ;
        }
    }

    _exit(1);
}

static void
Panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    Ns_VALog(Fatal, fmt, ap);
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
    NS_NONNULL_ASSERT(timeBuf != NULL);

    return Ns_LogTime2(timeBuf, NS_TRUE);
}

char *
Ns_LogTime2(char *timeBuf, bool gmt)
{
    Ns_Time now;
    LogCache   *cachePtr = GetCache();
    const char *timeString;
    size_t      timeStringLength;

    NS_NONNULL_ASSERT(timeBuf != NULL);

    if (cachePtr->finalizing) {
        timeBuf[0] = 0;
        return timeBuf;
    }

    /*
     * Add the log stamp
     */
    Ns_GetTime(&now);
    timeString = LogTime(cachePtr, &now, gmt);
    timeStringLength = gmt ? cachePtr->gbufSize : cachePtr->lbufSize;

    assert(timeStringLength < 41);
    assert(timeStringLength == strlen(timeString));

    return memcpy(timeBuf, timeString, timeStringLength + 1u);
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
LogTime(LogCache *cachePtr, const Ns_Time *timePtr, bool gmt)
{
    time_t    *tp;
    char      *bp;
    size_t    *sizePtr;

    NS_NONNULL_ASSERT(cachePtr != NULL);
    NS_NONNULL_ASSERT(timePtr != NULL);

    /*
     * Use either GMT or local time.
     */
    if (gmt) {
        tp = &cachePtr->gtime;
        bp = cachePtr->gbuf;
        sizePtr = &cachePtr->gbufSize;
    } else {
        tp = &cachePtr->ltime;
        bp = cachePtr->lbuf;
        sizePtr = &cachePtr->lbufSize;
    }

    /*
     * LogTime has a granularity of seconds. For frequent updates the
     * print string is therefore cached. Check if the value for
     * seconds in the cache is the same as the required value. If not,
     * recompute the string and store it in the cache.
     */
    if (*tp != timePtr->sec) {
        size_t           n;
        time_t           secs;
        const struct tm *ptm;

        *tp = timePtr->sec;

        secs = timePtr->sec;
        ptm = ns_localtime(&secs);

        n = strftime(bp, 32u, "[%d/%b/%Y:%H:%M:%S", ptm);
        if (!gmt) {
            bp[n++] = ']';
            bp[n] = '\0';
        } else {
            long gmtoff;
            char sign;
#ifdef HAVE_TM_GMTOFF
            gmtoff = ptm->tm_gmtoff / 60;
#else
            gmtoff = -1 * (int)timezone / 60;
            if (daylight != 0 && ptm->tm_isdst != 0) {
                gmtoff += 60;
            }
#endif
            if (gmtoff < 0) {
                sign = '-';
                gmtoff *= -1;
            } else {
                sign = '+';
            }
            n += (size_t)snprintf(bp + n, TIME_BUFFER_SIZE - n, " %c%02ld%02ld]",
                                  sign, gmtoff/60, gmtoff%60);
        }
        *sizePtr = n;
    }

    return bp;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLogObjCmd --
 *
 *      Implements "ns_log".
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
NsTclLogObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    void *addrPtr;
    int   result = TCL_OK;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "severity string ?string ...?");
        result = TCL_ERROR;
    } else if (unlikely(GetSeverityFromObj(interp, objv[1], &addrPtr) != TCL_OK)) {
        result = TCL_ERROR;
    } else {
        Ns_LogSeverity severity = PTR2INT(addrPtr);
        Ns_DString     ds;

        if (likely(objc == 3)) {
            Ns_Log(severity, "%s", Tcl_GetString(objv[2]));
        } else {
            int i;

            Ns_DStringInit(&ds);
            for (i = 2; i < objc; ++i) {
                Ns_DStringVarAppend(&ds, Tcl_GetString(objv[i]),
                                    i < (objc-1) ? " " : (char *)0, (char *)0L);
            }
            Ns_Log(severity, "%s", Ns_DStringValue(&ds));
            Ns_DStringFree(&ds);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsLogCtlSeverityObjCmd --
 *
 *      Implements "ns_logctl severtiy" command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

static int
NsLogCtlSeverityObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    Ns_LogSeverity    severity = 0; /* default value for the error cases */
    void             *addrPtr = NULL;
    int               result = TCL_OK, color = -1, intensity = -1, givenEnabled = -1;
    Ns_ObjvSpec       lopts[] = {
        {"-color",     Ns_ObjvIndex,  &color,     colors},
        {"-intensity", Ns_ObjvIndex,  &intensity, intensities},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec       args[] = {
        {"?enabled",   Ns_ObjvBool,   &givenEnabled, INT2PTR(NS_FALSE)},
        {NULL, NULL, NULL, NULL}
    };

    if (likely(objc < 3)) {
        Tcl_WrongNumArgs(interp, 2, objv, "severity-level ?-color color? ?-intensity intensity? ?bool?");
        result = TCL_ERROR;

    } else if (GetSeverityFromObj(interp, objv[2], &addrPtr) == TCL_OK) {
        /*
         * Severity lookup was ok
         */
        severity = PTR2INT(addrPtr);

    } else if (objc > 3) {
        /*
         * Severity lookup failed, but more arguments are specified,
         * we create a new severity.
         */
        severity = Ns_CreateLogSeverity(Tcl_GetString(objv[2]));
        assert(severity < severityMaxCount);

    } else {
        /*
         * Probably a typo when querying the severity.
         */
        result = TCL_ERROR;
    }

    if (likely(result == TCL_OK)
        && Ns_ParseObjv(lopts, args, interp, 2, objc-1, objv+1) != NS_OK
        ) {
        result = TCL_ERROR;
    }

    if (likely(result == TCL_OK)) {
        bool enabled;

        assert(severity < severityMaxCount);

        /*
         * Don't allow one to deactivate Fatal.
         */
        if (givenEnabled != -1 && severity != Fatal) {
            enabled = severityConfig[severity].enabled;
            severityConfig[severity].enabled = (givenEnabled == 1);
        } else {
            enabled = Ns_LogSeverityEnabled(severity);
        }

        /*
         * Set color attributes, when available.
         */
        if (color != -1) {
            severityConfig[severity].color = (LogColor)color;
        }
        if (intensity != -1) {
            severityConfig[severity].intensity = (LogColorIntensity)intensity;
        }

        /*
         * Return the new enabled state.
         */
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(enabled));
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLogCtlObjCmd --
 *
 *      Implements "ns_logctl". This command provides control over the
 *      the activated severities or buffering of log messages.
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
NsTclLogCtlObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int             result = TCL_OK, count, opt, i;
    Ns_DString      ds;
    Tcl_Obj        *objPtr;
    LogCache       *cachePtr = GetCache();
    LogFilter       filter, *filterPtr = &filter;
    void           *addr;
    Ns_TclCallback *cbPtr;

    static const char *const opts[] = {
        "count",
        "flush",
        "get",
        "hold",
        "peek",
        "register",
        "release",
        "severities",
        "severity",
        "stats",
        "truncate",
        "unregister",
        NULL
    };
    enum {
        CCountIdx,
        CFlushIdx,
        CGetIdx,
        CHoldIdx,
        CPeekIdx,
        CRegisterIdx,
        CReleaseIdx,
        CSeveritiesIdx,
        CSeverityIdx,
        CStatsIdx,
        CTruncIdx,
        CUnregisterIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg?");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else if (cachePtr->finalizing) {
        /*
         * Silent Fail.
         */
        return TCL_OK;

    } else {

        switch (opt) {

        case CRegisterIdx:
            if (objc < 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "script ?arg?");
                result = TCL_ERROR;
            } else {
                cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)Ns_TclCallbackProc,
                                          objv[2], objc - 3, objv + 3);
                Ns_AddLogFilter(LogToTcl, cbPtr, Ns_TclFreeCallback);
                Ns_TclSetAddrObj(Tcl_GetObjResult(interp), filterType, cbPtr);
            }
            break;

        case CUnregisterIdx:
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "handle");
                result = TCL_ERROR;
            } else if (Ns_TclGetAddrFromObj(interp, objv[2], filterType, &addr) != TCL_OK) {
                result = TCL_ERROR;
            } else {
                cbPtr = addr;
                Ns_RemoveLogFilter(LogToTcl, cbPtr);
            }
            break;

        case CHoldIdx:
            cachePtr->hold = NS_TRUE;
            break;

        case CPeekIdx:
        case CGetIdx:
            memset(filterPtr, 0, sizeof(*filterPtr));
            filterPtr->proc = LogToDString;
            filterPtr->arg  = &ds;
            Ns_DStringInit(&ds);
            LogFlush(cachePtr, filterPtr, -1, (opt == CGetIdx), NS_FALSE);
            Tcl_DStringResult(interp, &ds);
            break;

        case CReleaseIdx:
            cachePtr->hold = NS_FALSE;
            NS_FALL_THROUGH; /* fall through */
        case CFlushIdx:
            LogFlush(cachePtr, filters, -1, NS_TRUE, NS_TRUE);
            break;

        case CCountIdx:
            Tcl_SetObjResult(interp, Tcl_NewIntObj(cachePtr->count));
            break;

        case CTruncIdx:
            count = 0;
            if (objc > 2) {
                Ns_ObjvValueRange countRange = {0, INT_MAX};
                int               oc = 1;
                Ns_ObjvSpec       spec = {"?count", Ns_ObjvInt, &count, &countRange};

                if (Ns_ObjvInt(&spec, interp, &oc, &objv[2]) != TCL_OK) {
                    result = TCL_ERROR;
                }
            }
            if (result == TCL_OK) {
                memset(filterPtr, 0, sizeof(*filterPtr));
                LogFlush(cachePtr, filterPtr, count, NS_TRUE, NS_TRUE);
            }
            break;

        case CSeverityIdx:
            result = NsLogCtlSeverityObjCmd(clientData, interp, objc, objv);
            break;

        case CSeveritiesIdx:
            /*
             * Return all registered severities in a list
             */
            objPtr = Tcl_GetObjResult(interp);
            for (i = 0; i < severityIdx; i++) {
                if (Tcl_ListObjAppendElement(interp, objPtr,
                                             Tcl_NewStringObj(severityConfig[i].label, -1))
                    != TCL_OK) {
                    result = TCL_ERROR;
                    break;
                }
            }
            break;

        case CStatsIdx:
            Tcl_SetObjResult(interp, LogStats());
            break;

        default:
            /*
             * Unexpected value, raise an exception in development mode.
             */
            assert(opt && 0);
            break;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLogRollObjCmd --
 *
 *      Implements "ns_logroll".
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
NsTclLogRollObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                   int UNUSED(objc), Tcl_Obj *const* UNUSED(objv))
{
    if (Ns_LogRoll() != NS_OK) {
        Ns_TclPrintfResult(interp, "could not roll server log");
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LogRoll --
 *
 *      Function and signal handler for SIGHUP which will roll the
 *      system log (e.g. "error.log" or stderr). When NaviServer is
 *      logging to stderr (when e.g. started with -f) no rolling will
 *      be performed. The function returns potentially errors from
 *      opening the file named by logfileName as result.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will rename the log file and reopen it.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_LogRoll(void)
{
    Ns_ReturnCode status;

    if (logfileName != NULL && logOpenCalled) {
        status = Ns_RollFileCondFmt(LogOpen, LogClose, NULL,
                                    logfileName, rollfmt, maxbackup);
    } else {
        status = NS_OK;
    }

    return status;
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
 *      Configures this module to use the newly opened log file.  If
 *      LogRoll is turned on in the configuration file, then it registers a
 *      signal callback.
 *
 *----------------------------------------------------------------------
 */

void
NsLogOpen(void)
{
    /*
     * Open the log and schedule the signal roll.
     */

    if (LogOpen(NULL) != NS_OK) {
        Ns_Fatal("log: failed to open server log '%s': '%s'",
                 logfileName, strerror(errno));
    }
    if ((flags & LOG_ROLL) != 0u) {
        Ns_Callback *proc = (Ns_Callback *)(ns_funcptr_t)Ns_LogRoll;
        (void) Ns_RegisterAtSignal(proc, NULL);
    }
    logOpenCalled = NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * LogOpen --
 *
 *      Open the log filename specified in the global variable
 *      'logfileName'. If it is successfully opened, make that file the
 *      sink for stdout and stderr too.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogOpen(void *UNUSED(arg))
{
    int           fd;
    Ns_ReturnCode status = NS_OK;
    unsigned int  oflags;

    oflags = O_WRONLY | O_APPEND | O_CREAT;

#ifdef O_LARGEFILE
    oflags |= O_LARGEFILE;
#endif

    fd = ns_open(logfileName, (int)oflags, 0644);
    if (fd == NS_INVALID_FD) {
        Ns_Log(Error, "log: failed to re-open log file '%s': '%s'",
               logfileName, strerror(errno));
        status = NS_ERROR;
    } else {

        /*
         * Route stderr to the file
         */
        if (fd != STDERR_FILENO && ns_dup2(fd, STDERR_FILENO) == -1) {
            status = NS_ERROR;
        }

        /*
         * Route stdout to the file
         */
        if (ns_dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            Ns_Log(Error, "log: failed to route stdout to file: '%s'",
                   strerror(errno));
            status = NS_ERROR;
        }

        /*
         * Clean up dangling 'open' reference to the fd
         */
        if (fd != STDERR_FILENO && fd != STDOUT_FILENO) {
            (void) ns_close(fd);
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
LogFlush(LogCache *cachePtr, LogFilter *listPtr, int count, bool trunc, bool locked)
{
    int            nentry = 0;
    LogFilter     *cPtr;
    LogEntry      *ePtr;

    NS_NONNULL_ASSERT(cachePtr != NULL);
    NS_NONNULL_ASSERT(listPtr != NULL);

    /*fprintf(stderr, "#### %s cachePtr %p locked %d count %d cachePtr->count %d hold %d\n",
      Ns_ThreadGetName(), (void*)cachePtr, locked, count, cachePtr->count, cachePtr->hold);*/

    if (locked) {
        Ns_MutexLock(&lock);
    }

    ePtr = cachePtr->firstEntry;
    while (ePtr != NULL && cachePtr->currentEntry != NULL) {
        const char *logString = Ns_DStringValue(&cachePtr->buffer) + ePtr->offset;

        /*
         * Since listPtr is never NULL, a repeat-unil loop is
         * sufficient to guarantee that the initial cPtr is not NULL
         * either.
         */
        cPtr = listPtr;
        do {
            if (cPtr->proc != NULL) {
                Ns_ReturnCode  status;

                if (locked) {
                    cPtr->refcnt++;
                    Ns_MutexUnlock(&lock);
                }
                status = (*cPtr->proc)(cPtr->arg, ePtr->severity,
                                       &ePtr->stamp, logString, ePtr->length);
                if (locked) {
                    Ns_MutexLock(&lock);
                    cPtr->refcnt--;
                    Ns_CondBroadcast(&cond);
                }
                if (status == NS_ERROR) {
                    /*
                     * Callback signaled an error. Per definition we
                     * will skip invoking other registered filters. In
                     * such case we must assure that the current log
                     * entry eventually gets written into some log
                     * sink, so we use the default logfile sink.
                     */
                    (void) LogToFile(INT2PTR(STDERR_FILENO), ePtr->severity,
                                     &ePtr->stamp, logString, ePtr->length);
                    break;
                }
            }
            cPtr = cPtr->prevPtr;
        } while (cPtr != NULL);

        nentry++;
        if ((count > 0 && nentry >= count) || ePtr == cachePtr->currentEntry) {
            break;
        }
        ePtr = ePtr->nextPtr;
    }

    if (trunc) {
        if (count > 0) {
            size_t length = (ePtr != NULL) ? (ePtr->offset + ePtr->length) : 0u;
            cachePtr->count = (length != 0u) ? nentry : 0;
            cachePtr->currentEntry = ePtr;
            Ns_DStringSetLength(&cachePtr->buffer, (int)length);
        } else {
            LogEntry *entryPtr, *tmpPtr;

            /*
             * The cache is reset (count <= 0). If there are log
             * entries in the cache, first zero the pointers
             * to minimize free memory reads probability when
             * no locks are applied, then flush the memory.
             */
            entryPtr = cachePtr->firstEntry;

            cachePtr->count = 0;
            cachePtr->currentEntry = NULL;
            cachePtr->firstEntry = NULL;
            Ns_DStringSetLength(&cachePtr->buffer, 0);

            for (; entryPtr != NULL; entryPtr = tmpPtr) {
                tmpPtr = entryPtr->nextPtr;
                LogEntryFree(cachePtr, entryPtr);
            }
        }
    }

    if (locked) {
        Ns_MutexUnlock(&lock);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * LogClose --
 *
 *      Close the logfile. Since unixes, this happens via stderr
 *      magic, we need here just the handling for windows.
 *
 * Results:
 *      NS_OK
 *
 * Side effects:
 *      Closing the STDERR+STDOUT on windows.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
LogClose(void *UNUSED(arg))
{
    /*
     * Probably, LogFlush() should be done here as well, but it was
     * not used so far at this place.
     */
#ifdef _WIN32
    /* On Windows you MUST close stdout and stderr now, or
       Tcl_FSRenameFile() will fail with "Permission denied". */
    (void)ns_close(STDOUT_FILENO);
    (void)ns_close(STDERR_FILENO);
#endif
    return NS_OK;
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

static Ns_ReturnCode
LogToDString(const void *arg, Ns_LogSeverity severity, const Ns_Time *stamp,
            const char *msg, size_t len)
{
    Ns_DString *dsPtr  = (Ns_DString *)arg;
    LogCache   *cachePtr = GetCache();
    char        buffer[COLOR_BUFFER_SIZE];

    NS_NONNULL_ASSERT(arg != NULL);
    NS_NONNULL_ASSERT(stamp != NULL);
    NS_NONNULL_ASSERT(msg != NULL);

    if (cachePtr->finalizing) {
        /*
         * Silent fail.
         */
        return NS_OK;
    }

    /*
     * In case colorization was configured, add the escape necessary
     * sequences.
     */
    if ((flags & LOG_COLORIZE) != 0u) {
        Ns_DStringPrintf(dsPtr, "%s%d;%dm", LOG_COLORSTART, prefixIntensity, prefixColor);
    }

    if ((flags & LOG_SEC) != 0u) {
        const char *timeString;
        size_t      timeStringLength;

        /*
         * Add the log stamp
         */
        timeString = LogTime(cachePtr, stamp, NS_FALSE);
        timeStringLength = cachePtr->lbufSize;
        Ns_DStringNAppend(dsPtr, timeString, (int)timeStringLength);
    }

    if ((flags & LOG_USEC) != 0u) {
        Ns_DStringSetLength(dsPtr, Ns_DStringLength(dsPtr) - 1);
        Ns_DStringPrintf(dsPtr, ".%06ld]", stamp->usec);
    }

    if ((flags & LOG_USEC_DIFF) != 0u) {
        Ns_Time        now;
        static Ns_Time last = {0, 0};

        Ns_GetTime(&now);
        /*
         * Initialize if needed.
         */
        if (last.sec == 0) {
            last.sec = now.sec;
            last.usec = now.usec;
        }
        /*
         * Skip last char.
         */
        Ns_DStringSetLength(dsPtr, Ns_DStringLength(dsPtr) - 1);
        /*
         * Handle change in seconds.
         */
        if (last.sec < now.sec) {
            last.sec = now.sec;
            Ns_DStringPrintf(dsPtr, "-%.6ld]", now.usec + (1000000 - last.usec));
        } else {
            Ns_DStringPrintf(dsPtr, "-%.6ld]", now.usec-last.usec);
        }
        last.usec = now.usec;
    }
    if ((flags & LOG_THREAD) != 0u) {
        Ns_DStringPrintf(dsPtr, "[%d.%" PRIxPTR "]", (int)Ns_InfoPid(), Ns_ThreadId());
    }
    if ((flags & LOG_COLORIZE) != 0u) {
        Ns_DStringPrintf(dsPtr, "[%s] %s%s%s: ",
                         Ns_ThreadGetName(),
                         (const char *)LOG_COLOREND,
                         LogSeverityColor(buffer, severity),
                         Ns_LogSeverityName(severity));
    } else {
        Ns_DStringPrintf(dsPtr, "[%s] %s: ",
                         Ns_ThreadGetName(),
                         Ns_LogSeverityName(severity));
    }

    if ((flags & LOG_EXPAND) != 0u) {
        Ns_DStringNAppend(dsPtr, "\n    ", 5);
    }

    /*
     * Add the log message
     */

    if (len == 0u) {
        len = strlen(msg);
    }
    if (nsconf.sanitize_logfiles > 0) {
        Ns_DStringAppendPrintable(dsPtr, nsconf.sanitize_logfiles == 2, msg, len);
    } else {
        Ns_DStringNAppend(dsPtr, msg, (int)len);
    }
    if ((flags & LOG_COLORIZE) != 0u) {
        Ns_DStringNAppend(dsPtr, (const char *)LOG_COLOREND, 4);
    }
    Ns_DStringNAppend(dsPtr, "\n", 1);
    if ((flags & LOG_EXPAND) != 0u) {
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
 *      Returns always NS_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogToFile(const void *arg, Ns_LogSeverity severity, const Ns_Time *stamp,
          const char *msg, size_t len)
{
    int        fd = PTR2INT(arg);
    Ns_DString ds;

    NS_NONNULL_ASSERT(arg != NULL);
    NS_NONNULL_ASSERT(stamp != NULL);
    NS_NONNULL_ASSERT(msg != NULL);

    Ns_DStringInit(&ds);

    (void) LogToDString(&ds, severity, stamp, msg, len);
    (void) NsAsyncWrite(fd, Ns_DStringValue(&ds), (size_t)Ns_DStringLength(&ds));

    Ns_DStringFree(&ds);
    return NS_OK;
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
 *      This call deliberately does not use Ns_TclEvalCallback(), as
 *      if the Tcl code throws error, that one will invoke
 *      Ns_TclLogError() and will deadlock in the log code.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
LogToTcl(const void *arg, Ns_LogSeverity severity, const Ns_Time *stamp,
         const char *msg, size_t len)
{
    Ns_ReturnCode         status;

    NS_NONNULL_ASSERT(arg != NULL);
    NS_NONNULL_ASSERT(stamp != NULL);
    NS_NONNULL_ASSERT(msg != NULL);

    if (severity == Fatal) {
        /*
         * In case of Fatal severity, do nothing. We have to assume
         * that Tcl is not functioning anymore.
         */
        status = NS_OK;

    } else {
        int                   ii, ret;
        void                 *logfile = INT2PTR(STDERR_FILENO);
        Tcl_Obj              *stampObj;
        Ns_DString            ds, ds2;
        Tcl_Interp           *interp;
        const Ns_TclCallback *cbPtr = (Ns_TclCallback *)arg;

        /*
         * Try to obtain an interpreter:
         */
        interp = Ns_TclAllocateInterp(cbPtr->server);
        if (interp == NULL) {
            (void)LogToFile(logfile, Error, stamp,
                            "LogToTcl: can't get interpreter", 0u);
            status = NS_ERROR;
        } else {

            Ns_DStringInit(&ds);
            stampObj = Tcl_NewObj();
            Ns_TclSetTimeObj(stampObj, stamp);

            /*
             * Construct args for passing to the callback script:
             *
             *      callback severity timestamp log ?arg...?
             *
             * The script may contain blanks therefore append as regular
             * string instead of as list element.  Other arguments are
             * appended to it as elements.
             */
            Ns_DStringVarAppend(&ds, cbPtr->script, " ", Ns_LogSeverityName(severity), (char *)0L);
            Ns_DStringAppendElement(&ds, Tcl_GetString(stampObj));
            Tcl_DecrRefCount(stampObj);

            /*
             * Append n bytes of msg as proper list element to ds. Since
             * Tcl_DStringAppendElement has no length parameter, we have
             * to use a temporary DString here.
             */
            Ns_DStringInit(&ds2);
            Ns_DStringNAppend(&ds2, msg, (int)len);
            Ns_DStringAppendElement(&ds, ds2.string);
            Ns_DStringFree(&ds2);

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
                (void)LogToFile(logfile, Error, stamp, Ns_DStringValue(&ds),
                                (size_t)Ns_DStringLength(&ds));
            }
            Ns_DStringFree(&ds);
            Ns_TclDeAllocateInterp(interp);

            status = (ret == TCL_ERROR) ? NS_ERROR: NS_OK;
        }
    }
    return status;
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
#if defined(NS_THREAD_LOCAL)
    static NS_THREAD_LOCAL LogCache *cachePtr = NULL;

    if (cachePtr == NULL) {
        cachePtr = ns_calloc(1u, sizeof(LogCache));
        Ns_DStringInit(&cachePtr->buffer);
        Ns_TlsSet(&tls, cachePtr);
    }
#else
    LogCache *cachePtr;

    cachePtr = Ns_TlsGet(&tls);
    if (cachePtr == NULL) {
        cachePtr = ns_calloc(1u, sizeof(LogCache));
        Ns_DStringInit(&cachePtr->buffer);
        Ns_TlsSet(&tls, cachePtr);
    }
#endif
    return cachePtr;
}

#if !defined(NS_THREAD_LOCAL)
static void LogEntriesFree(void *arg)
{
    LogEntry *logEntry = (LogEntry *)arg;
    fprintf(stderr, "LOGENTRY: free list %p\n", (void*)logEntry);
    ns_free(logEntry);
}
#endif

static LogEntry *
LogEntryGet(LogCache *cachePtr)
{
    LogEntry *entryPtr;

    if (cachePtr->hold) {
        entryPtr = ns_malloc(sizeof(LogEntry));
    } else {
#if defined(NS_THREAD_LOCAL)
        static NS_THREAD_LOCAL LogEntry logEntry = {0};
        entryPtr = &logEntry;
#else
        LogCache *logEntryList;

        logEntryList = Ns_TlsGet(&tlsEntry);
        if (logEntryList == NULL) {
            logEntryList = ns_calloc(1u, sizeof(LogEntry));
            entryPtr = logEntryList;
            Ns_TlsSet(&tlsEntry, logEntryList);
        } else {
            entryPtr = logEntryList;
            logEntryList = entryPtr->nextPtr;
            entryPtr->nextPtr = NULL;
        }
#endif
    }
    return entryPtr;
}

static void
LogEntryFree(LogCache *cachePtr, LogEntry *logEntryPtr)
{
    if (cachePtr->hold) {
        memset(logEntryPtr, 0, sizeof(LogEntry));
        ns_free(logEntryPtr);
    } else {
#if defined(NS_THREAD_LOCAL)
#else
        logEntryList = Ns_TlsGet(&tlsEntry);
        logEntryPtr->nextPtr = logEntryList;
        if (logEntryList != NULL) {
            logEntryList->nextPtr = logEntryPtr;
        }
#endif
        /*fprintf(stderr, "LOGENTRY: pushed\n");*/
    }
}


/*
 *----------------------------------------------------------------------
 *
 * FreeCache --
 *
 *      TLS cleanup callback to destroy per-thread Cache struct.
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

    if (!cachePtr->finalizing) {

        cachePtr->finalizing = NS_TRUE;

        LogFlush(cachePtr, filters, -1, NS_TRUE, NS_TRUE);

        Ns_DStringFree(&cachePtr->buffer);
        ns_free(cachePtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * GetSeverityFromObj --
 *
 *      Get the severity level from the Tcl_Obj, possibly setting
 *      its internal representation.
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
GetSeverityFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, void **addrPtrPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(addrPtrPtr != NULL);

    if (Ns_TclGetOpaqueFromObj(objPtr, severityType, addrPtrPtr) != TCL_OK) {
        const Tcl_HashEntry *hPtr;

        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(&severityTable, Tcl_GetString(objPtr));
        Ns_MutexUnlock(&lock);

        if (hPtr != NULL) {
            *addrPtrPtr = Tcl_GetHashValue(hPtr);
        } else {
            int  i;
            /*
             * Check for a legacy integer severity.
             */
            if (Tcl_GetIntFromObj(NULL, objPtr, &i) == TCL_OK
                && i < severityMaxCount) {
                *addrPtrPtr = INT2PTR(i);
            } else {
                Tcl_DString ds;

                Tcl_DStringInit(&ds);
                Ns_DStringPrintf(&ds, "unknown severity: \"%s\":"
                                 " should be one of: ", Tcl_GetString(objPtr));
                for (i = 0; i < severityIdx; i++) {
                    Ns_DStringAppend(&ds, severityConfig[i].label);
                    Ns_DStringNAppend(&ds, " ", 1);
                }
                Tcl_DStringResult(interp, &ds);
                result = TCL_ERROR;
            }
        }
        if (result == TCL_OK) {
            /*
             * Stash the severity for future speedy lookup.
             */
            Ns_TclSetOpaqueObj(objPtr, severityType, *addrPtrPtr);
        }
    }

    return result;
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
Ns_SetLogFlushProc(Ns_LogFlushProc *UNUSED(procPtr))
{
    Ns_Fatal("Ns_SetLogFlushProc: deprecated, use Ns_AddLogFilter() instead");
}

void
Ns_SetNsLogProc(Ns_LogProc *UNUSED(procPtr))
{
    Ns_Fatal("Ns_SetNsLogProc: deprecated, use Ns_AddLogFilter() instead");
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
