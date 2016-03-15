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

/*
 * The following define available flags bits.
 */

#define LOG_ROLL     0x01u
#define LOG_EXPAND   0x02u
#define LOG_USEC     0x04u
#define LOG_COLORIZE 0x08u

/*
 * The following struct represents a log entry header as stored in
 * the per-thread cache. It is followed by a variable-length log
 * string as passed by the caller (format expanded).
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
    Ns_Callback       *freeArgProc; /* User-given function to free passed arg */
    void              *arg;         /* Argument passed to proc and free */
    int                refcnt;      /* Number of current consumers */
    struct LogFilter  *nextPtr;      /* Maintains double linked list */
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
    size_t      gbufSize;
    size_t      lbufSize;
    LogEntry   *firstEntry;   /* First in the list of log entries */
    LogEntry   *currEntry;    /* Current in the list of log entries */
    Ns_DString  buffer;       /* The log entries cache text-cache */
} LogCache;

/*
 * Local functions defined in this file
 */

static Ns_TlsCleanup FreeCache;
static Tcl_PanicProc Panic;

static Ns_LogFilter LogToFile;
static Ns_LogFilter LogToTcl;
static Ns_LogFilter LogToDString;

static LogCache* GetCache(void)
    NS_GNUC_RETURNS_NONNULL;

static int GetSeverityFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
                              void **addrPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void  LogFlush(LogCache *cachePtr, LogFilter *listPtr, int count,
                      bool trunc, bool locked)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int   LogOpen(void);

static char* LogTime(LogCache *cachePtr, const Ns_Time *timePtr, int gmt)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Tcl_Obj *LogStats(void);

/*
 * Static variables defined in this file
 */

static Ns_Tls       tls;
static Ns_Mutex     lock;
static Ns_Cond      cond;

static const char  *file;
static unsigned int flags = 0u;
static int          maxback;

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

static char *LogSeverityColor(char *buffer, Ns_LogSeverity severity) NS_GNUC_NONNULL(1);
static int ObjvTableLookup(const char *path, const char *param, Ns_ObjvTable *tablePtr, int *idxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);


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
    Tcl_InitHashTable(&severityTable, TCL_STRING_KEYS);

    Tcl_SetPanicProc(Panic);
    Ns_AddLogFilter(LogToFile, INT2PTR(STDERR_FILENO), NULL);

    /*
     * Initialise the entire space with backwards-compatible integer keys.
     */

    for (i = PredefinedLogSeveritiesCount; i < severityMaxCount; i++) {
        snprintf(buf, sizeof(buf), "%d", i);
        hPtr = Tcl_CreateHashEntry(&severityTable, buf, &isNew);
        Tcl_SetHashValue(hPtr, INT2PTR(i));
        severityConfig[i].label = Tcl_GetHashKey(&severityTable, hPtr);
        severityConfig[i].enabled = 0;
    }

    /*
     * Initialise the built-in severities and lower-case aliases.
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
 *      Lookup a value from an Ns_ObjvTable and return its associated value
 *      in the last parameter, if the lookup was successful.
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
    
    valueString = Ns_ConfigString(path, param, "");

    len = strlen(valueString);
    if (len > 0u) {
        Ns_ObjvSpec  spec;
        Tcl_Obj     *objPtr = Tcl_NewStringObj(valueString, (int)len);

        spec.arg  = tablePtr;
        spec.dest = idxPtr;
        result = Ns_ObjvIndex(&spec, NULL, &pos, &objPtr);

        if (result != TCL_OK) {
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
 *      Depends on config file.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigLog(void)
{
    Ns_DString  ds;
    const char *path = NS_CONFIG_PARAMETERS;
    Ns_Set     *set  = Ns_ConfigCreateSection(path);

    severityConfig[Debug ].enabled = Ns_ConfigBool(path, "logdebug",  NS_FALSE);
    severityConfig[Dev   ].enabled = Ns_ConfigBool(path, "logdev",    NS_FALSE);
    severityConfig[Notice].enabled = Ns_ConfigBool(path, "lognotice", NS_TRUE);

    if (Ns_ConfigBool(path, "logroll", NS_TRUE) == NS_TRUE) {
        flags |= LOG_ROLL;
    }
    if (Ns_ConfigBool(path, "logusec", NS_FALSE) == NS_TRUE) {
        flags |= LOG_USEC;
    }
    if (Ns_ConfigBool(path, "logexpanded", NS_FALSE) == NS_TRUE) {
        flags |= LOG_EXPAND;
    }
    if (Ns_ConfigBool(path, "logcolorize", NS_FALSE) == NS_TRUE) {
        flags |= LOG_COLORIZE;
    }
    if ((flags & LOG_COLORIZE) != 0u) {
        int result, idx;

        result = ObjvTableLookup(path, "logprefixcolor", colors, &idx);
        if (result == TCL_OK) {
            prefixColor = (LogColor)idx;
        }
        result = ObjvTableLookup(path, "logprefixintensity", intensities, &idx);
        if (result == TCL_OK) {
            prefixIntensity = (LogColorIntensity)idx;
        }
    }

    maxback  = Ns_ConfigIntRange(path, "logmaxbackup", 10, 0, 999);

    file = Ns_ConfigString(path, "serverlog", "nsd.log");
    if (Ns_PathIsAbsolute(file) == NS_FALSE) {
        Ns_DStringInit(&ds);
        if (Ns_HomePathExists("logs", (char *)0)) {
            Ns_HomePath(&ds, "logs", file, NULL);
        } else {
            Ns_HomePath(&ds, file, NULL);
        }
        file = Ns_DStringExport(&ds);
	Ns_SetUpdate(set, "serverlog", file);
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

const char *
Ns_InfoErrorLog(void)
{
    return file;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CreateLogSeverity --
 *
 *      Create and return a new log severity with the given name, which
 *      will initially be disabled (except for the built-ins).
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
Ns_CreateLogSeverity(const char *name)
{
    Ns_LogSeverity  severity;
    Tcl_HashEntry  *hPtr;
    int             isNew;

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

const char *
Ns_LogSeverityName(Ns_LogSeverity severity)
{
    if (severity < severityMaxCount) {
        return severityConfig[severity].label;
    }
    return "Unknown";
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
        sprintf(buffer, "%s%d;%dm", LOG_COLORSTART, severityConfig[severity].intensity, severityConfig[severity].color);
    } else {
        sprintf(buffer, "%s0m", LOG_COLORSTART);
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
 *      Upadating severityConfig
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
 *      Return a Tcl list containing the labels and counts for all severities.
 *      The function should be probably be guarded by a lock, but we have just
 *      single word operations and potentially incorrect counts are not fatal.
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
Ns_VALog(Ns_LogSeverity severity, const char *fmt, va_list *const vaPtr)
{
    size_t    length, offset;
    LogCache *cachePtr;
    LogEntry *entryPtr = NULL;

    NS_NONNULL_ASSERT(fmt != NULL);

    /*
     * Skip if logging for selected severity is disabled
     * or if severity level out of range(s).
     */

    if (Ns_LogSeverityEnabled(severity) == NS_FALSE) {
        return;
    }
    severityConfig[severity].count ++;

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

    offset = (size_t)Ns_DStringLength(&cachePtr->buffer);
    Ns_DStringVPrintf(&cachePtr->buffer, fmt, *vaPtr);
    length = (size_t)Ns_DStringLength(&cachePtr->buffer) - offset;

    entryPtr->severity = severity;
    entryPtr->offset   = offset;
    entryPtr->length   = length;
    Ns_GetTime(&entryPtr->stamp);

    /*
     * Flush it out if not held
     */

    if (cachePtr->hold == 0 || severity == Fatal) {
        LogFlush(cachePtr, filters, -1, NS_TRUE, NS_TRUE);
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
Ns_AddLogFilter(Ns_LogFilter *procPtr, void *arg, Ns_Callback *freeProc)
{
    LogFilter *filterPtr = ns_calloc(1U, sizeof *filterPtr);

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
    NS_NONNULL_ASSERT(timeBuf != NULL);

    return Ns_LogTime2(timeBuf, 1);
}

char *
Ns_LogTime2(char *timeBuf, int gmt)
{
    Ns_Time now;
    LogCache   *cachePtr = GetCache();
    const char *timeString;
    size_t      timeStringLength;

    NS_NONNULL_ASSERT(timeBuf != NULL);

    /*
     * Add the log stamp
     */
    Ns_GetTime(&now);
    timeString = LogTime(cachePtr, &now, gmt);
    timeStringLength = (gmt == 0) ? cachePtr->lbufSize : cachePtr->gbufSize;

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
LogTime(LogCache *cachePtr, const Ns_Time *timePtr, int gmt)
{
    time_t    *tp;
    char      *bp;
    size_t    *sizePtr;

    NS_NONNULL_ASSERT(cachePtr != NULL);
    NS_NONNULL_ASSERT(timePtr != NULL);

    if (gmt != 0) {
        tp = &cachePtr->gtime;
        bp = cachePtr->gbuf;
        sizePtr = &cachePtr->gbufSize;
    } else {
        tp = &cachePtr->ltime;
        bp = cachePtr->lbuf;
        sizePtr = &cachePtr->lbufSize;
    }
    
    /*
     * Check if the value for seconds in the cache is the same as the required
     * value. If not, recompute the string and store it in the cache.x
     */
    if (*tp != timePtr->sec) {
        size_t n;
	time_t secs;
	struct tm *ptm;

        *tp = timePtr->sec;

	secs = timePtr->sec;
        ptm = ns_localtime(&secs);

        n = strftime(bp, 32u, "[%d/%b/%Y:%H:%M:%S", ptm);
        if (gmt == 0) {
            bp[n++] = ']';
            bp[n] = '\0';
        } else {
 	    int gmtoff, sign;
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
            n += (size_t)sprintf(bp + n, " %c%02d%02d]", sign, gmtoff/60, gmtoff%60);
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
NsTclLogObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_LogSeverity severity;
    Ns_DString     ds;
    void 	  *addrPtr;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "severity string ?string ...?");
        return TCL_ERROR;
    }
    if (GetSeverityFromObj(interp, objv[1], &addrPtr) != TCL_OK) {
        return TCL_ERROR;
    }
    severity = PTR2INT(addrPtr);

    if (objc == 3) {
        Ns_Log(severity, "%s", Tcl_GetString(objv[2]));
    } else {
        int i;

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
NsTclLogCtlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int             result = TCL_OK, count, opt, i;
    Ns_DString      ds;
    Tcl_Obj        *objPtr;
    LogCache       *cachePtr = GetCache();
    LogFilter       filter, *filterPtr = &filter;
    void           *addr;
    Ns_TclCallback *cbPtr;

    static const char *const opts[] = {
        "hold", "count", "get", "peek", "flush", "release",
        "truncate", "severity", "severities", "stats",
        "register", "unregister", NULL
    };
    enum {
        CHoldIdx, CCountIdx, CGetIdx, CPeekIdx, CFlushIdx, CReleaseIdx,
        CTruncIdx, CSeverityIdx, CSeveritiesIdx, CStatsIdx,
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
            result = TCL_ERROR;
        } else {
            cbPtr = Ns_TclNewCallback(interp, Ns_TclCallbackProc,
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
        cachePtr->hold = 1;
        break;

    case CPeekIdx:
    case CGetIdx:
        memset(filterPtr, 0, sizeof *filterPtr);
        filterPtr->proc = LogToDString;
        filterPtr->arg  = &ds;
        Ns_DStringInit(&ds);
        LogFlush(cachePtr, filterPtr, -1, (opt == CGetIdx) ? NS_TRUE : NS_FALSE, NS_FALSE);
        Tcl_DStringResult(interp, &ds);
        Ns_DStringFree(&ds);
        break;

    case CReleaseIdx:
        cachePtr->hold = 0;
        /* FALLTHROUGH */

    case CFlushIdx:
        LogFlush(cachePtr, filters, -1, NS_TRUE, NS_TRUE);
        break;

    case CCountIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(cachePtr->count));
        break;

    case CTruncIdx:
        count = 0;
        if (objc > 2 && Tcl_GetIntFromObj(interp, objv[2], &count) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            memset(filterPtr, 0, sizeof *filterPtr);
            LogFlush(cachePtr, filterPtr, count, NS_TRUE, NS_FALSE);
        }
        break;

    case CSeverityIdx:
      {
          Ns_LogSeverity    severity = 0; /* default value for the error cases */
          void             *addrPtr = NULL;
          bool              enabled;
          int               color = -1, intensity = -1, givenEnabled = -1;

          Ns_ObjvSpec lopts[] = {
              {"-color",     Ns_ObjvIndex,  &color,     colors},
              {"-intensity", Ns_ObjvIndex,  &intensity, intensities},
              {"--",         Ns_ObjvBreak,  NULL,       NULL},
              {NULL, NULL, NULL, NULL}
          };
          Ns_ObjvSpec args[] = {
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

          if (likely(result == TCL_OK) && Ns_ParseObjv(lopts, args, interp, 2, objc-1, objv+1) != NS_OK) {
              result = TCL_ERROR;
          }

          if (likely(result == TCL_OK)) {

              assert(severity < severityMaxCount);

              /*
               * Don't allow to deactivate Fatal.
               */
              if (givenEnabled != -1 && severity != Fatal) {
                  enabled = severityConfig[severity].enabled;
                  severityConfig[severity].enabled = givenEnabled;
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

          break;
      }

    case CSeveritiesIdx:
        /*
         * Return all registered severites in a list
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

    return result;
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
NsTclLogRollObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
		   int UNUSED(objc), Tcl_Obj *CONST* UNUSED(objv))
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
 *      Signal handler for SIGHUP which will roll the files.
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
    int rc = NS_OK;

    if (file != NULL) {
	NsAsyncWriterQueueDisable(0);

        if (access(file, F_OK) == 0) {
            (void) Ns_RollFile(file, maxback);
        }
        Ns_Log(Notice, "log: re-opening log file '%s'", file);
	rc = LogOpen();

	NsAsyncWriterQueueEnable();
    }

    return rc;
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
    if ((flags & LOG_ROLL) != 0u) {
        (void) Ns_RegisterAtSignal((Ns_Callback *) Ns_LogRoll, NULL);
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
    int          fd, status = NS_OK;
    unsigned int oflags;

    oflags = O_WRONLY | O_APPEND | O_CREAT;

#ifdef O_LARGEFILE
    oflags |= O_LARGEFILE;
#endif

    fd = ns_open(file, (int)oflags, 0644);
    if (fd == NS_INVALID_FD) {
    	Ns_Log(Error, "log: failed to re-open log file '%s': '%s'",
               file, strerror(errno));
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
    int        status, nentry = 0;
    LogFilter *cPtr;
    LogEntry  *ePtr;

    NS_NONNULL_ASSERT(cachePtr != NULL);
    NS_NONNULL_ASSERT(listPtr != NULL);

    ePtr = cachePtr->firstEntry;
    while (ePtr != NULL && cachePtr->currEntry != NULL) {
        const char *logString = Ns_DStringValue(&cachePtr->buffer) + ePtr->offset;

        if (locked == NS_TRUE) {
            Ns_MutexLock(&lock);
        }
        
        /*
         * Since listPtr is never NULL, a repeat-unil loop is sufficient to
         * guarantee that the initial cPtr is not NULL either.
         */
        cPtr = listPtr;
        do {
            if (cPtr->proc != NULL) {
                if (locked == NS_TRUE) {
                    cPtr->refcnt++;
                    Ns_MutexUnlock(&lock);
                }
                status = (*cPtr->proc)(cPtr->arg, ePtr->severity,
                                       &ePtr->stamp, logString, ePtr->length);
                if (locked == NS_TRUE) {
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
                    (void) LogToFile(INT2PTR(STDERR_FILENO), ePtr->severity,
                                     &ePtr->stamp, logString, ePtr->length);
                    break;
                }
            }
            cPtr = cPtr->prevPtr;
        } while (cPtr != NULL);
        
        if (locked == NS_TRUE) {
            Ns_MutexUnlock(&lock);
        }
        nentry++;
        if ((count > 0 && nentry >= count) || ePtr == cachePtr->currEntry) {
            break;
        }
        ePtr = ePtr->nextPtr;
    }

    if (trunc == NS_TRUE) {
        if (count > 0) {
	    size_t length = (ePtr != NULL) ? (ePtr->offset + ePtr->length) : 0u;
            cachePtr->count = (length != 0u) ? nentry : 0;
            cachePtr->currEntry = ePtr;
            Ns_DStringSetLength(&cachePtr->buffer, (int)length);
        } else {
	    LogEntry *entryPtr, *tmpPtr;

	    /*
	     * The cache is reset (count <= 0). If there are log
	     * entries in the cache, flush these before setting the
	     * pointers to zero.
	     */
	    for (entryPtr = cachePtr->firstEntry; entryPtr != NULL; entryPtr = tmpPtr) {
	        tmpPtr = entryPtr->nextPtr;
	        ns_free(entryPtr);
	    }
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
LogToDString(void *arg, Ns_LogSeverity severity, const Ns_Time *stamp,
            const char *msg, size_t len)
{
    Ns_DString *dsPtr  = (Ns_DString *)arg;
    LogCache   *cachePtr = GetCache();
    const char *timeString;
    size_t      timeStringLength;
    char        buffer[255];

    NS_NONNULL_ASSERT(arg != NULL);
    NS_NONNULL_ASSERT(stamp != NULL);
    NS_NONNULL_ASSERT(msg != NULL);

    /*
     * Add the log stamp
     */
    timeString = LogTime(cachePtr, stamp, 0);
    timeStringLength = cachePtr->lbufSize;

    if ((flags & LOG_COLORIZE) != 0u) {
        Ns_DStringPrintf(dsPtr, "%s%d;%dm", LOG_COLORSTART, prefixIntensity, prefixColor);
    }

    Ns_DStringNAppend(dsPtr, timeString, (int)timeStringLength);
    if ((flags & LOG_USEC) != 0u) {
        Ns_DStringSetLength(dsPtr, Ns_DStringLength(dsPtr) - 1);
        Ns_DStringPrintf(dsPtr, ".%06ld]", stamp->usec);
    }
    if ((flags & LOG_COLORIZE) != 0u) {
        Ns_DStringPrintf(dsPtr, "[%d.%" PRIxPTR "][%s] %s%s%s: ",
                         (int)Ns_InfoPid(), Ns_ThreadId(), Ns_ThreadGetName(),
                         (const char *)LOG_COLOREND,
                         LogSeverityColor(buffer, severity),
                         Ns_LogSeverityName(severity));
    } else {
        Ns_DStringPrintf(dsPtr, "[%d.%" PRIxPTR "][%s] %s: ",
                         (int)Ns_InfoPid(), Ns_ThreadId(), Ns_ThreadGetName(),
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
    Ns_DStringNAppend(dsPtr, msg, (int)len);
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

static int
LogToFile(void *arg, Ns_LogSeverity severity, const Ns_Time *stamp,
          const char *msg, size_t len)
{
    int        fd = PTR2INT(arg);
    Ns_DString ds;

    NS_NONNULL_ASSERT(arg != NULL);
    NS_NONNULL_ASSERT(stamp != NULL);
    NS_NONNULL_ASSERT(msg != NULL);

    Ns_DStringInit(&ds);

    LogToDString(&ds, severity, stamp, msg, len);
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
 *      This call deliberately does not use Ns_TclEvalCallback(),
 *      as if the Tcl code throws error, that one will invoke
 *      Ns_TclLogError() and will deadlock in the log code.
 *
 *----------------------------------------------------------------------
 */

static int
LogToTcl(void *arg, Ns_LogSeverity severity, const Ns_Time *stamp,
         const char *msg, size_t len)
{
    int             ii, ret;
    void           *logfile = INT2PTR(STDERR_FILENO);
    Tcl_Obj        *stampObj;
    Ns_DString      ds, ds2;
    Tcl_Interp     *interp;
    Ns_TclCallback *cbPtr = (Ns_TclCallback *)arg;

    NS_NONNULL_ASSERT(arg != NULL);
    NS_NONNULL_ASSERT(stamp != NULL);
    NS_NONNULL_ASSERT(msg != NULL);

    if (severity == Fatal) {
        return NS_OK;
    }

    interp = Ns_TclAllocateInterp(cbPtr->server);
    if (interp == NULL) {
        char *err = "LogToTcl: can't get interpreter";
        (void)LogToFile(logfile, Error, stamp, err, 0u);
        return NS_ERROR;
    }

    Ns_DStringInit(&ds);
    stampObj = Tcl_NewObj();
    Ns_TclSetTimeObj(stampObj, stamp);

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
    Ns_DStringAppendElement(&ds, Tcl_GetString(stampObj));
    Tcl_DecrRefCount(stampObj);

    /*
     * Append n bytes of msg as proper list element to ds. Since
     * Tcl_DStringAppendElement has no length parameter, we have to
     * use a temporary DString here.
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
        cachePtr = ns_calloc(1u, sizeof(LogCache));
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

    LogFlush(cachePtr, filters, -1, NS_TRUE, NS_TRUE);
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
GetSeverityFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, void **addrPtrPtr)
{
    int            i;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(addrPtrPtr != NULL);

    if (Ns_TclGetOpaqueFromObj(objPtr, severityType, addrPtrPtr) != TCL_OK) {
	Tcl_HashEntry *hPtr;

        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(&severityTable, Tcl_GetString(objPtr));
        Ns_MutexUnlock(&lock);

        if (hPtr != NULL) {
            *addrPtrPtr = Tcl_GetHashValue(hPtr);
        } else {
            /*
             * Check for a legacy integer severity.
             */
            if (Tcl_GetIntFromObj(NULL, objPtr, &i) == TCL_OK
		&& i < severityMaxCount) {
		*addrPtrPtr = INT2PTR(i);
            } else {
                Tcl_AppendResult(interp, "unknown severity: \"",
                                 Tcl_GetString(objPtr),
                                 "\": should be one of: ", NULL);
                for (i = 0; i < severityIdx; i++) {
                    Tcl_AppendResult(interp, severityConfig[i].label, " ", NULL);
                }
                return TCL_ERROR;
            }
        }
        /*
         * Stash the severity for future speedy lookup.
         */
        Ns_TclSetOpaqueObj(objPtr, severityType, *addrPtrPtr);
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
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
