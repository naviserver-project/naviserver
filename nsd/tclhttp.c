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
 * tclhttp.c --
 *
 *      Support for the ns_http command.
 *      Uses the Ns_Task interface to run/queue HTTP tasks.
 */

#include "nsd.h"

#ifdef HAVE_OPENSSL_EVP_H
#include <openssl/err.h>
#endif

#define TCLHTTP_USE_EXTERNALTOUTF 1

/*
 * The maximum number of bytes we can send to TLS
 * in one operation is 2^14 => 16384 (see RFC 5246).
 * This is used when reading data from file/channel
 * and writing data to the connected socket.
 *
 * At some point, this should be abstracted by the
 * future socket communication module.
 */
#define CHUNK_SIZE 16384

/*
 * String equivalents of some methods, header keys
 */
static const char *transferEncodingHeader = "Transfer-Encoding";
static const char *acceptEncodingHeader   = "Accept-Encoding";
static const char *contentEncodingHeader  = "Content-Encoding";
static const char *contentTypeHeader      = "Content-Type";
static const char *contentLengthHeader    = "Content-Length";
static const char *connectionHeader       = "Connection";
static const char *trailersHeader         = "Trailers";
static const char *hostHeader             = "Host";
static const char *userAgentHeader        = "User-Agent";
static const char *connectMethod          = "CONNECT";

static const int acceptEncodingHeaderLength = 15;

/*
 * Attempt to maintain Tcl errorCode variable.
 * This is still not done thoroughly through the code.
 */
static const char *errorCodeTimeoutString = "NS_TIMEOUT";

/*
 * For http task mutex naming
 */
static uint64_t httpClientRequestCount = 0u; /* MT: static variable! */
static Ns_TaskQueue *taskQueue = NULL; /* MT: static variable! */

/*
 * Local functions defined in this file
 */
static bool InitializeTaskQueue(void);

static int HttpQueue(
    NsInterp *itPtr,
    TCL_OBJC_T objc,
    Tcl_Obj *const*
    objv,
    bool run
) NS_GNUC_NONNULL(1);

static int HttpConnect(
    NsInterp *itPtr,
    const char *method,
    const char *url,
    Tcl_Obj *proxyObj,
    Ns_Set *hdrPtr,
    ssize_t bodySize,
    Tcl_Obj *bodyObj,
    const char *bodyFileName,
    const char *cert,
    const char *caFile,
    const char *caPath,
    const char *sniHostname,
    bool verifyCert,
    bool keepHostHdr,
    Ns_Time *timeoutPtr,
    Ns_Time *expirePtr,
    Ns_Time *keepAliveTimeoutPtr,
    NsHttpTask **httpPtrPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(18);

static bool HttpGet(
    NsInterp *itPtr,
    const char *taskID,
    NsHttpTask **httpPtrPtr,
    bool remove
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void HttpClose(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static void HttpCleanupPerRequestData(
    NsHttpTask *httpPtr
)  NS_GNUC_NONNULL(1);

static void HttpCancel(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static int HttpAppendContent(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int HttpAppendChunked(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int HttpAppendBuffer(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int HttpAppendRawBuffer(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int
SkipMessage(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static Ns_ReturnCode HttpWaitForSocketEvent(
    NS_SOCKET sock,
    short events,
    const Ns_Time *timeoutPtr
);

static void HttpAddInfo(
    NsHttpTask *httpPtr,
    const char *key,
    const char *value
)  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void HttpCheckHeader(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static int HttpCheckSpool(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static ssize_t HttpTaskSend(
    const NsHttpTask *httpPtr,
    const void *buffer,
    size_t length
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static ssize_t HttpTaskRecv(
    const NsHttpTask *httpPtr,
    char *buffer,
    size_t length,
    Ns_SockState *statePtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int HttpCutChannel(
    Tcl_Interp *interp,
    Tcl_Channel chan
) NS_GNUC_NONNULL(2);

static void HttpSpliceChannel(
    Tcl_Interp *interp,
    Tcl_Channel chan
) NS_GNUC_NONNULL(2);

static void HttpSpliceChannels(
    Tcl_Interp *interp,
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(2);

static int HttpGetResult(
    Tcl_Interp *interp,
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void HttpDoneCallback(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static void HttpClientLogWrite(
    const NsHttpTask *httpPtr,
    const char       *causeString
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static NS_SOCKET HttpTunnel(
    NsInterp *itPtr,
    const char *proxyhost,
    unsigned short proxyport,
    const char *host,
    unsigned short port,
    const Ns_Time *timeout
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);


static bool PersistentConnectionLookup(NsHttpTask *httpPtr, NsHttpTask **waitingHttpPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static bool PersistentConnectionAdd(NsHttpTask *httpPtr, const char **reasonPtr)
    NS_GNUC_NONNULL(1);
static bool PersistentConnectionDelete(NsHttpTask *httpPtr)
    NS_GNUC_NONNULL(1);

static void LogDebug(const char *before, NsHttpTask *httpPtr, const char *after)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static Ns_LogCallbackProc HttpClientLogOpen;
static Ns_LogCallbackProc HttpClientLogClose;
static Ns_LogCallbackProc HttpClientLogRoll;
static Ns_SchedProc       SchedLogRollCallback;
static Ns_ArgProc         SchedLogArg;

static Ns_TaskProc HttpProc;
static Ns_TaskProc CloseWaitProc;

/*
 * Function implementing the Tcl interface.
 */
static TCL_OBJCMDPROC_T HttpCancelObjCmd;
static TCL_OBJCMDPROC_T HttpCleanupObjCmd;
static TCL_OBJCMDPROC_T HttpListObjCmd;
static TCL_OBJCMDPROC_T HttpStatsObjCmd;
static TCL_OBJCMDPROC_T HttpQueueObjCmd;
static TCL_OBJCMDPROC_T HttpRunObjCmd;
static TCL_OBJCMDPROC_T HttpWaitObjCmd;

static NsHttpParseProc ParseCRProc;
static NsHttpParseProc ParseLFProc;
static NsHttpParseProc ParseLengthProc;
static NsHttpParseProc ChunkInitProc;
static NsHttpParseProc ParseBodyProc;
static NsHttpParseProc TrailerInitProc;
static NsHttpParseProc ParseTrailerProc;
static NsHttpParseProc ParseEndProc;

static char* SkipDigits(char *chars) NS_GNUC_NONNULL(1);
static char *DStringAppendHttpFlags(Tcl_DString *dsPtr, unsigned int flags) NS_GNUC_NONNULL(1);

/*
 * Callbacks for the chunked-encoding state machine
 * to parse variable number of chunks.
 */
static NsHttpParseProc* ChunkParsers[] = {
    &ChunkInitProc,
    &ParseLengthProc,
    &ParseCRProc,
    &ParseLFProc,
    &ParseBodyProc,
    &ParseCRProc,
    &ParseLFProc,
    NULL
};

/*
 * Callbacks for the chunked-encoding parse machine
 * to parse variable number of optional trailers.
 */
static NsHttpParseProc* TrailerParsers[] = {
    &TrailerInitProc,
    &ParseTrailerProc,
    &ParseCRProc,
    &ParseLFProc,
    NULL
};

/*
 * Callbacks for the chunked-encoding parse machine
 * to parse terminating frame (CRLF sequence).
 */
static NsHttpParseProc* EndParsers[] = {
    &ParseCRProc,
    &ParseLFProc,
    &ParseEndProc,
    NULL
};

/*----------------------------------------------------------------------
 *
 * DStringAppendHttpFlags --
 *
 *      Append the provided taskHTTP flags in human readable form.
 *
 * Results:
 *      Tcl_DString value
 *
 * Side effects:
 *      Appends to the Tcl_DString
 *
 *----------------------------------------------------------------------
 */

static char *
DStringAppendHttpFlags(Tcl_DString *dsPtr, unsigned int flags)
{
    int    count = 0;
    size_t i;
    static const struct {
        unsigned int state;
        const char  *label;
    } options[] = {
        { NS_HTTP_FLAG_DECOMPRESS,    "DECOMPRESS" },
        { NS_HTTP_FLAG_GZIP_ENCODING, "GZIP" },
        { NS_HTTP_FLAG_CHUNKED,       "CHUNKED" },
        { NS_HTTP_FLAG_CHUNKED_END,   "CHUNKED_END" },
        { NS_HTTP_FLAG_BINARY,        "BINARY" },
        { NS_HTTP_FLAG_EMPTY,         "EMPTY" },
        { NS_HTTP_KEEPALIVE,          "KEEPALIVE" },
        { NS_HTTP_VERSION_1_1,        "1.1" },
        { NS_HTTP_STREAMING,          "STREAMING" },
        { NS_HTTP_HEADERS_PENDING,    "HDR_PENDING" }
    };

    NS_NONNULL_ASSERT(dsPtr != NULL);

    for (i=0; i<sizeof(options)/sizeof(options[0]); i++) {
        if ((options[i].state & flags) != 0u) {
            if (count > 0) {
                Tcl_DStringAppend(dsPtr, "|", 1);
            }
            Tcl_DStringAppend(dsPtr, options[i].label, TCL_INDEX_NONE);
            count ++;
        }
    }
    return dsPtr->string;
}



/*----------------------------------------------------------------------
 *
 * LogDebug --
 *
 *      When task debugging is on, write a standardized debug message to the
 *      log file, including the final sock state and error in human readable
 *      form.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Writes to the log file.
 *
 *----------------------------------------------------------------------
 */
static void
LogDebug(const char *before, NsHttpTask *httpPtr, const char *after)
{
    NS_NONNULL_ASSERT(before != NULL);
    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(after != NULL);

    if (Ns_LogSeverityEnabled(Ns_LogTaskDebug)) {
        Tcl_DString dsSockState, dsHttpState;

        Tcl_DStringInit(&dsSockState);
        Tcl_DStringInit(&dsHttpState);
        Ns_Log(Ns_LogTaskDebug, "%s httpPtr:%p flags:%s finalSockState:%s err:(%s) %s",
               before,
               (void*)httpPtr,
               DStringAppendHttpFlags(&dsHttpState, httpPtr->flags),
               Ns_DStringAppendSockState(&dsSockState, httpPtr->finalSockState),
               (httpPtr->error != NULL) ? httpPtr->error : "none",
               after);
        Tcl_DStringFree(&dsSockState);
        Tcl_DStringFree(&dsHttpState);
    }
}

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
NsInitHttp(NsServer *servPtr)
{
    const char  *path;

    NS_NONNULL_ASSERT(servPtr != NULL);

    //fprintf(stderr, "============== NsInitHttp %p ==============\n", (void*)servPtr);
    Ns_MutexInit(&servPtr->httpclient.lock);
    Ns_MutexSetName2(&servPtr->httpclient.lock, "httpclient", servPtr->server);

    Tcl_InitHashTable(&servPtr->httpclient.pconns, TCL_STRING_KEYS);

    NS_INIT_ONCE(InitializeTaskQueue);

    path = Ns_ConfigSectionPath(NULL, servPtr->server, NULL, "httpclient", (char *)0L);
    Ns_ConfigTimeUnitRange(path, "keepalive",
                           "0s", 0, 0, INT_MAX, 0, &servPtr->httpclient.keepaliveTimeout);

    servPtr->httpclient.logging = Ns_ConfigBool(path, "logging", NS_FALSE);

    if (servPtr->httpclient.logging) {
        const char  *filename;
        Tcl_DString  defaultLogFileName;

        Tcl_DStringInit(&defaultLogFileName);
        filename = Ns_ConfigString(path, "logfile", NULL);
        if (filename == NULL) {
            Tcl_DStringAppend(&defaultLogFileName, "httpclient-", 11);
            Tcl_DStringAppend(&defaultLogFileName, servPtr->server, TCL_INDEX_NONE);
            Tcl_DStringAppend(&defaultLogFileName, ".log", 4);
            filename = defaultLogFileName.string;
        }

        if (Ns_PathIsAbsolute(filename) == NS_TRUE) {
            servPtr->httpclient.logFileName = ns_strdup(filename);
        } else {
            Tcl_DString ds;

            Tcl_DStringInit(&ds);
            (void) Ns_HomePath(&ds, "logs", "/", filename, (char *)0L);
            servPtr->httpclient.logFileName = Ns_DStringExport(&ds);
        }
        Tcl_DStringFree(&defaultLogFileName);
        servPtr->httpclient.logRollfmt = ns_strcopy(Ns_ConfigGetValue(path, "logrollfmt"));
        servPtr->httpclient.logMaxbackup = (TCL_SIZE_T)Ns_ConfigIntRange(path, "logmaxbackup",
                                                                         100, 1, INT_MAX);

        HttpClientLogOpen(servPtr);

        /*
         *  Schedule various log roll and shutdown options.
         */

        if (Ns_ConfigBool(path, "logroll", NS_TRUE)) {
            int hour = Ns_ConfigIntRange(path, "logrollhour", 0, 0, 23);

            Ns_ScheduleDaily(SchedLogRollCallback, servPtr, 0u,
                             hour, 0, NULL);
        }
        if (Ns_ConfigBool(path, "logrollonsignal", NS_FALSE)) {
            Ns_RegisterAtSignal((Ns_Callback *)(ns_funcptr_t)SchedLogRollCallback, servPtr);
        }

        Ns_RegisterProcInfo((ns_funcptr_t)SchedLogRollCallback, "httpclientlog:roll", SchedLogArg);

    } else {
        servPtr->httpclient.fd = NS_INVALID_FD;
        servPtr->httpclient.logFileName = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SchedLogRollCallback --
 *
 *      Callback for scheduled procedure to roll the client logfile.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Rolling the client logfile when configured.
 *
 *----------------------------------------------------------------------
 */
static void
SchedLogRollCallback(void *arg, int UNUSED(id))
{
    NsServer *servPtr = (NsServer *)arg;

    Ns_Log(Notice, "httpclient: scheduled callback '%s'",
           servPtr->httpclient.logFileName);

    HttpClientLogRoll(servPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * SchedLogArg --
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
SchedLogArg(Tcl_DString *dsPtr, const void *arg)
{
    const NsServer *servPtr = (NsServer *)arg;

    Tcl_DStringAppendElement(dsPtr, servPtr->httpclient.logFileName);
}

/*
 *----------------------------------------------------------------------
 *
 * HttpClientLogRoll --
 *
 *      Rolling function for the client logfile.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Rolling the client logfile when configured.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
HttpClientLogRoll(void *arg)
{
    Ns_ReturnCode status = NS_OK;
    NsServer     *servPtr = (NsServer *)arg;

    Ns_Log(Notice, "httpclient: client roll '%s' (logging %d)",
           servPtr->httpclient.logFileName, servPtr->httpclient.logging);

    if (servPtr->httpclient.logging) {
        status = Ns_RollFileCondFmt(HttpClientLogOpen, HttpClientLogClose, servPtr,
                                    servPtr->httpclient.logFileName,
                                    servPtr->httpclient.logRollfmt,
                                    servPtr->httpclient.logMaxbackup);
    }
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HttpClientLogOpen --
 *
 *      Function for opening the client logfile. This function is only called,
 *      when logging is configured.
 *
 * Results:
 *      NS_OK, NS_ERROR
 *
 * Side effects:
 *      Opening the client logfile.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
HttpClientLogOpen(void *arg)
{
    Ns_ReturnCode status;
    NsServer     *servPtr = (NsServer *)arg;

    servPtr->httpclient.fd = ns_open(servPtr->httpclient.logFileName,
                                     O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC,
                                     0644);
    if (servPtr->httpclient.fd == NS_INVALID_FD) {
        Ns_Log(Error, "httpclient: error '%s' opening '%s'",
               strerror(errno), servPtr->httpclient.logFileName);
        status = NS_ERROR;
    } else {
        Ns_Log(Notice, "httpclient: logfile '%s' opened",
               servPtr->httpclient.logFileName);
        status = NS_OK;
    }
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * HttpClientLogClose --
 *
 *      Function for closing the client logfile when configured.
 *
 * Results:
 *      NS_OK, NS_ERROR
 *
 * Side effects:
 *      Closing the client logfile when configured.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
HttpClientLogClose(void *arg)
{
    Ns_ReturnCode status = NS_OK;
    NsServer     *servPtr = (NsServer *)arg;

    if (servPtr->httpclient.fd != NS_INVALID_FD) {
        Ns_Log(Notice, "httpclient: logfile '%s' try to close (fd %d)",
               servPtr->httpclient.logFileName, servPtr->httpclient.fd);

        ns_close(servPtr->httpclient.fd);
        servPtr->httpclient.fd = NS_INVALID_FD;
        Ns_Log(Notice, "httpclient: logfile '%s' closed",
               servPtr->httpclient.logFileName);
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * NsStopHttp --
 *
 *      Function to be called, when the server shuts down.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Closing the client logfile when configured.
 *
 *----------------------------------------------------------------------
 */
void
NsStopHttp(NsServer *servPtr)
{
    NS_NONNULL_ASSERT(servPtr != NULL);

    (void)HttpClientLogClose(servPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * SkipDigits --
 *
 *    Helper function of Ns_HttpParseHost() to skip digits in a string.
 *
 * Results:
 *    First non-digit character.
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */
static char*
SkipDigits(char *chars)
{
    NS_NONNULL_ASSERT(chars != NULL);

    for (; *chars  >= '0' && *chars <= '9'; chars++) {
        ;
    }
    return chars;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpParseHost2, Ns_HttpParseHost --
 *
 *      Obtain the hostname from a writable string
 *      using syntax as specified in RFC 3986 section 3.2.2.
 *
 *      Examples:
 *
 *          [2001:db8:1f70::999:de8:7648:6e8]:8000 (IP-literal notation)
 *          openacs.org:80                         (reg-name notation)
 *
 *      Ns_HttpParseHost() is the legacy version of Ns_HttpParseHost2().
 *
 * Results:
 *      Boolean value indicating success.
 *
 *      In addition, parts of the parsed content is returned via the
 *      provided pointers:
 *
 *      - If a port is indicated after the hostname, the "portStart"
 *        will contain a string starting with ":", otherwise NULL.
 *
 *      - If "hostStart" is non-null, a pointer will point to the
 *        hostname, which will be terminated by '\0' in case of an IPv6
 *        address in IP-literal notation.
 *
 *      Note: Ns_HttpParseHost2 can be used to parse empty host/port
 *      values. To detect these cases, use a test like
 *
 *        if (hostParsedOk && hostString != end && hostStart != portStart) ...
 *
 * Side effects:
 *      May write NUL character '\0' into the passed hostString.
 *
 *----------------------------------------------------------------------
 */
void
Ns_HttpParseHost(
    char *hostString,
    char **hostStart,
    char **portStart
) {
    char *end;

    NS_NONNULL_ASSERT(hostString != NULL);
    NS_NONNULL_ASSERT(portStart != NULL);

    (void) Ns_HttpParseHost2(hostString, NS_FALSE, hostStart, portStart, &end);
    if (*portStart != NULL) {
        /*
         * The old version was returning in portStart the position of the
         * character BEFORE the port (usually ':'). So, keep compatibility.
         */
        *portStart = *portStart-1;
    }
}

bool
Ns_HttpParseHost2(
    char *hostString,
    bool strict,
    char **hostStart,
    char **portStart,
    char **end
) {
    bool ipLiteral = NS_FALSE, success = NS_TRUE;

    /*
     * RFC 3986 defines
     *
     *   reg-name    = *( unreserved / pct-encoded / sub-delims )
     *   unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
     *   sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
     *               / "*" / "+" / "," / ";" / "="
     *
     *   ALPHA   = (%41-%5A and %61-%7A)
     *   DIGIT   = (%30-%39),
     *   hyphen (%2D), period (%2E), underscore (%5F), tilde (%7E)
     *   exclam (%21) dollar (%24) amp (%26) singlequote (%27)
     *   lparen (%28) lparen (%29) asterisk (%2A) plus (%2B)
     *   comma (%2C) semicolon (%3B) equals (%3D)
     *
     * However, errata #4942 of RFC 3986 says:
     *
     *   reg-name    = *( unreserved / pct-encoded / "-" / ".")
     *
     * A reg-name consists of a sequence of domain labels separated by ".",
     * each domain label starting and ending with an alphanumeric character
     * and possibly also containing "-" characters.  The rightmost domain
     * label of a fully qualified domain name in DNS may be followed by a
     * single "." and should be if it is necessary to distinguish between the
     * complete domain name and some local domain.
     *
     * Percent-encoded is just checked by the character range, but does not
     * check the two following (number) chars.
     *
     *   percent (%25) ... for percent-encoded
     */
    static const bool regname_table[256] = {
        /*          0  1  2  3   4  5  6  7   8  9  a  b   c  d  e  f */
        /* 0x00 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x10 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x20 */  0, 0, 0, 0,  0, 1, 0, 0,  0, 0, 0, 0,  0, 1, 1, 0,
        /* 0x30 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 0, 0,  0, 0, 0, 0,
        /* 0x40 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 1,
        /* 0x60 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 1, 0,
        /* 0x80 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x90 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xa0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xb0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xc0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xd0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xe0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xf0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
    };

    /*
     * Host name delimiters ":/?#" and NUL
     */
    static const bool delimiter_table[256] = {
        /*          0  1  2  3   4  5  6  7   8  9  a  b   c  d  e  f */
        /* 0x00 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x10 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x20 */  0, 1, 1, 0,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,
        /* 0x30 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 0, 1,  1, 1, 1, 0,
        /* 0x40 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x60 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x80 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x90 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0xa0 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0xb0 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0xc0 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0xd0 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0xe0 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0xf0 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1
    };

    NS_NONNULL_ASSERT(hostString != NULL);
    NS_NONNULL_ASSERT(portStart != NULL);
    NS_NONNULL_ASSERT(end != NULL);

    /*
     * RFC 3986 defines
     *
     *   host       = IP-literal / IPv4address / reg-name
     *   IP-literal = "[" ( IPv6address / IPvFuture  ) "]"
     */
    if (*hostString == '[') {
        char *p;

        /*
         * This looks like an address in IP-literal notation in square brackets.
         */
        p = strchr(hostString + 1, INTCHAR(']'));
        if (p != NULL) {
            ipLiteral = NS_TRUE;

            /*
             * Zero-byte terminate the IP-literal if hostStart is given.
             */
            if (hostStart != NULL) {
                *p = '\0';
                *hostStart = hostString + 1;
            }
            p++;
            if (*p == ':') {
                *(p++) = '\0';
                *portStart = p;
                *end = SkipDigits(p);
            } else {
                *portStart = NULL;
                *end = p;
            }
            /*fprintf(stderr, "==== IP literal portStart '%s' end '%s'\n", *portStart, *end);*/
        } else {
            /*
             * There is no closing square bracket
             */
            success = NS_FALSE;
            *portStart = NULL;
            if (hostStart != NULL) {
                *hostStart = NULL;
            }
            *end = p;
        }
    }
    if (success && !ipLiteral) {
        char *p;

        /*
         * Still to handle from the RFC 3986 "host" rule:
         *
         *   host        = .... / IPv4address / reg-name
         *
         * Character-wise, IPv4address is a special case of reg-name.
         *
         *   reg-name    = *( unreserved / pct-encoded / sub-delims )
         *   unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
         *   sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
         *               / "*" / "+" / "," / ";" / "="
         *
         * However: errata #4942 of RFC 3986 says:
         *
         *   reg-name    = *( unreserved / pct-encoded / "-" / ".")
         *
         * which is more in sync with reality. In the errata, the two
         * explicitly mentioned characters are not needed, since these are
         * already part of "unreserved". Probably, there are characters in
         * "unreserved", which are not desired either.
         *
         * RFC 3986 sec 3.2: The authority component is preceded by a double
         * slash ("//") and is terminated by the next slash ("/"), question
         * mark ("?"), or number sign ("#") character, or by the end of the
         * URI.
         *
         */

        if (strict) {
            /*
             * Use the table based on regname + errata in RFC 3986.
             */
            for (p = hostString; regname_table[UCHAR(*p)]; p++) {
                ;
            }
        } else {
            /*
             * Just scan for the bare necessity based on delimiters.
             */
            for (p = hostString; delimiter_table[UCHAR(*p)]; p++) {
                ;
            }

        }
        /*
         * The host is not allowed to start with a dot ("dots are separators
         * for labels"), and it has to be at least one character long.
         *
         * Colon is not part of the allowed characters in reg-name, so we can
         * use it to determine the (optional) port.
         *
         */
        success = (*hostString != '.'
                   && (*p == '\0' || *p == ':' || *p == '/' || *p == '?' || *p == '#'));
        if (*p == ':') {
            *(p++) = '\0';
            *portStart = p;
            *end = SkipDigits(p);
        } else {
            *portStart = NULL;
            *end = p;
        }

        /* fprintf(stderr, "==== p %.2x, success %d '%s'\n", *p, success, hostString); */

        if (hostStart != NULL) {
            *hostStart = hostString;
        }
    }

    /*
     * When a port is found, make sure, the port is at least one digit.
     * We could consider making the test only in the non-strict case,
     * but it is hard to believe that zero-byte ports make sense in any
     * scenario.
     */
    if (success && *portStart != NULL) {
        success = (*portStart != *end);
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpLocationString --
 *
 *      Build an HTTP location string following the IP literation
 *      notation as in RFC 3986 section 3.2.2 in the provided
 *      Tcl_DString. In case protoString is non-null, prepend
 *      the protocol. In case port != defPort, append the port.
 *
 * Results:
 *      Location strings such as e.g.
 *          [2001:db8:1f70::999:de8:7648:6e8]:8000 (IP-literal notation)
 *          https://openacs.org                    (reg-name notation)
 *
 * Side effects:
 *      Modifies passed Tcl_DString
 *
 *----------------------------------------------------------------------
 */

char *
Ns_HttpLocationString(
    Tcl_DString *dsPtr,
    const char *protoString,
    const char *hostString,
    unsigned short port,
    unsigned short defPort
) {
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(hostString != NULL);

    if (protoString != NULL) {
        Ns_DStringVarAppend(dsPtr, protoString, "://", (char *)0L);
    }
    if (port == 0 && defPort == 0) {
        /*
         * We assume, that the host contains already a port (as provided from
         * the host header field), and all we have to do is to prepend the
         * protocol prefix.
         */
        Ns_DStringVarAppend(dsPtr, hostString, (char *)0L);
    } else {
        if (strchr(hostString, INTCHAR(':')) != NULL) {
            Ns_DStringVarAppend(dsPtr, "[", hostString, "]", (char *)0L);
        } else {
            Ns_DStringVarAppend(dsPtr, hostString, (char *)0L);
        }
        if (port != defPort) {
            (void) Ns_DStringPrintf(dsPtr, ":%d", port);
        }
    }

    return dsPtr->string;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpMessageParse --
 *
 *      Parse an HTTP message (response line and headers).
 *      The headers are returned into the provided Ns_Set,
 *      while the rest is returned via output args.
 *
 * Results:
 *      Ns_ReturnCode
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_HttpMessageParse(
    char *message,
    size_t size,
    Ns_Set *hdrPtr,
    int *majorPtr,
    int *minorPtr,
    int *statusPtr,
    char **payloadPtr
) {
    Ns_ReturnCode status = NS_OK;
    int           items, major, minor;

    NS_NONNULL_ASSERT(hdrPtr != NULL);
    NS_NONNULL_ASSERT(message != NULL);
    NS_NONNULL_ASSERT(statusPtr != NULL);

    if (majorPtr == NULL) {
        majorPtr = &major;
    }
    if (minorPtr == NULL) {
        minorPtr = &minor;
    }
    /*
     * If provided, set *payloadPtr always to a sensible value.
     * And... risk the memory leak!
     */
    if (payloadPtr != NULL) {
        *payloadPtr = NULL;
    }
    Ns_Log(Ns_LogTaskDebug, "Message Parse <%s>", message);

    items = sscanf(message, "HTTP/%2d.%2d %3d", majorPtr, minorPtr, statusPtr);
    if (items != 3) {
        status = NS_ERROR;
    } else {
        char   *p, *eol;
        int     firsthdr = 1;
        size_t  parsed;

        p = message;
        while ((eol = strchr(p, INTCHAR('\n'))) != NULL) {
            size_t len;

            *eol++ = '\0';
            len = (size_t)((eol-p)-1);

            if (len > 0u && p[len - 1u] == '\r') {
                p[len - 1u] = '\0';
            }
            if (firsthdr != 0) {
                ns_free((void *)hdrPtr->name);
                hdrPtr->name = ns_strdup(p);
                firsthdr = 0;
            } else if (len < 2 || Ns_ParseHeader(hdrPtr, p, NULL, ToLower, NULL) != NS_OK) {
                break;
            }
            p = eol;
        }
        parsed = (size_t)(p - message);

        Ns_Log(Ns_LogRequestDebug, "Ns_ParseHeader <%s> len %" PRIuz " parsed %"
               PRIuz, message, size, parsed);

        if (payloadPtr != NULL && (size - parsed) >= 2u) {
            p += 2;
            *payloadPtr = p;
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHttpObjCmd --
 *
 *      Implements "ns_http".  This caommand is the general interface for
 *      handling HTTP client requests.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Depens on the subcommand.
 *
 *----------------------------------------------------------------------
 */

int
NsTclHttpObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    TCL_OBJC_T objc,
    Tcl_Obj *const* objv
) {
    const Ns_SubCmdSpec subcmds[] = {
        {"cancel",   HttpCancelObjCmd},
        {"cleanup",  HttpCleanupObjCmd},
        {"list",     HttpListObjCmd},
        {"queue",    HttpQueueObjCmd},
        {"run",      HttpRunObjCmd},
        {"stats",    HttpStatsObjCmd},
        {"wait",     HttpWaitObjCmd},
        {NULL,       NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpRunObjCmd
 *
 *      Implements "ns_http run".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpRunObjCmd(
    ClientData clientData,
    Tcl_Interp *UNUSED(interp),
    TCL_OBJC_T objc,
    Tcl_Obj *const* objv
) {
    return HttpQueue(clientData, objc, objv, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpQueueObjCmd
 *
 *      Implements "ns_http queue".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpQueueObjCmd(
    ClientData clientData,
    Tcl_Interp *UNUSED(interp),
    TCL_OBJC_T objc,
    Tcl_Obj *const* objv
) {
    return HttpQueue(clientData, objc, objv, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpWaitObjCmd --
 *
 *      Implements "ns_http wait".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Typically closing request.
 *
 *      The current [ns_http wait] API is broken w.r.t options
 *      being accepted on the command line, as some of the
 *      options may influence the request processing in the
 *      detached task which is running asynchronously in the
 *      task thread.
 *
 *      At the time of [ns_http wait] the task may have been
 *      completed already, so manipulating task options at
 *      this point is meaningless and error-prone.
 *
 *      The "problematic" options include:
 *
 *      -headers
 *          Every dispatched task stores reply headers in the
 *          private ns_set and this set is provided as a part
 *          of the command result. Putting extra headers will
 *          only copy the internal set over, thus adding nothing
 *          more of a value than a waste of time.
 *
 *      -spoolsize
 *          This limits the size of the reply content that is
 *          being stored in memory during the task processing.
 *          However, the task may already handle the body at
 *          the time somebody calls [ns_http wait] so changing
 *          this value may have no real effect (any more).
 *
 *      -outputfile
 *          This, in conjunction with -spoolsize instructs the task
 *          to store response content in a given file. But again, at
 *          the time this command is called, the task may have been
 *          completely done and the content may already sit in a
 *          temporary file (name of which can be obtained by -file).
 *
 *      -decompress
 *          This flag tells the task to automatically decompress
 *          gzip'ed content. At the time of [ns_http wait] the
 *          content may have been received and left compressed
 *          already, so setting this flag may have no effect.
 *
 *       We should eliminate above options from the API at some time.
 *       At the moment they are declared deprecated but the old
 *       implementation is still there. However, be aware that it may
 *       not work as you expect.
 *
 *       At the same time, all of the optional variables that
 *       might receive information about the wait'ed task are
 *       deprecated. The command result returns a Tcl dict with
 *       all of those already calculated, so there is no need
 *       for extra command options any more.
 *
 *----------------------------------------------------------------------
 */

static int
HttpWaitObjCmd(
    ClientData  clientData,
    Tcl_Interp *interp,
    TCL_OBJC_T         objc,
    Tcl_Obj    *const* objv
) {
    NsInterp   *itPtr = clientData;
    NsHttpTask *httpPtr = NULL;

    char       *id = NULL, *outputFileName = NULL;
    int         result = TCL_OK, decompress = 0, binary = 0;
    Tcl_WideInt spoolLimit = -1;
    Tcl_Obj    *elapsedVarObj = NULL,
               *resultVarObj = NULL,
               *statusVarObj = NULL,
               *fileVarObj = NULL;
    Ns_Set     *replyHeaders = NULL;
    Ns_Time    *timeoutPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-binary",     Ns_ObjvBool,    &binary,          INT2PTR(NS_TRUE)},
        {"-decompress", Ns_ObjvBool,    &decompress,      INT2PTR(NS_TRUE)},
        {"-elapsed",    Ns_ObjvObj,     &elapsedVarObj,   NULL},
        {"-file",       Ns_ObjvObj,     &fileVarObj,      NULL},
        {"-headers",    Ns_ObjvSet,     &replyHeaders,    NULL},
        {"-outputfile", Ns_ObjvString,  &outputFileName,  NULL},
        {"-result",     Ns_ObjvObj,     &resultVarObj,    NULL},
        {"-spoolsize",  Ns_ObjvMemUnit, &spoolLimit,      NULL},
        {"-status",     Ns_ObjvObj,     &statusVarObj,    NULL},
        {"-timeout",    Ns_ObjvTime,    &timeoutPtr,      NULL},
        {NULL,          NULL,           NULL,             NULL}
    };
    Ns_ObjvSpec args[] = {
        {"id", Ns_ObjvString, &id,  NULL},
        {NULL, NULL,          NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (HttpGet(itPtr, id, &httpPtr, NS_TRUE) == NS_FALSE) {
        result = TCL_ERROR;

    } else {
        Ns_ReturnCode rc;

        /*
         * All following options are not supposed to be present here.
         * The command API should be cleansed, but for now, lets play
         * backward compatibility...
         */
        if (replyHeaders != NULL) {
            Ns_Log(Warning, "ns_http_wait: -headers option is deprecated");
        }
        if (decompress != 0) {
            Ns_Log(Warning, "ns_http_wait: ignore obsolete flag -decompress");
        }

        if (binary != 0) {
            Ns_Log(Warning, "ns_http_wait: -binary option is deprecated");
            httpPtr->flags |= NS_HTTP_FLAG_BINARY;
        }
        if (spoolLimit > -1) {
            Ns_Log(Warning, "ns_http_wait: -spoolsize option is deprecated");
            httpPtr->spoolLimit = spoolLimit;
        }
        if (outputFileName != NULL) {
            Ns_Log(Warning, "ns_http_wait: -outputfile option is deprecated");
            Ns_MutexLock(&httpPtr->lock);
            if (httpPtr->spoolFileName != NULL) {
                Ns_Log(Warning, "ns_http_wait: the -outputfile was already"
                       " set in the ns_http_queue; ignored!");
            } else {
                httpPtr->spoolFileName = ns_strdup(outputFileName);
            }
            Ns_MutexUnlock(&httpPtr->lock);
        }
        /*
         * Always decompress unless told differently. Here we do not have the
         * "-raw" option, since we do not need backward compatibility.
         */
        httpPtr->flags |= NS_HTTP_FLAG_DECOMPRESS;

        if (elapsedVarObj != NULL) {
            Ns_Log(Warning, "ns_http_wait: -elapsed option is deprecated");
        }
        if (resultVarObj != NULL) {
            Ns_Log(Warning, "ns_http_wait: -result option is deprecated");
        }
        if (statusVarObj != NULL) {
            Ns_Log(Warning, "ns_http_wait: -status option is deprecated");
        }
        if (fileVarObj != NULL) {
            Ns_Log(Warning, "ns_http_wait: -file option is deprecated");
        }
        if (timeoutPtr == NULL) {
            timeoutPtr = httpPtr->timeout;
        }

        rc = Ns_TaskWait(httpPtr->task, timeoutPtr);
        Ns_Log(Ns_LogTaskDebug, "Ns_TaskWait returns %d", rc);

        if (likely(rc == NS_OK)) {
            result = HttpGetResult(interp, httpPtr);
        } else {
            HttpCancel(httpPtr);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(httpPtr->error, TCL_INDEX_NONE));
            if (rc == NS_TIMEOUT) {
                Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
                Ns_Log(Ns_LogTimeoutDebug, "ns_http request '%s' runs into timeout",
                       httpPtr->url);
                HttpClientLogWrite(httpPtr, "tasktimeout");
            }
            result = TCL_ERROR;
        }

        /*
         * This part is deprecated and can be removed
         * once we go up to a next major version
         * where [ns_http wait] will accept no options.
         */
        if (result == TCL_OK) {
            int      ii;
            Tcl_Obj *rObj, *vObj, *oObj[8];

            /*
             * Pick up corresponding dictionary elements
             * and fill-in passed variables.
             */
            oObj[0] = Tcl_NewStringObj("time", 4);
            oObj[1] = elapsedVarObj;

            oObj[2] = Tcl_NewStringObj("body", 4);
            oObj[3] = resultVarObj;

            oObj[4] = Tcl_NewStringObj("status", 6);
            oObj[5] = statusVarObj;

            oObj[6] = Tcl_NewStringObj("file", 4);
            oObj[7] = fileVarObj;

            rObj = Tcl_GetObjResult(interp);

            for (ii = 0; ii < 8; ii += 2) {
                Tcl_DictObjGet(interp, rObj, oObj[ii], &vObj);
                if (oObj[ii+1] != NULL && vObj != NULL) {
                    if (Ns_SetNamedVar(interp, oObj[ii+1], vObj) == NS_FALSE) {
                        result = TCL_ERROR;
                    }
                }
                Tcl_DecrRefCount(oObj[ii]);
            }

            if (replyHeaders != NULL) {
                Ns_Set  *headers;
                Tcl_Obj *kObj;

                /*
                 * Merge reply headers into the user-passed set.
                 */
                kObj = Tcl_NewStringObj("headers", 7);
                Tcl_DictObjGet(interp, rObj, kObj, &vObj);
                Tcl_DecrRefCount(kObj);
                NS_NONNULL_ASSERT(vObj != NULL);
                headers = Ns_TclGetSet(interp, Tcl_GetString(vObj));
                NS_NONNULL_ASSERT(headers != NULL);
                Ns_SetMerge(replyHeaders, headers);
            }
        }
    }

    if (httpPtr != NULL) {
        HttpSpliceChannels(interp, httpPtr);
        HttpClose(httpPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCancelObjCmd --
 *
 *      Implements "ns_http cancel".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Typically aborting and closing request.
 *
 *----------------------------------------------------------------------
 */

static int
HttpCancelObjCmd(
    ClientData  clientData,
    Tcl_Interp *interp,
    TCL_OBJC_T         objc,
    Tcl_Obj    *const* objv
) {
    NsInterp    *itPtr = clientData;
    char        *idString;
    int          result = TCL_OK;

    Ns_ObjvSpec  args[] = {
        {"id", Ns_ObjvString, &idString, NULL},
        {NULL, NULL,          NULL,      NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        NsHttpTask *httpPtr = NULL;

        if (HttpGet(itPtr, idString, &httpPtr, NS_TRUE) == NS_FALSE) {
            result = TCL_ERROR;
        } else {
            HttpCancel(httpPtr);
            HttpSpliceChannels(interp, httpPtr);
            HttpClose(httpPtr);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCleanupObjCmd
 *
 *      Implements "ns_http cleanup".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Cancel all pending requests.
 *      Dirty-close of any task-associated body/output channels.
 *
 *----------------------------------------------------------------------
 */

static int
HttpCleanupObjCmd(
    ClientData  clientData,
    Tcl_Interp *interp,
    TCL_OBJC_T         objc,
    Tcl_Obj    *const* objv
) {
    NsInterp    *itPtr = clientData;
    int          result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_HashSearch search;
        Tcl_HashEntry *hPtr;

        for (hPtr = Tcl_FirstHashEntry(&itPtr->httpRequests, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search)) {
            NsHttpTask *httpPtr;
            char       *taskName;

            httpPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);
            assert(httpPtr != NULL);

            taskName = Tcl_GetHashKey(&itPtr->httpRequests, hPtr);

            Ns_Log(Warning, "HttpCleanup: cancel task:%s", taskName);

            HttpCancel(httpPtr);

            /*
             * Normally, channels should be re-integrated
             * into the running interp and [close]'d from
             * there. But our current cleanup semantics
             * does not allow that, so we simply and dirty
             * close the channels here. At this point they
             * should be not part of any thread (must have
             * been Tcl_Cut'ed) nor interp (must have been
             * Tcl_Unregister'ed). Failure to do so may
             * wreak havoc with our memory.
             * As with the current design, the channel must
             * have a refcount of 1 at this place, since we
             * reserved it in the HttpCutChannel() call.
             * Now we must do the reverse here, but do the
             * unregister with NULL interp just to reduce
             * the refcount. This should also implicitly
             * close the channel. If not, there is a leak.
             */
            if (httpPtr->bodyChan != NULL) {
                Tcl_SpliceChannel(httpPtr->bodyChan);
                Tcl_UnregisterChannel((Tcl_Interp *)NULL, httpPtr->bodyChan);
                httpPtr->bodyChan = NULL;
            }
            if (httpPtr->spoolChan != NULL) {
                Tcl_SpliceChannel(httpPtr->spoolChan);
                Tcl_UnregisterChannel((Tcl_Interp *)NULL, httpPtr->spoolChan);
                httpPtr->spoolChan = NULL;
            }

            HttpClose(httpPtr);
            Tcl_DeleteHashEntry(hPtr);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpListObjCmd
 *
 *      Implements "ns_http list".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpListObjCmd(
    ClientData  clientData,
    Tcl_Interp *interp,
    TCL_OBJC_T         objc,
    Tcl_Obj    *const* objv
) {
    NsInterp      *itPtr = clientData;
    char          *idString = NULL;
    int            result = TCL_OK;
    Tcl_Obj       *resultObj;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    /*
     * Syntax: ns_http list ?id?
     */
    if (objc == 3) {
        idString = Tcl_GetString(objv[2]);
    }

    resultObj = Tcl_NewListObj(0, NULL);

    for (hPtr = Tcl_FirstHashEntry(&itPtr->httpRequests, &search);
         hPtr != NULL;
         hPtr = Tcl_NextHashEntry(&search) ) {
        char *taskString = Tcl_GetHashKey(&itPtr->httpRequests, hPtr);

        if (idString == NULL || STREQ(taskString, idString)) {
            const char *taskState;
            NsHttpTask *httpPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);

            assert(httpPtr != NULL);

            if (Ns_TaskCompleted(httpPtr->task) == NS_TRUE) {
                taskState = "done";
            } else if (httpPtr->error != NULL) {
                taskState = "error";
            } else {
                taskState = "running";
            }

            Tcl_ListObjAppendElement
                (interp, resultObj, Tcl_NewStringObj(taskString, TCL_INDEX_NONE));

            Tcl_ListObjAppendElement
                (interp, resultObj, Tcl_NewStringObj(httpPtr->url, TCL_INDEX_NONE));

            Tcl_ListObjAppendElement
                (interp, resultObj, Tcl_NewStringObj(taskState, TCL_INDEX_NONE));
        }
    }

    Tcl_SetObjResult(interp, resultObj);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpStatsObjCmd
 *
 *      Implements "ns_http stats".
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpStatsObjCmd(
    ClientData  clientData,
    Tcl_Interp *interp,
    TCL_OBJC_T         objc,
    Tcl_Obj    *const* objv
) {
    NsInterp      *itPtr = clientData;
    char          *idString = NULL;
    int            result = TCL_OK;
    Tcl_Obj       *resultObj = NULL;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    /*
     * Syntax: ns_http stats ?id?
     */
    if (objc == 3) {
        idString = Tcl_GetString(objv[2]);
    }

    if (idString == NULL) {
        resultObj = Tcl_NewListObj(0, NULL);
    }

    for (hPtr = Tcl_FirstHashEntry(&itPtr->httpRequests, &search);
         hPtr != NULL;
         hPtr = Tcl_NextHashEntry(&search)) {

        const char *taskString;

        taskString = Tcl_GetHashKey(&itPtr->httpRequests, hPtr);

        if (idString == NULL || STREQ(taskString, idString)) {
            NsHttpTask *httpPtr;
            Tcl_Obj    *entryObj;

            httpPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);
            NS_NONNULL_ASSERT(httpPtr != NULL);

            entryObj = Tcl_NewDictObj();

            /*
             * Following are not being changed by the task thread
             * so we need no extra lock here.
             */

            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("task", 4),
                 Tcl_NewStringObj(taskString, TCL_INDEX_NONE));

            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("url", 3),
                 Tcl_NewStringObj(httpPtr->url, TCL_INDEX_NONE));

            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("requestlength", 13),
                 Tcl_NewWideIntObj((Tcl_WideInt)httpPtr->requestLength));

            /*
             * Following may be subject to change by the task thread
             * so we sync-up on the mutex.
             */

            Ns_MutexLock(&httpPtr->lock);

            /*
             * This element is a misnomer, but we leave it for the
             * sake of backwards compatibility. Actually, this is
             * the value of the returned Content-Length header.
             */
            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("replylength", 11),
                 Tcl_NewWideIntObj((Tcl_WideInt)httpPtr->replyLength));

            /*
             * Counter of bytes of the request sent so far.
             * It includes all of the request (status line, headers, body).
             */
            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("sent", 4),
                 Tcl_NewWideIntObj((Tcl_WideInt)httpPtr->sent));

            /*
             * Counter of bytes of the reply received so far.
             * It includes all of the reply (status line, headers, body).
             */
            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("received", 8),
                 Tcl_NewWideIntObj((Tcl_WideInt)httpPtr->received));

            /*
             * Counter of the request body sent so far.
             */
            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("sendbodysize", 12),
                 Tcl_NewWideIntObj((Tcl_WideInt)httpPtr->sendBodySize));

            /*
             * Counter of processed (potentially deflated)
             * reply body received so far.
             */
            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("replybodysize", 13),
                 Tcl_NewWideIntObj((Tcl_WideInt)httpPtr->replyBodySize));

            /*
             * Counter of the non-processed (potentially compressed)
             * reply body received so far.
             * For compressed but not deflated reply content
             * the replybodysize and replysize will be equal.
             */
            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("replysize", 9),
                 Tcl_NewWideIntObj((Tcl_WideInt)httpPtr->replySize));

            Ns_MutexUnlock(&httpPtr->lock);

            if (resultObj == NULL) {
                Tcl_SetObjResult(interp, entryObj);
            } else {
                (void) Tcl_ListObjAppendElement(interp, resultObj, entryObj);
            }
        }
    }

    if (resultObj != NULL) {
        Tcl_SetObjResult(interp, resultObj);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * InitializeTaskQueue --
 *
 *      Make sure that we have a task queue defined.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May queue an HTTP request.
 *
 *----------------------------------------------------------------------
 */

static bool InitializeTaskQueue(void) {

    //fprintf(stderr, "============== TaskQueueRequire %p ==============\n", (void*)taskQueue);
    taskQueue = Ns_CreateTaskQueue("tclhttp");

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpQueue --
 *
 *      Enqueues the HTTP task and optionally returns the taskID
 *      in the interp result. This taskID can be used by other
 *      commands to cancel or wait for the task to finish.
 *
 *      The taskID is not returned if the "-doneCallback" option
 *      is specified. In that case, the task is handled and
 *      garbage collected by the thread executing the task.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May queue an HTTP request.
 *
 *----------------------------------------------------------------------
 */

static int
HttpQueue(
    NsInterp *itPtr,
    TCL_OBJC_T objc,
    Tcl_Obj *const* objv,
    bool run
) {
    Tcl_Interp *interp;
    int         result = TCL_OK, decompress = 0, raw = 0, binary = 0, partialResults = 0;
    Tcl_WideInt spoolLimit = -1;
    int         verifyCert = 0, keepHostHdr = 0;
    NsHttpTask *httpPtr = NULL;
    char       *cert = NULL,
               *caFile = NULL,
               *caPath = NULL,
               *sniHostname = NULL,
               *outputFileName = NULL,
               *outputChanName = NULL,
               *method = (char *)"GET",
               *url = NULL,
               *doneCallback = NULL,
               *bodyChanName = NULL,
               *bodyFileName = NULL;
    Ns_Set     *requestHdrPtr = NULL;
    Tcl_Obj    *bodyObj = NULL, *proxyObj = NULL;
    Ns_Time    *timeoutPtr = NULL, *expirePtr = NULL, *keepAliveTimeoutPtr = NULL;
    Tcl_WideInt bodySize = 0;
    Tcl_Channel bodyChan = NULL, spoolChan = NULL;
    Ns_ObjvValueRange sizeRange = {0, LLONG_MAX};

    Ns_ObjvSpec opts[] = {
        {"-binary",           Ns_ObjvBool,    &binary,         INT2PTR(NS_TRUE)},
        {"-body",             Ns_ObjvObj,     &bodyObj,        NULL},
        {"-body_chan",        Ns_ObjvString,  &bodyChanName,   NULL},
        {"-body_file",        Ns_ObjvString,  &bodyFileName,   NULL},
        {"-body_size",        Ns_ObjvWideInt, &bodySize,       &sizeRange},
        {"-cafile",           Ns_ObjvString,  &caFile,         NULL},
        {"-capath",           Ns_ObjvString,  &caPath,         NULL},
        {"-cert",             Ns_ObjvString,  &cert,           NULL},
        {"-raw",              Ns_ObjvBool,    &raw,            INT2PTR(NS_TRUE)},
        {"-decompress",       Ns_ObjvBool,    &decompress,     INT2PTR(NS_TRUE)},
        {"-donecallback",     Ns_ObjvString,  &doneCallback,   NULL},
        {"-headers",          Ns_ObjvSet,     &requestHdrPtr,  NULL},
        {"-hostname",         Ns_ObjvString,  &sniHostname,    NULL},
        {"-keepalive",        Ns_ObjvTime,    &keepAliveTimeoutPtr, NULL},
        {"-keep_host_header", Ns_ObjvBool,    &keepHostHdr,    INT2PTR(NS_TRUE)},
        {"-method",           Ns_ObjvString,  &method,         NULL},
        {"-outputchan",       Ns_ObjvString,  &outputChanName, NULL},
        {"-outputfile",       Ns_ObjvString,  &outputFileName, NULL},
        {"-partialresults",   Ns_ObjvBool,    &partialResults, INT2PTR(NS_TRUE)},
        {"-spoolsize",        Ns_ObjvMemUnit, &spoolLimit,     NULL},
        {"-expire",           Ns_ObjvTime,    &expirePtr,      NULL},
        {"-timeout",          Ns_ObjvTime,    &timeoutPtr,     NULL},
        {"-verify",           Ns_ObjvBool,    &verifyCert,     INT2PTR(NS_TRUE)},
        {"-proxy",            Ns_ObjvObj,     &proxyObj,       NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",       Ns_ObjvString, &url, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);
    interp = itPtr->interp;

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (run == NS_TRUE && doneCallback != NULL) {
        Ns_TclPrintfResult(interp, "option -doneCallback allowed only"
                           " for [ns_http_queue]");
        result = TCL_ERROR;
    } else if (outputFileName != NULL && outputChanName != NULL) {
        Ns_TclPrintfResult(interp, "only one of -outputchan or -outputfile"
                           " options are allowed");
        result = TCL_ERROR;
    } else if (((bodyFileName != NULL) + (bodyChanName != NULL) + (bodyObj != NULL)) > 1) {
        Ns_TclPrintfResult(interp, "only one of -body, -body_chan or -body_file"
                           " options are allowed");
        result = TCL_ERROR;
    } else if (unlikely(decompress != 0)) {
        Ns_Log(Warning, "ignore obsolete flag -decompress");
    } else if (raw != 1) {
        decompress = 1;
    }

    if (result == TCL_OK && bodyFileName != NULL) {
        struct stat bodyStat;

        if (Ns_Stat(bodyFileName, &bodyStat) == NS_TRUE) {
            if (bodySize == 0) {
                bodySize = (Tcl_WideInt)bodyStat.st_size;
            }
        } else {
            Ns_TclPrintfResult(interp, "cannot stat: %s ", bodyFileName);
            result = TCL_ERROR;
        }
    }

    if (result == TCL_OK && bodyChanName != NULL) {
        if (Ns_TclGetOpenChannel(interp, bodyChanName, /* write */ 0,
                                 /* check */ 1, &bodyChan) != TCL_OK) {
            result = TCL_ERROR;
        } else if (bodySize == 0) {
            bodySize = Tcl_Seek(bodyChan, 0, SEEK_END);
            if (bodySize == -1) {
                Ns_TclPrintfResult(interp, "can't seek channel: %s",
                                   Tcl_ErrnoMsg(Tcl_GetErrno()));
                result = TCL_ERROR;
            }
        }
    }

    if (result == TCL_OK && outputChanName != NULL) {
        if (Ns_TclGetOpenChannel(interp, outputChanName, /* write */ 1,
                                 /* check */ 1, &spoolChan) != TCL_OK) {
            result = TCL_ERROR;
        }
    }

    if (result == TCL_OK) {
        Ns_Log(Ns_LogTaskDebug, "HttpQueue calls HttpConnect with timeout:%p", (void*)timeoutPtr);

        result = HttpConnect(itPtr,
                             method,
                             url,
                             proxyObj,
                             requestHdrPtr,
                             bodySize,
                             bodyObj,
                             bodyFileName,
                             cert,
                             caFile,
                             caPath,
                             sniHostname,
                             (verifyCert  == 1),
                             (keepHostHdr == 1),
                             timeoutPtr,
                             expirePtr,
                             keepAliveTimeoutPtr,
                             &httpPtr);
    }

    if (result == TCL_OK && bodyChan != NULL) {
        if (HttpCutChannel(interp, bodyChan) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            httpPtr->bodyChan = bodyChan;
        }
    }

    if (result == TCL_OK && spoolChan != NULL) {
        if (HttpCutChannel(interp, spoolChan) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            httpPtr->spoolChan = spoolChan;
        }
    }

    if (result != TCL_OK) {
        if (httpPtr != NULL) {
            HttpSpliceChannels(interp, httpPtr);
            HttpClose(httpPtr);
        }
    } else {

        /*
         * All is fine. Fill in the rest of the task options
         */
        if (spoolLimit > -1) {
            httpPtr->spoolLimit = spoolLimit;
        }
        if (outputFileName != NULL) {
            httpPtr->spoolFileName =  ns_strdup(outputFileName);
        }
        if (doneCallback != NULL) {
            httpPtr->doneCallback = ns_strdup(doneCallback);
        }
        if (likely(decompress != 0) && likely(raw == 0)) {
            httpPtr->flags |= NS_HTTP_FLAG_DECOMPRESS;
        } else {
            httpPtr->flags = (httpPtr->flags & ~NS_HTTP_FLAG_DECOMPRESS);
        }
        if (binary != 0) {
            httpPtr->flags |= NS_HTTP_FLAG_BINARY;
        }
        if (partialResults != 0) {
            httpPtr->flags |= NS_HTTP_PARTIAL_RESULTS;
        }

        httpPtr->servPtr = itPtr->servPtr;

        httpPtr->task = Ns_TaskTimedCreate(httpPtr->sock, HttpProc, httpPtr, expirePtr);

        if (run == NS_TRUE) {

            /*
             * Run the task and collect the result in one go.
             * The task is executed in the current thread.
             */
            Ns_TaskRun(httpPtr->task);
            result = HttpGetResult(interp, httpPtr);
            HttpSpliceChannels(interp, httpPtr);
            HttpClose(httpPtr);

        } else {

            /*
             * Enqueue the task, optionally returning the taskID
             */
            assert(taskQueue != NULL);

            if (Ns_TaskEnqueue(httpPtr->task, taskQueue) != NS_OK) {
                HttpSpliceChannels(interp, httpPtr);
                HttpClose(httpPtr);
                Ns_TclPrintfResult(interp, "could not queue HTTP task");
                result = TCL_ERROR;

            } else if (doneCallback != NULL) {

                /*
                 * There is nothing to wait on when the doneCallback
                 * was declared, since the callback garbage-collects
                 * the task. Hence we do not create the taskID.
                 */
                Ns_Log(Ns_LogTaskDebug, "HttpQueue: no taskID returned");

            } else {
                Tcl_HashEntry *hPtr = NULL;
                uint32_t       ii;
                TCL_SIZE_T     len;
                char           buf[TCL_INTEGER_SPACE + 4];

                /*
                 * Create taskID to be used for [ns_http_wait] et al.
                 */
                memcpy(buf, "http", 4u);
                for (ii = (uint32_t)itPtr->httpRequests.numEntries; ; ii++) {
                    int new = 0;

                    len = (TCL_SIZE_T)ns_uint32toa(&buf[4], ii);
                    hPtr = Tcl_CreateHashEntry(&itPtr->httpRequests, buf, &new);
                    if (new != 0) {
                        break;
                    }
                }
                assert(hPtr != NULL);
                Tcl_SetHashValue(hPtr, (ClientData)httpPtr);
                Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, len+4));
            }
        }
    }

    return result;
}

static void
HttpClientLogWrite(
    const NsHttpTask *httpPtr,
    const char       *causeString
) {
    Ns_Time   diff;
    NsServer *servPtr;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(causeString != NULL);

    /*fprintf(stderr, "================ HttpClientLog %d fd %d %s etime %ld cause %s\n",
            httpPtr->servPtr->httpclient.logging,
            httpPtr->servPtr->httpclient.fd,
            httpPtr->servPtr->httpclient.logFileName,
            (long)(httpPtr->etime.sec),
            causeString
            );*/

    Ns_DiffTime(&httpPtr->etime, &httpPtr->stime, &diff);

    if (likely(httpPtr->servPtr != NULL)) {
        servPtr = httpPtr->servPtr;
    } else {
        /*
         * In case, there is no server provided in httpPtr (e.g. the itPtr had
         * no servPtr set), use the configuration of the default server.
         */
        servPtr = NsGetServer(nsconf.defaultServer);
        if (servPtr == NULL) {
            Ns_Log(Error, "http client log: server could not be determined, logging attempt rejected");
            return;
        }
    }

    if (servPtr->httpclient.logging
        && servPtr->httpclient.fd != NS_INVALID_FD
       ) {
        Tcl_DString logString;
        char buf[41]; /* Big enough for Ns_LogTime(). */

        Tcl_DStringInit(&logString);
        Ns_DStringPrintf(&logString, "%s %s %d %s %s " NS_TIME_FMT
                         " %" PRIdz " %" PRIdz " %s\n",
                         Ns_LogTime(buf),
                         Ns_ThreadGetName(),
                         httpPtr->status == 0 ? 408 : httpPtr->status,
                         httpPtr->method,
                         httpPtr->url,
                         (int64_t)diff.sec, diff.usec,
                         httpPtr->sent,
                         httpPtr->received,
                         causeString
                        );

        Ns_MutexLock(&servPtr->httpclient.lock);
        (void)NsAsyncWrite(servPtr->httpclient.fd,
                           logString.string, (size_t)logString.length);
        Ns_MutexUnlock(&servPtr->httpclient.lock);

        Tcl_DStringFree(&logString);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * HttpGetResult --
 *
 *      Get the result of the Task and set it in the interp result.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpGetResult(
    Tcl_Interp *interp,
    NsHttpTask *httpPtr
) {
    int      result = TCL_OK;
    Ns_Time  diff;
    Tcl_Obj *statusObj       = NULL,
            *replyBodyObj    = NULL,
            *fileNameObj     = NULL,
            *resultObj       = NULL,
            *replyHeadersObj = NULL,
            *errorObj        = NULL,
            *elapsedTimeObj;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(httpPtr != NULL);

    /*
     * In some error conditions, the endtime is not set. make sure, take the
     * current time in these cases.
     */
    if (httpPtr->etime.sec == 0) {
        Ns_GetTime(&httpPtr->etime);
    }

    Ns_DiffTime(&httpPtr->etime, &httpPtr->stime, &diff);
    elapsedTimeObj = Tcl_NewObj();
    Ns_TclSetTimeObj(elapsedTimeObj, &diff);

    if (httpPtr->error != NULL) {
        errorObj = Tcl_NewStringObj(httpPtr->error, TCL_INDEX_NONE);
        /*
         * Write always httpCLientLog file with "socktimeout" and system log entry.
         */
        if (httpPtr->finalSockState == NS_SOCK_TIMEOUT) {
            Ns_Log(Ns_LogTimeoutDebug, "ns_http request '%s' runs into timeout",
                   httpPtr->url);
            HttpClientLogWrite(httpPtr, "socktimeout");
        } else {
            HttpClientLogWrite(httpPtr, "error");
        }

        if ((httpPtr->flags & NS_HTTP_PARTIAL_RESULTS) == 0u) {
            Tcl_SetObjResult(interp, errorObj);
            if (httpPtr->finalSockState == NS_SOCK_TIMEOUT) {
                Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
            }
            result = TCL_ERROR;
            goto err;
        }
    } else {
        HttpClientLogWrite(httpPtr, "ok");
    }

    if (httpPtr->recvSpoolMode == NS_FALSE) {
#if defined(TCLHTTP_USE_EXTERNALTOUTF)
        Tcl_Encoding encoding = NULL;
#endif
        bool       binary = NS_FALSE;
        TCL_SIZE_T cSize;
        char      *cData;

        /*
         * Determine type (binary/text) of the received data
         * and decide what kind of object we should create
         * to return the content to the Tcl.
         * We have a choice between binary and string objects.
         * Unfortunately, this is mostly whole lotta guess-work...
         */
        if (unlikely((httpPtr->flags & NS_HTTP_FLAG_GZIP_ENCODING) != 0u)) {
            if (unlikely((httpPtr->flags & NS_HTTP_FLAG_DECOMPRESS) == 0u)) {

                /*
                 * Gzipped but not inflated content
                 * is automatically of a binary-type.
                 * This is pretty straight-forward.
                 */
                binary = NS_TRUE;
            }
        }
        if ((httpPtr->flags & NS_HTTP_FLAG_BINARY) != 0u) {
            binary = NS_TRUE;
        }
        if (binary == NS_FALSE) {
            const char *cType;

            cType = Ns_SetIGet(httpPtr->replyHeaders, contentTypeHeader);
            if (cType != NULL) {

                /*
                 * "binary" actually means: just to take the data as it is,
                 * i.e. to perform no charset conversion.
                 */
                binary = Ns_IsBinaryMimeType(cType);
                /*
                 * When the MIME type does not indicate binary treatment, a
                 * charset encoding is required. (e.g. "text/plain;
                 * charset=iso-8859-2")
                 */
#if defined(TCLHTTP_USE_EXTERNALTOUTF)
                if (binary == NS_FALSE) {
                    encoding = Ns_GetTypeEncoding(cType);
                    if (encoding == NULL) {
                        encoding = NS_utf8Encoding;
                    }
                }
#endif
            }
        }

        cData = httpPtr->ds.string + httpPtr->replyHeaderSize;
        cSize = (TCL_SIZE_T)httpPtr->replyBodySize;

        if (binary == NS_TRUE)  {
            //NsHexPrint("replybodyobj", (unsigned char *)cData, (size_t)cSize, 20, NS_TRUE);
            replyBodyObj = Tcl_NewByteArrayObj((unsigned char *)cData, cSize);
        } else {
#if defined(TCLHTTP_USE_EXTERNALTOUTF)
            Tcl_DString ds;
            Tcl_DStringInit(&ds);
            (void)Tcl_ExternalToUtfDString(encoding, cData, cSize, &ds);
            replyBodyObj = Tcl_NewStringObj(Tcl_DStringValue(&ds), TCL_INDEX_NONE);
            Tcl_DStringFree(&ds);
#else
            replyBodyObj = Tcl_NewStringObj(cData, cSize);
#endif
        }
    }

    statusObj = Tcl_NewIntObj(httpPtr->status);

    if (httpPtr->spoolFd != NS_INVALID_FD) {
        fileNameObj = Tcl_NewStringObj(httpPtr->spoolFileName, TCL_INDEX_NONE);
    }

    /*
     * Check, if "connection: keep-alive" was provided in the reply.
     */
    {
        const char *field;

        /*
         * Set the default value of KEEPALIVE handling depending on HTTP
         * version.  For HTTP/1.1 the default is KEEPALIVE, unless there is an
         * explicit "connection: close" provided from the server.
         */
        if ((httpPtr->flags & NS_HTTP_VERSION_1_1) != 0u) {
            httpPtr->flags |= NS_HTTP_KEEPALIVE;
        } else {
            httpPtr->flags &= ~NS_HTTP_KEEPALIVE;
        }

        field = Ns_SetIGet(httpPtr->replyHeaders, connectionHeader);
        if (field != NULL) {
            if (strncasecmp(field, "close", 5) == 0) {
                httpPtr->flags &= ~NS_HTTP_KEEPALIVE;
            }
        }
        /*
         * Sanity check: When the keep-alive flag is still set, we should have
         * also a keep-alive timeout value present. This timeout value
         * controls the initialization logic during connection setup. By using
         * this sanity check, we do not rely only on the response of the
         * server with its exact field contents.
         */
        if ((httpPtr->flags & NS_HTTP_KEEPALIVE) != 0u
            && httpPtr->keepAliveTimeout.sec == 0
            && httpPtr->keepAliveTimeout.usec == 0
           ) {
            httpPtr->flags &= ~NS_HTTP_KEEPALIVE;
            Ns_Log(Ns_LogTaskDebug, "HttpGetResult: sanity check deactivates keep-alive");
        }
        Ns_Log(Ns_LogTaskDebug, "HttpGetResult: connection: %s",
               (httpPtr->flags & NS_HTTP_KEEPALIVE) != 0u ? "keep-alive" : "close");
    }
    /* Ns_Log(Notice, "replyHeaders");
       Ns_SetPrint(httpPtr->replyHeaders); */

    /*
     * Add reply headers set into the interp
     */
    result = Ns_TclEnterSet(interp, httpPtr->replyHeaders, NS_TCL_SET_DYNAMIC);
    if (result != TCL_OK) {
        goto err;
    }

    httpPtr->replyHeaders = NULL; /* Prevents Ns_SetFree() in HttpClose() */
    replyHeadersObj = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(replyHeadersObj);


    /*
     * Assemble the resulting dictionary
     */
    resultObj = Tcl_NewDictObj();

    Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("status", 6),
                   statusObj);

    Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("time", 4),
                   elapsedTimeObj);

    Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("headers", 7),
                   replyHeadersObj);

    if (fileNameObj != NULL) {
        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("file", 4),
                       fileNameObj);
    }
    if (replyBodyObj != NULL) {
        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("body", 4),
                       replyBodyObj);
    }
    if (errorObj != NULL) {
        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("error", 5),
                       errorObj);
    }
    if (httpPtr->infoObj != NULL) {
        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("https", 5),
                       httpPtr->infoObj);
    }
    if (httpPtr->bodyChan != NULL) {
        const char *chanName = Tcl_GetChannelName(httpPtr->bodyChan);

        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("body_chan", 9),
                       Tcl_NewStringObj(chanName, TCL_INDEX_NONE));
    }
    if (httpPtr->spoolChan != NULL) {
        const char *chanName = Tcl_GetChannelName(httpPtr->spoolChan);

        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("outputchan", 10),
                       Tcl_NewStringObj(chanName, TCL_INDEX_NONE));
    }

    Tcl_SetObjResult(interp, resultObj);

    Tcl_DecrRefCount(replyHeadersObj);

 err:
    if (result != TCL_OK) {
        if (statusObj != NULL) {
            Tcl_DecrRefCount(statusObj);
        }
        if (fileNameObj != NULL) {
            Tcl_DecrRefCount(fileNameObj);
        }
        if (elapsedTimeObj != NULL) {
            Tcl_DecrRefCount(elapsedTimeObj);
        }
        if (replyBodyObj != NULL) {
            Tcl_DecrRefCount(replyBodyObj);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCheckHeader --
 *
 *      Check whether we have full HTTP response including headers.  If yes,
 *      record the total size of the response (including the lone CR/LF
 *      delimiter) in the NsHttpTask structure, as to avoid subsequent
 *      checking.  Terminate the response string by eliminating the lone CR/LF
 *      delimiter (put a NULL byte at the CR place).  This way it is easy to
 *      calculate size of the optional body content following the response
 *      line/headers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Handles the case where server responds with invalid
 *      lone LF delimiters.
 *
 *----------------------------------------------------------------------
 */

static void
HttpCheckHeader(
    NsHttpTask *httpPtr
) {
    char *eoh;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    eoh = strstr(httpPtr->ds.string, "\r\n\r\n");
    if (eoh != NULL) {
        httpPtr->replyHeaderSize = (TCL_SIZE_T)(eoh - httpPtr->ds.string) + 4;
        *(eoh + 2) = '\0';
        httpPtr->flags &= ~NS_HTTP_HEADERS_PENDING;
        Ns_Log(Ns_LogTaskDebug, "HttpCheckHeader: headers complete");
    } else {
        eoh = strstr(httpPtr->ds.string, "\n\n");
        if (eoh != NULL) {
            Ns_Log(Warning, "HttpCheckHeader: client reply contains"
                   " LF instead of CR/LF trailer which should not happen");
            httpPtr->replyHeaderSize = (TCL_SIZE_T)(eoh - httpPtr->ds.string) + 2;
            *(eoh + 1) = '\0';
            httpPtr->flags &= ~NS_HTTP_HEADERS_PENDING;
        } else {
            Ns_Log(Ns_LogTaskDebug, "HttpCheckHeader: headers not complete");
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCheckSpool --
 *
 *      Determine, whether the received data should be left in the
 *      memory or whether it should be spooled to a file or channel,
 *      depending on the size of the returned content and the
 *      configuration settings. The function might return TCL_CONTINUE
 *      to signal, that the buffer has to be processed again.
 *
 * Results:
 *      Tcl Return Code
 *
 * Side effects:
 *      Handles the partial response content located in memory.
 *
 *----------------------------------------------------------------------
 */

static int
HttpCheckSpool(
    NsHttpTask *httpPtr
) {
    int result = TCL_OK;
    int major = 0, minor = 0;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "HttpCheckSpool");
    /*
     * At this point, we already identified the end of the
     * response/headers but haven not yet parsed it because
     * we still do not know the value of the response status.
     *
     * The Tcl_DString in httpPtr->ds contains, at this point:
     *
     *     1. HTTP response line (delimited by CR/LF)
     *     2. Response header(s) (each delimited by CR/LF)
     *     3. Terminating zero byte (was \r; see HttpCheckHeader())
     *     4. Lone \n character (see HttpCheckHeader())
     *     5. Content (or part of it) up to the end of the DString
     *
     * The size of 1.-4. is stored in httpPtr->replyHeaderSize.
     * The 3. delimits the partial content from the response
     * status lines/headers. Note that we parse the size of
     * the response line/headers by explicitly taking the
     * length of the DString value (size of 1.-3.) and not
     * using the DString length element.
     */
    if (Ns_HttpMessageParse(httpPtr->ds.string, strlen(httpPtr->ds.string),
                            httpPtr->replyHeaders,
                            &major,
                            &minor,
                            &httpPtr->status,
                            NULL) != NS_OK || httpPtr->status == 0) {

        Ns_Log(Warning, "ns_http: parsing reply failed");
        result = TCL_ERROR;

    } else {
        const char *header;
        Tcl_WideInt replyLength = 0;

        /*
         * We have received the message header and parsed the first
         * line. Therefore, we know the HTTP status code and the version
         * numbers.
         */
        if (minor == 1 && major == 1) {
            httpPtr->flags |= NS_HTTP_VERSION_1_1;
        }

        if (httpPtr->status / 100 == 1) {
            /*
             * Handling of all informational messages, such as "100
             * continue". We skip here the message without further
             * processing.
             */
            SkipMessage(httpPtr);
            return TCL_CONTINUE;

        } else if (httpPtr->status == 204) {
            /*
             * In case the requests returns 204 (no content), no body is
             * expected.
             */
            httpPtr->flags |= NS_HTTP_FLAG_EMPTY;
        }

        /*
         * Check the returned Content-Length
         */
        header = Ns_SetIGet(httpPtr->replyHeaders, contentLengthHeader);
        if (header != NULL) {
            (void)Ns_StrToWideInt(header, &replyLength);

            /*
             * Don't get fooled by some invalid value!
             */
            if (replyLength < 0) {
                replyLength = 0;
            }

            Ns_Log(Ns_LogTaskDebug, "HttpCheckSpool: %s: %" TCL_LL_MODIFIER "d",
                   contentLengthHeader, replyLength);
        } else {

            /*
             * If none, see if we have Transfer-Encoding.
             * For now, we support "chunked" encoding only.
             */
            header = Ns_SetIGet(httpPtr->replyHeaders, transferEncodingHeader);
            if (header != NULL && Ns_Match(header, "chunked") != NULL) {
                httpPtr->flags |= NS_HTTP_FLAG_CHUNKED;
                httpPtr->chunk->parsers = ChunkParsers;
                Ns_Log(Ns_LogTaskDebug, "HttpCheckSpool: %s: %s",
                       transferEncodingHeader, header);
                /*
                 * Why is this header field deleted here?
                 */
                Ns_SetIDeleteKey(httpPtr->replyHeaders, transferEncodingHeader);
            } else if (httpPtr->status != 204) {
                /*
                 * No content-length provided and not chunked, assume
                 * streaming HTML.
                 */
                Ns_Log(Ns_LogTaskDebug, "ns_http: assume streaming HTML, status %d", httpPtr->status);
                httpPtr->flags |= NS_HTTP_STREAMING;
            }
        }

        /*
         * See if we are handling compressed content.
         * Turn-on auto-decompress if requested.
         */
        header = Ns_SetIGet(httpPtr->replyHeaders, contentEncodingHeader);
        if (header != NULL && Ns_Match(header, "gzip") != NULL) {
            httpPtr->flags |= NS_HTTP_FLAG_GZIP_ENCODING;
            if ((httpPtr->flags & NS_HTTP_FLAG_DECOMPRESS) != 0u) {
                httpPtr->compress = ns_calloc(1u, sizeof(Ns_CompressStream));
                (void) Ns_InflateInit(httpPtr->compress);
                Ns_Log(Ns_LogTaskDebug, "HttpCheckSpool: %s: %s",
                       contentEncodingHeader, header);
            }
        }

        Ns_MutexLock(&httpPtr->lock);
        httpPtr->replyLength = (size_t)replyLength;
        Ns_MutexUnlock(&httpPtr->lock);

        /*
         * See if we need to spool the response content
         * to file/channel or leave it in the memory.
         */
        if (httpPtr->spoolLimit > -1
            && (replyLength == 0 || replyLength >= httpPtr->spoolLimit)) {

            if (httpPtr->spoolChan != NULL) {
                httpPtr->spoolFd = NS_INVALID_FD;
                httpPtr->recvSpoolMode = NS_TRUE;
            } else {
                int fd;

                if (httpPtr->spoolFileName != NULL) {
                    int flags;

                    flags = O_WRONLY|O_CREAT|O_CLOEXEC;
                    fd = ns_open(httpPtr->spoolFileName, flags, 0644);
                } else {
                    const char *tmpDir, *tmpFile = "http.XXXXXX";
                    size_t      tmpLen;

                    tmpDir = nsconf.tmpDir;
                    tmpLen = strlen(tmpDir) + 13;

                    /*
                     * This lock is necessary for [ns_http wait]
                     * backward compatibility. It can be removed
                     * once we modify [ns_http wait] to disable
                     * options processing.
                     */
                    Ns_MutexLock(&httpPtr->lock);
                    httpPtr->spoolFileName = ns_malloc(tmpLen);
                    snprintf(httpPtr->spoolFileName, tmpLen, "%s/%s", tmpDir, tmpFile);
                    Ns_MutexUnlock(&httpPtr->lock);

                    fd = ns_mkstemp(httpPtr->spoolFileName);
                }
                if (fd != NS_INVALID_FD) {
                    httpPtr->spoolFd = fd;
                    httpPtr->recvSpoolMode = NS_TRUE;
                } else {

                    /*
                     * FIXME:
                     *
                     * The ns_mkstemp/ns_open on Unix are clear but
                     * what happens with handling error on Windows?
                     */
                    Ns_Log(Error, "ns_http: can't open spool file: %s:",
                           httpPtr->spoolFileName);
                    result = TCL_ERROR;
                }
            }
        }
    }

    if (result == TCL_OK) {
        size_t cSize;

        cSize = (size_t)(httpPtr->ds.length - httpPtr->replyHeaderSize);
        if (cSize > 0) {
            char buf[CHUNK_SIZE], *cData;

            /*
             * There is (a part of the) content, past headers.
             * At this point, it is important to note that we may
             * be encountering chunked or compressed content...
             * Hence we copy this part into the private buffer,
             * erase it from the memory and let the HttpAppendContent
             * do the "right thing".
             */
            cData = httpPtr->ds.string + httpPtr->replyHeaderSize;
            if (httpPtr->replyLength > 0 && cSize > httpPtr->replyLength) {
                cSize = httpPtr->replyLength;
            }
            memcpy(buf, cData, cSize);
            Ns_DStringSetLength(&httpPtr->ds, httpPtr->replyHeaderSize);
            if (HttpAppendContent(httpPtr, buf, cSize) != TCL_OK) {
                result = TCL_ERROR;
            }
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpGet --
 *
 *      Locate Http struct for a given taskID.
 *
 * Results:
 *      NS_TRUE on success, NS_FALSE otherwise.
 *
 * Side effects:
 *      Will update given httpPtrPtr with the pointer to NsHttpTask
 *
 *----------------------------------------------------------------------
 */

static bool
HttpGet(
    NsInterp *itPtr,
    const char *taskID,
    NsHttpTask **httpPtrPtr,
    bool remove
) {
    Tcl_HashEntry *hPtr;
    bool           success;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(taskID != NULL);
    NS_NONNULL_ASSERT(httpPtrPtr != NULL);

    hPtr = Tcl_FindHashEntry(&itPtr->httpRequests, taskID);
    if (hPtr == NULL) {
        Ns_TclPrintfResult(itPtr->interp, "no such request: %s", taskID);
        success = NS_FALSE;
    } else {
        *httpPtrPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);
        if (remove) {
            Tcl_DeleteHashEntry(hPtr);
        }
        success = NS_TRUE;
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpWaitForSocketEvent --
 *
 *        Wait until the specified event on socket.
 *
 * Results:
 *        Ns_ReturnCode
 *
 * Side effects:
 *        None
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
HttpWaitForSocketEvent(
    NS_SOCKET sock,
    short events,
    const Ns_Time *timeoutPtr
) {
    Ns_ReturnCode result;
    struct pollfd pollfd;
    int           retval;
    long          ms;

    pollfd.fd = (int)sock;
    pollfd.events = events;

    if (timeoutPtr == NULL) {
        ms = -1;
    } else {
        ms = (long)Ns_TimeToMilliseconds(timeoutPtr);
        if (ms == 0) {
            ms = 1;
        }
    }

    do {
        retval = ns_poll(&pollfd, (NS_POLL_NFDS_TYPE)1, ms);
    } while (retval == -1 && errno == NS_EINTR);

    switch (retval) {
    case 0:
        result = NS_TIMEOUT;
        break;
    case 1:
        result = NS_OK;
        break;
    default:
        result = NS_ERROR;
        break;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpConnect --
 *
 *        Open a connection to the given URL
 *        and construct an NsHttpTask to handle the request.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        On TCL_OK, updates httpPtrPtr with allocated NsHttpTask.
 *
 *----------------------------------------------------------------------
 */

static int
HttpConnect(
    NsInterp *itPtr,
    const char *method,
    const char *url,
    Tcl_Obj *proxyObj,
    Ns_Set *hdrPtr,
    ssize_t bodySize,
    Tcl_Obj *bodyObj,
    const char *bodyFileName,
    const char *cert,
    const char *caFile,
    const char *caPath,
    const char *sniHostname,
    bool verifyCert,
    bool keepHostHdr,
    Ns_Time *timeoutPtr,
    Ns_Time *expirePtr,
    Ns_Time *keepAliveTimeoutPtr,
    NsHttpTask **httpPtrPtr
) {
    Tcl_Interp     *interp;
    NsHttpTask     *httpPtr;
    Ns_DString     *dsPtr;
    bool            haveUserAgent = NS_FALSE, ownHeaders = NS_FALSE;
    bool            httpTunnel = NS_FALSE, httpProxy = NS_FALSE;
    unsigned short  portNr, defPortNr, pPortNr = 0;
    char           *url2, *pHost = NULL;
    Ns_URL          u;
    const char     *errorMsg = NULL;
    const char     *contentType = NULL;
    uint64_t        requestCount = 0u;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(httpPtrPtr != NULL);

    /*Ns_Log(Notice, "HttpConnect bodySize %ld body type %s", bodySize, bodyObj->typePtr?bodyObj->typePtr->name:"none");*/

    interp = itPtr->interp;
    assert(itPtr->servPtr != NULL);

    /*
     * Setup the NsHttpTask structure. From this point on
     * if something goes wrong, we must HttpClose().
     */
    httpPtr = ns_calloc(1u, sizeof(NsHttpTask));
    httpPtr->chunk = ns_calloc(1u, sizeof(NsHttpChunk));
    httpPtr->bodyFileFd = NS_INVALID_FD;
    httpPtr->spoolFd = NS_INVALID_FD;
    httpPtr->sock = NS_INVALID_SOCKET;
    httpPtr->spoolLimit = -1;
    httpPtr->url = ns_strdup(url);
    httpPtr->method = ns_strdup(method);
    httpPtr->replyHeaders = Ns_SetCreate(NS_SET_NAME_CLIENT_RESPONSE);
    httpPtr->servPtr = itPtr->servPtr;
    httpPtr->flags = NS_HTTP_HEADERS_PENDING;

    if (timeoutPtr != NULL) {
        httpPtr->timeout = ns_calloc(1u, sizeof(Ns_Time));
        *httpPtr->timeout = *timeoutPtr;
    }

    /*
     * Take keep-alive timeout either from provided flag, or from
     * configuration file.
     */
    if (keepAliveTimeoutPtr == NULL &&
        (itPtr->servPtr->httpclient.keepaliveTimeout.sec != 0
         || itPtr->servPtr->httpclient.keepaliveTimeout.usec != 0
        )) {
        keepAliveTimeoutPtr = &itPtr->servPtr->httpclient.keepaliveTimeout;
        Ns_Log(Ns_LogTaskDebug, "HttpConnect: use keep-alive " NS_TIME_FMT
               " from configuration file",
               (int64_t)keepAliveTimeoutPtr->sec, keepAliveTimeoutPtr->usec );
    }
    if (keepAliveTimeoutPtr != NULL) {
        httpPtr->keepAliveTimeout = *keepAliveTimeoutPtr;
    }

    Ns_GetTime(&httpPtr->stime);

    dsPtr = &httpPtr->ds;
    Tcl_DStringInit(&httpPtr->ds);
    Tcl_DStringInit(&httpPtr->chunk->ds);

    Ns_MasterLock();
    requestCount = ++httpClientRequestCount;
    Ns_MasterUnlock();

    Ns_MutexInit(&httpPtr->lock);
    (void)ns_uint64toa(dsPtr->string, requestCount);
    Ns_MutexSetName2(&httpPtr->lock, "ns:httptask", dsPtr->string);

    /*
     * Parse given URL into pieces. Accept a fully qualified URL only.
     * Make a non-const copy of url, in which Ns_ParseUrl can replace
     * the item separating characters with '\0' characters.
     */
    url2 = ns_strdup(url);
    if (Ns_ParseUrl(url2, NS_FALSE, &u, &errorMsg) != NS_OK
        || u.protocol == NULL
        || u.host  == NULL
        || u.path  == NULL
        || u.tail  == NULL) {

        Ns_TclPrintfResult(interp, "invalid URL \"%s\": %s", url, errorMsg);
        goto fail;
    }

    if (u.userinfo != NULL) {
        Ns_Log(Warning, "ns_http: userinfo '%s' ignored: %s", u.userinfo, url);
    }

    /*
     * If "-keep_host_header" option set
     * then "Host:" header must be given.
     */
    if (keepHostHdr == NS_TRUE) {
        if (hdrPtr == NULL || Ns_SetIFind(hdrPtr, hostHeader) == -1) {
            Ns_TclPrintfResult(interp, "-keep_host_header specified"
                               " but no Host header given");
            goto fail;
        }
    }

    /*
     * Check used protocol and protocol-specific parameters
     * and determine the default port (80 for HTTP, 443 for HTTPS)
     */
    if (STREQ("http", u.protocol)) {
        if (cert != NULL
            || caFile != NULL
            || caPath != NULL
            || verifyCert == NS_TRUE) {

            Ns_TclPrintfResult(interp, "HTTPS options allowed for HTTPS only");
            goto fail;
        }
        defPortNr = 80u;
    }
#ifdef HAVE_OPENSSL_EVP_H
    else if (STREQ("https", u.protocol)) {
        defPortNr = 443u;
    }
#endif
    else {
        Ns_TclPrintfResult(interp, "invalid URL \"%s\"", url);
        goto fail;
    }

    /*
     * Connect to specified port or to the default port.
     */
    if (u.port != NULL) {
        portNr = (unsigned short) strtol(u.port, NULL, 10);
    } else {
        portNr = defPortNr;
    }

    /*
     * For request body optionally open the backing file.
     */
    if (bodySize > 0 && bodyFileName != NULL) {
        httpPtr->bodyFileFd = ns_open(bodyFileName, O_RDONLY|O_CLOEXEC, 0);
        if (unlikely(httpPtr->bodyFileFd == NS_INVALID_FD)) {
            Ns_TclPrintfResult(interp, "cannot open file %s", bodyFileName);
            goto fail;
        }
    }

    /*
     * If content decompression allowed and no encodings explicitly set
     * we tell remote what we would accept per-default.
     */
#ifdef HAVE_ZLIB_H
    if (likely((httpPtr->flags & NS_HTTP_FLAG_DECOMPRESS) != 0u)) {
        if (hdrPtr == NULL || Ns_SetIFind(hdrPtr, acceptEncodingHeader) == -1) {
            const char *acceptEncodings = "gzip, deflate";

            if (hdrPtr == NULL) {
                hdrPtr = Ns_SetCreate(NULL);
                ownHeaders = NS_TRUE;
            }

            Ns_SetPutSz(hdrPtr, acceptEncodingHeader, acceptEncodingHeaderLength,
                        acceptEncodings, 13);
        }
    }
#endif

    /*
     * Check if we need to connect to the proxy server first.
     * If the passed dictionary contains "host" key, we expect
     * to find the "port" and (optionally) "tunnel" keys.
     * If host is found, we will proxy.
     * For https connections we will tunnel, otherwise we will
     * cache-proxy. We will tunnel always if optional "tunnel"
     * key is true.
     */
    if (proxyObj != NULL) {
        Tcl_Obj *keyObj, *valObj;

        keyObj = Tcl_NewStringObj("host", 4);
        valObj = NULL;
        if (Tcl_DictObjGet(interp, proxyObj, keyObj, &valObj) != TCL_OK) {
            Tcl_DecrRefCount(keyObj);
            goto fail; /* proxyObj is not a dictionary? */
        }
        Tcl_DecrRefCount(keyObj);
        pHost = (valObj != NULL) ? Tcl_GetString(valObj) : NULL;
        if (pHost != NULL) {
            int portval = 0;

            keyObj = Tcl_NewStringObj("port", 4);
            valObj = NULL;
            Tcl_DictObjGet(interp, proxyObj, keyObj, &valObj);
            Tcl_DecrRefCount(keyObj);
            if (valObj == NULL) {
                Ns_TclPrintfResult(interp, "missing proxy port");
                goto fail;
            }
            if (Tcl_GetIntFromObj(interp, valObj, &portval) != TCL_OK) {
                goto fail;
            }
            if (portval <= 0) {
                Ns_TclPrintfResult(interp, "invalid proxy port");
            }
            pPortNr = (unsigned short)portval;
            if (defPortNr == 443u) {
                httpTunnel = NS_TRUE;
            } else {
                keyObj = Tcl_NewStringObj("tunnel", 6);
                valObj = NULL;
                Tcl_DictObjGet(interp, proxyObj, keyObj, &valObj);
                Tcl_DecrRefCount(keyObj);
                if (valObj == NULL) {
                    httpTunnel = NS_FALSE;
                } else {
                    int tunnel;

                    if (Tcl_GetBooleanFromObj(interp, valObj, &tunnel) != TCL_OK) {
                        goto fail;
                    }
                    httpTunnel = (tunnel == 1) ? NS_TRUE : NS_FALSE;
                }
            }
            httpProxy = (defPortNr == 80u) && (httpTunnel == NS_FALSE);
        }
    }

    /*
     * Now we are ready to attempt the connection.
     * If no timeout given, assume 5 seconds.
     */

    {
        Ns_ReturnCode rc;
        Ns_Time       defaultTimout = {5, 0}, *toPtr = NULL, startTime;

        Ns_GetTime(&startTime);
        Ns_Log(Ns_LogTaskDebug, "HttpConnect: connecting to [%s]:%hu", u.host, portNr);

        /*
         * Open the socket to remote, assure it is writable
         */
        if (timeoutPtr != NULL && expirePtr != NULL) {
            if (Ns_DiffTime(timeoutPtr, expirePtr, NULL) < 0) {
                toPtr = timeoutPtr;
            } else {
                toPtr = expirePtr;
            }
        } else if (timeoutPtr != NULL) {
            toPtr = timeoutPtr;
        } else if (expirePtr != NULL) {
            toPtr = expirePtr;
        } else {
            toPtr = &defaultTimout;
        }
        if (httpTunnel == NS_TRUE) {
            httpPtr->sock = HttpTunnel(itPtr, pHost, pPortNr, u.host, portNr, toPtr);
            if (httpPtr->sock == NS_INVALID_SOCKET) {
                goto fail;
            }
        } else {
            char          *rhost = u.host;
            unsigned short rport = portNr;
            Tcl_DString    persistentKeyDs;
            bool           lookupSuccessful;
            NsHttpTask    *waitingHttpPtr = NULL;

            if (httpProxy == NS_TRUE) {
                rhost = pHost;
                rport = pPortNr;
            }

            Tcl_DStringInit(&persistentKeyDs);
            Ns_DStringPrintf(&persistentKeyDs, "%s:%hu", rhost, rport);
            httpPtr->persistentKey = Ns_DStringExport(&persistentKeyDs);

            if (strcasecmp(httpPtr->method, "HEAD") == 0) {
                /*
                 * Do not expect a response content.
                 */
                httpPtr->flags |= NS_HTTP_FLAG_EMPTY;
            }

            lookupSuccessful = PersistentConnectionLookup(httpPtr, &waitingHttpPtr);
            Ns_Log(Ns_LogTaskDebug, "PersistentConnectionLookup persistent key <%s> -> %d",
                   httpPtr->persistentKey, lookupSuccessful);

            if (lookupSuccessful) {
                /*
                 * We can reuse the connection data.
                 */
                httpPtr->sock = waitingHttpPtr->sock;
                httpPtr->ctx = waitingHttpPtr->ctx;
                httpPtr->ssl = waitingHttpPtr->ssl;
                Ns_Log(Ns_LogTaskDebug, "PersistentConnectionLookup REUSE sock %d ctx %p ssl %p",
                       httpPtr->sock, (void*) httpPtr->ctx, (void*) httpPtr->ssl);
                /*
                 * Invalidate reused members in waitingHttpPtr.
                 */
                waitingHttpPtr->sock = NS_INVALID_SOCKET;
                waitingHttpPtr->ctx = NULL;
                waitingHttpPtr->ssl = NULL;
            } else {
                /*
                 * Setup fresh connection data.
                 */
                httpPtr->sock = Ns_SockTimedConnect2(rhost, rport, NULL, 0, toPtr, &rc);
                if (httpPtr->sock == NS_INVALID_SOCKET) {
                    Ns_SockConnectError(interp, rhost, rport, rc);
                    if (rc == NS_TIMEOUT) {
                        HttpClientLogWrite(httpPtr, "connecttimeout");
                    }
                    goto fail;
                }
                if (Ns_SockSetNonBlocking(httpPtr->sock) != NS_OK) {
                    Ns_TclPrintfResult(interp, "can't set socket nonblocking mode");
                    goto fail;
                }
                rc = HttpWaitForSocketEvent(httpPtr->sock, POLLOUT, toPtr);
                if (rc != NS_OK) {
                    if (rc == NS_TIMEOUT) {
                        Ns_TclPrintfResult(interp, "timeout waiting for writable socket");
                        HttpClientLogWrite(httpPtr, "writetimeout");
                        Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
                    } else {
                        Ns_TclPrintfResult(interp, "waiting for writable socket: %s",
                                           ns_sockstrerror(ns_sockerrno));
                    }
                    goto fail;
                }

                /*
                 * Optionally setup an SSL connection
                 */
                if (defPortNr == 443u) {
                    NS_TLS_SSL_CTX *ctx = NULL;
                    int             result;

                    result = Ns_TLS_CtxClientCreate(interp, cert, caFile, caPath,
                                                    verifyCert, &ctx);
                    if (likely(result == TCL_OK)) {
                        NS_TLS_SSL *ssl = NULL;
                        Ns_Time now, remainingTime;

                        httpPtr->ctx = ctx;
                        Ns_GetTime(&now);
                        Ns_DiffTime(&now, &startTime, &remainingTime);
                        if (Ns_DiffTime(toPtr, &remainingTime, &remainingTime) < 0) {
                            /*
                             * The remaining timeout is already negative,
                             * already too late to call Ns_TLS_SSLConnect()
                             */
                            Ns_Log(Ns_LogTaskDebug, "Ns_TLS_SSLConnect negative remaining timeout " NS_TIME_FMT,
                                   (int64_t)remainingTime.sec, remainingTime.usec);
                            Ns_TclPrintfResult(interp, "timeout waiting for TLS setup");
                            HttpClientLogWrite(httpPtr, "tlssetuptimeout");
                            Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
                            goto fail;
                        } else {
                            Ns_Log(Ns_LogTaskDebug, "Ns_TLS_SSLConnect remaining timeout " NS_TIME_FMT,
                                   (int64_t)remainingTime.sec, remainingTime.usec);
                            /*
                             * If the user has specified an sniHostname, use
                             * it. Otherwise use the hostname from the URL,
                             * when it is non-numeric.
                             */
                            if (sniHostname == NULL && !NsHostnameIsNumericIP(rhost)) {
                                sniHostname = rhost;
                                Ns_Log(Debug, "automatically use SNI <%s>", rhost);
                            }
                            rc = Ns_TLS_SSLConnect(interp, httpPtr->sock, ctx,
                                                   sniHostname, &remainingTime, &ssl);
                            if (rc == NS_TIMEOUT) {
                                /*
                                 * Ns_TLS_SSLConnect ran into a timeout.
                                 */
                                Ns_TclPrintfResult(interp, "timeout waiting for TLS handshake");
                                HttpClientLogWrite(httpPtr, "tlsconnecttimeout");
                                Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
                                goto fail;

                            } else if (rc == NS_ERROR) {
                                result = TCL_ERROR;
                            } else {
                                result = TCL_OK;
                            }
                        }

                        if (likely(result == TCL_OK)) {
                            httpPtr->ssl = ssl;
#ifdef HAVE_OPENSSL_EVP_H
                            HttpAddInfo(httpPtr, "sslversion", SSL_get_version(ssl));
                            HttpAddInfo(httpPtr, "cipher", SSL_get_cipher(ssl));
                            SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
#endif
                        }
                    }
                    if (unlikely(result != TCL_OK)) {
                        goto fail;
                    }
                }
            }
        }
    }

    /*
     * At this point we are connected.
     * Construct HTTP request line.
     */
    Ns_DStringSetLength(dsPtr, 0);
    Ns_DStringAppend(dsPtr, method);
    Ns_StrToUpper(Ns_DStringValue(dsPtr));
    if (httpProxy == NS_TRUE) {
        Ns_DStringNAppend(dsPtr, " ", 1);
        Ns_DStringNAppend(dsPtr, url, TCL_INDEX_NONE);
    } else {
        Ns_DStringNAppend(dsPtr, " /", 2);
        if (*u.path != '\0') {
            Ns_DStringNAppend(dsPtr, u.path, TCL_INDEX_NONE);
            Ns_DStringNAppend(dsPtr, "/", 1);
        }
        Ns_DStringNAppend(dsPtr, u.tail, TCL_INDEX_NONE);
        if (u.query != NULL) {
            Ns_DStringNAppend(dsPtr, "?", 1);
            Ns_DStringNAppend(dsPtr, u.query, TCL_INDEX_NONE);
        }
        if (u.fragment != NULL) {
            Ns_DStringNAppend(dsPtr, "#", 1);
            Ns_DStringNAppend(dsPtr, u.fragment, TCL_INDEX_NONE);
        }
    }
    Ns_DStringNAppend(dsPtr, " HTTP/1.1\r\n", 11);

    Ns_Log(Ns_LogTaskDebug, "HttpConnect: %s request: %s", u.protocol, dsPtr->string);

    /*
     * Add provided headers, remove headers we are providing explicitly,
     * check User-Agent header existence.
     */
    if (ownHeaders == NS_FALSE && hdrPtr != NULL) {
        size_t ii;

        if (keepHostHdr == NS_FALSE) {
            Ns_SetIDeleteKey(hdrPtr, hostHeader);
        }
        Ns_SetIDeleteKey(hdrPtr, contentLengthHeader);
        Ns_SetIDeleteKey(hdrPtr, connectionHeader);
        for (ii = 0u; ii < Ns_SetSize(hdrPtr); ii++) {
            const char *key, *val;

            key = Ns_SetKey(hdrPtr, ii);
            val = Ns_SetValue(hdrPtr, ii);
            Ns_DStringPrintf(dsPtr, "%s: %s\r\n", key, val);

            if (haveUserAgent == NS_FALSE) {
                haveUserAgent = (strcasecmp(key, userAgentHeader) == 0);
            }
        }
    }

    /*
     * If User-Agent header not supplied, add our own
     */
    if (haveUserAgent == NS_FALSE) {
        Ns_DStringPrintf(dsPtr, "%s: %s/%s\r\n", userAgentHeader,
                         Ns_InfoServerName(), Ns_InfoServerVersion());
    }

    /*
     * Disable keep-alive connections, when no keep-alive timeout is
     * specified.
     */
    Ns_Log(Ns_LogTaskDebug, "HttpConnect: keepAliveTimeoutPtr %p", (void*)keepAliveTimeoutPtr);
    if (keepAliveTimeoutPtr == NULL
        || (keepAliveTimeoutPtr->sec == 0 && keepAliveTimeoutPtr->usec == 0)
       ) {
        Ns_DStringPrintf(dsPtr, "%s: close\r\n", connectionHeader);
    }

    /*
     * Optionally, add our own Host header
     */
    if (keepHostHdr == NS_FALSE) {
        (void)Ns_DStringVarAppend(dsPtr, hostHeader, ": ", (char *)0L);
        (void)Ns_HttpLocationString(dsPtr, NULL, u.host, portNr, defPortNr);
        Ns_DStringNAppend(dsPtr, "\r\n", 2);
    }

    Ns_Log(Ns_LogTaskDebug, "HttpConnect: %s request: %s",
           u.protocol, dsPtr->string);

    /*
     * Calculate Content-Length header, handle in-memory body
     */
    if (bodyObj == NULL && bodySize == 0) {

        /*
         * No body provided, close request/headers part
         */
        httpPtr->bodySize = 0u;
        Ns_DStringNAppend(dsPtr, "\r\n", 2);
        httpPtr->requestHeaderSize = (size_t)dsPtr->length;

    } else {

        if (ownHeaders == NS_FALSE && hdrPtr != NULL) {
            contentType = Ns_SetIGet(hdrPtr, contentTypeHeader);
        }

        if (contentType == NULL) {

            /*
             * Previously, we required a content-type when a body is provided,
             * which was too strong due to the following paragraph in RFC 7231:
             *
             *    A sender that generates a message containing a payload body
             *    SHOULD generate a Content-Type header field in that message
             *    unless the intended media type of the enclosed
             *    representation is unknown to the sender.  If a Content-Type
             *    header field is not present, the recipient MAY either assume
             *    a media type of "application/octet-stream" ([RFC2046],
             *    Section 4.5.1) or examine the data to determine its type.
             */

            if (bodyFileName != NULL) {
                contentType = Ns_GetMimeType(bodyFileName);
            } else {
                /*
                 * We could call Ns_GetMimeType(tail), but this does not seem
                 * to be the intention of RFC2046.
                 */
                contentType = "application/octet-stream";
            }
        }

        if (bodyObj != NULL) {
            TCL_SIZE_T bodyLen = 0;
            char      *bodyStr = NULL;
            bool       binary;

            /*
             * Append in-memory body to the requests string
             * and calculate correct Content-Length header.
             * We do not anticipate in-memory body to be
             * 2GB+ hence the signed int type suffices.
             */
            binary = NsTclObjIsByteArray(bodyObj);

            if (binary == NS_FALSE) {
                if (contentType != NULL) {

                    /*
                     * Caveat Emptor:
                     * This call may return true even for
                     * completely regular text formats.
                     */
                    binary = Ns_IsBinaryMimeType(contentType);
                }
            }
            if (binary == NS_TRUE) {
                /*Ns_Log(Notice, "... bodyObj has type %s ", bodyObj->typePtr?bodyObj->typePtr->name:"NONE");*/
                /*Ns_Log(Notice, "... before GetByteArrayFromObj body <%s>", Tcl_GetString(bodyObj));*/
                bodyStr = (char *)Tcl_GetByteArrayFromObj(bodyObj, &bodyLen);
                /*NsHexPrint("after GetByteArrayFromObj", (unsigned char *)bodyStr, (size_t)bodyLen, 20, NS_TRUE);*/
//#define JAN 1
#ifdef JAN
                if (bodyStr == NULL) {
                    Ns_TclPrintfResult(interp, "Body is not really binary");
                    goto fail;
                }
#endif
#if !defined(NS_TCL_PRE9)
                if (bodyStr == NULL) {
                    bodyStr = (char *)Tcl_GetBytesFromObj(interp, bodyObj, &bodyLen);
                    Ns_Log(Notice, "... Tcl_GetBytesFromObj returned len %zu body '%p'", bodyLen, (void*)bodyStr);
                }
#endif
                //Ns_Log(Notice, "... body as bytearray len %zu body '%s'", bodyLen, bodyStr);
            } else {
                bodyStr = Tcl_GetStringFromObj(bodyObj, &bodyLen);
            }

            httpPtr->bodySize = (size_t)bodyLen;
            Ns_DStringPrintf(dsPtr, "%s: %" PRITcl_Size "\r\n\r\n", contentLengthHeader,
                             bodyLen);

            httpPtr->requestHeaderSize = (size_t)dsPtr->length;
            Ns_DStringNAppend(dsPtr, bodyStr, bodyLen);

        } else if (bodySize > 0) {

            /*
             * Body will be passed over file/channel and the caller
             * has already determined the correct content size.
             * Note: body size may be way over 2GB!
             */
            httpPtr->bodySize = (size_t)bodySize;
            Ns_DStringPrintf(dsPtr, "%s: %" PRIdz "\r\n\r\n",
                             contentLengthHeader, bodySize);
            httpPtr->requestHeaderSize = (size_t)dsPtr->length;
        }
    }

    httpPtr->requestLength = (size_t)dsPtr->length;
    httpPtr->next = dsPtr->string;

    *httpPtrPtr = httpPtr;
    ns_free((void *)url2);

    if (Ns_LogSeverityEnabled(Ns_LogRequestDebug)) {
        Tcl_DString d;

        Tcl_DStringInit(&d);
        Ns_Log(Ns_LogRequestDebug, "full request (len %" PRITcl_Size ") <%s>",
               dsPtr->length,
               Ns_DStringAppendPrintable(&d, NS_TRUE, dsPtr->string,
                                         (size_t)dsPtr->length));
        Tcl_DStringFree(&d);
    }


    if (ownHeaders == NS_TRUE) {
        Ns_SetFree(hdrPtr);
    }

    return TCL_OK;

 fail:
    if (ownHeaders == NS_TRUE) {
        Ns_SetFree(hdrPtr);
    }
    ns_free((void *)url2);
    HttpClose(httpPtr);

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpAppendRawBuffer --
 *
 *        Append data to a spool file, a Tcl channel or memory.
 *
 * Results:
 *        Tcl result code.
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpAppendRawBuffer(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) {
    int     result = TCL_OK;
    ssize_t written;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (httpPtr->recvSpoolMode == NS_TRUE) {
        if (httpPtr->spoolFd != NS_INVALID_FD) {
            written = ns_write(httpPtr->spoolFd, buffer, size);
        } else if (httpPtr->spoolChan != NULL) {
            written = (ssize_t)Tcl_Write(httpPtr->spoolChan, buffer, (TCL_SIZE_T)size);
        } else {
            written = -1;
        }
    } else {
        Tcl_DStringAppend(&httpPtr->ds, buffer, (TCL_SIZE_T)size);
        written = (ssize_t)size;
    }

    if (written > -1) {
        result = TCL_OK;
    } else {
        Ns_Log(Error, "HttpAppendRawBuffer: spooling of received content failed");
        result = TCL_ERROR;
    }

    return result;
}
/*
 *----------------------------------------------------------------------
 *
 * SkipMessage --
 *
 *        Skip the incoming message. This is needed e.g. for "100
 *        continue" handling.
 *
 * Results:
 *        Tcl result code.
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */
static int
SkipMessage(
    NsHttpTask *httpPtr
) {
    int result = TCL_OK;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "RESET 1xx");

    if (httpPtr->recvSpoolMode == NS_TRUE) {
        /*
         * Spool mode is activated after the header
         * processing. Therefore, it should be here set to NS_FALSE.
         */
        Ns_Log(Error, "ns_http: SkipMessage is called in spool mode (should never happen).");

    } else {
        /*
        NsHexPrint("old buffer", (unsigned char*)httpPtr->ds.string, httpPtr->ds.length,
                   20, NS_TRUE);
        Ns_Log(Notice, "... old <%s>", httpPtr->ds.string);
        Ns_Log(Notice, "... replyHeaderSize %d current Size %d",
               httpPtr->replyHeaderSize, httpPtr->ds.length);
        */
        if (httpPtr->ds.length == httpPtr->replyHeaderSize) {
            /*
             * We have received just the header. Skip it.
             */
            Tcl_DStringSetLength(&httpPtr->ds, (TCL_SIZE_T)0);

        } else if (httpPtr->ds.length > httpPtr->replyHeaderSize) {
            TCL_SIZE_T newSize;
            /*
             * We have received more than the header. Move remaining
             * content upfront in the buffer.
             */
            newSize =  httpPtr->ds.length - httpPtr->replyHeaderSize;
            assert(newSize>=0);
            memmove(httpPtr->ds.string, httpPtr->ds.string + httpPtr->replyHeaderSize, (size_t)newSize);
            Tcl_DStringSetLength(&httpPtr->ds, newSize);

        } else {
            Ns_Log(Error, "ns_http: SkipMessage called with header size way too large"
                   "(should never happen)");
        }
        /*
        NsHexPrint("new buffer", (unsigned char*)httpPtr->ds.string, httpPtr->ds.length,
                   20, NS_TRUE);
        Ns_Log(Notice, "... new <%s>", httpPtr->ds.string);
        Ns_Log(Notice, "... replyLength %zu replySize %zu",
               httpPtr->replyLength, httpPtr->replySize);
        */
        httpPtr->replyHeaderSize = 0;
    }
    httpPtr->flags |= NS_HTTP_HEADERS_PENDING;
    httpPtr->status = 0;

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * HttpAppendBuffer
 *
 *        Append data (w/ or w/o compression) to the spool file
 *        or Tcl channel or memory.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        May uncompress datat in the passed buffer.
 *
 *----------------------------------------------------------------------
 */

static int
HttpAppendBuffer(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) {
    int    result = TCL_OK;
    size_t bodySize = 0u;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    Ns_Log(Ns_LogTaskDebug, "HttpAppendBuffer: got %" PRIuz " bytes flags:%.6x",
           size, httpPtr->flags);

    if (unlikely((httpPtr->flags & NS_HTTP_FLAG_DECOMPRESS) == 0u)
        || likely((httpPtr->flags & NS_HTTP_FLAG_GZIP_ENCODING) == 0u)) {

        /*
         * Output raw content
         */
        result = HttpAppendRawBuffer(httpPtr, buffer, size);
        if (result == TCL_OK) {
            bodySize = size;
        }

    } else {
        char out[CHUNK_SIZE];

        out[0] = '\0';

        /*
         * Decompress content
         */
        (void) Ns_InflateBufferInit(httpPtr->compress, buffer, size);
        do {
            size_t ul = 0u;

            result = Ns_InflateBuffer(httpPtr->compress, out, CHUNK_SIZE, &ul);
            if (HttpAppendRawBuffer(httpPtr, out, ul) == TCL_OK) {
                bodySize += ul;
            } else {
                result = TCL_ERROR;
            }
        } while(result == TCL_CONTINUE);
    }

    if (result == TCL_OK) {
        if (httpPtr->replyHeaderSize > 0 && httpPtr->status > 0) {

            /*
             * Headers and status have been parsed so all the
             * data coming from this point are counted up as
             * being the (uncompressed, decoded) reply content.
             */
            Ns_MutexLock(&httpPtr->lock);
            httpPtr->replyBodySize += bodySize;
            httpPtr->replySize += size;
            Ns_MutexUnlock(&httpPtr->lock);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpAppendContent
 *
 *        Append reply content to where it belongs,
 *        potentially decoding the chunked reply format.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpAppendContent(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) {
    int result;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if ((httpPtr->flags & NS_HTTP_FLAG_CHUNKED) == 0u) {
        result = HttpAppendBuffer(httpPtr, buffer, size);
    } else {
        result = HttpAppendChunked(httpPtr, buffer, size);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpAppendChunked
 *
 *        Parse chunked content.
 *
 *        This implements a simple state machine that parses data
 *        delivered blockwise. As the chunked-format may be
 *        sliced on an arbitrary point between the blocks, we must
 *        operate character-wise and maintain the internal state.
 *        In order not to write yet-another completely closed and
 *        fixed parser for the format, here is the implementation
 *        of a simple state machine that can be easily programmed
 *        to parse any character sequence, including the chunked.
 *
 *        The machine consists of a set of callbacks. Each callback
 *        operates on the passed buffer and size of data in the
 *        buffer. Callbacks are invoked in the order how they are
 *        specified in the array. Each callback returns signals
 *        that influence the order of callback invocation.
 *        Also each callback can replace the callback-set during
 *        its operation and adjust the pointer to the next in row.
 *        The signals returned by each callback include:
 *
 *            TCL_OK      done regularly, go to the next one
 *            TCL_BREAK   re-start from the first callback
 *            TCL_ERROR   stops parsing
 *
 *        Callbacks are invoked one after another until there is
 *        unrocessed data in the buffer.
 *        The last callback is marked as NULL. After reaching it
 *        all is repeated from the beginning. When all data is
 *        consumed the callback that encountered that state
 *        usually returns TCL_BREAK which stops the machine and
 *        gives the control back to the user.
 *        Each callback adjusts the number of bytes left in the
 *        buffer and repositions the buffer to skip consumed
 *        characters.
 *
 *        Writing a parser requires writing one or more
 *        HttpParserProcs, stuffing them in an array
 *        terminated by the NULL parser and starting the
 *        machine by simply invoking the registered procs.
 *
 *        Due to its universal nature, this code can be made
 *        independent of NsHttp and re-used elsewhere.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpAppendChunked(
    NsHttpTask *httpPtr,
    const char *buffer,
    size_t size
) {
    int          result = TCL_OK;
    char        *buf = (char *)buffer;
    size_t       len = size;
    NsHttpChunk *chunkPtr;
    NsHttpParseProc *parseProcPtr = NULL;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    chunkPtr = httpPtr->chunk;
    assert(chunkPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "HttpAppendChunked: http:%p, task:%p bytes:%lu",
           (void*)httpPtr, (void*)httpPtr->task, size);
    /* NsHexPrint("", buffer, size, 20, NS_TRUE);*/

    while (len > 0 && result != TCL_ERROR) {

        Ns_Log(Ns_LogTaskDebug, "... len %lu ", len);

        parseProcPtr = *(chunkPtr->parsers + chunkPtr->callx);
        while (len > 0 && parseProcPtr != NULL) {
            result = (*parseProcPtr)(httpPtr, &buf, &len);
            Ns_Log(Ns_LogTaskDebug, "... parse proc %d from %p returns %s ",
                   chunkPtr->callx, (void*)chunkPtr->parsers, result == TCL_OK ? "OK" : "not ok");
            if (result != TCL_OK) {
                break;
            }
            chunkPtr->callx++;
            parseProcPtr = *(chunkPtr->parsers + chunkPtr->callx);
        }
        if (parseProcPtr == NULL) {
            chunkPtr->callx = 0; /* Repeat from the first proc */
        }
    }
    /*
     * When we reach the end, len == 0 and we jump out of the loop. When we
     * have reached the end parser, call it here.
     */
    if (parseProcPtr == ParseEndProc) {
        result = (*parseProcPtr)(httpPtr, &buf, &len);
    }

    if (result != TCL_ERROR) {
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CloseWaitProc --
 *
 *        Task Proc for connection-close handling.
 *
 * Results:
 *        None
 *
 * Side effects:
 *        DONE handler calls Ns_TaskFree()
 *
 *----------------------------------------------------------------------
 */
static void
CloseWaitProc(
    Ns_Task *task,
    NS_SOCKET UNUSED(sock),
    void *arg,
    Ns_SockState why
) {
    NsHttpTask  *httpPtr;

    NS_NONNULL_ASSERT(task != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    httpPtr = (NsHttpTask *)arg;

    switch (why) {
    case NS_SOCK_INIT: {
        Ns_Log(Ns_LogTaskDebug, "CloseWaitProc: INIT %p, key <%s> flags %.6x",
               (void*)task, httpPtr->persistentKey, httpPtr->flags);
        break;
    }

    case NS_SOCK_TIMEOUT: {
        Ns_Log(Ns_LogTaskDebug, "CloseWaitProc: TIMEOUT %p, key <%s> flags %.6x",
               (void*)task, httpPtr->persistentKey, httpPtr->flags);

        httpPtr->flags &= ~NS_HTTP_KEEPALIVE;
        HttpClose(httpPtr);
        break;
    }

    case NS_SOCK_CANCEL: {
        Ns_Log(Ns_LogTaskDebug, "CloseWaitProc: CANCEL %p, key <%s> flags %.6x",
               (void*)task, httpPtr->persistentKey, httpPtr->flags);
        httpPtr->flags &= ~NS_HTTP_KEEPALIVE;
        /*
         * CANCEL will be followed by DONE.
         */
        break;
    }

    case NS_SOCK_DONE: {
        Ns_Log(Ns_LogTaskDebug, "CloseWaitProc: DONE %p, key <%s> flags %.6x",
               (void*)task, httpPtr->persistentKey, httpPtr->flags);
        httpPtr->flags &= ~NS_HTTP_KEEPALIVE;
        Ns_Log(Ns_LogTaskDebug, "TaskFree %p in CloseWaitProc DONE", (void*)task);
        (void) Ns_TaskFree(task);
        break;
    }
    case NS_SOCK_READ:      NS_FALL_THROUGH; /* fall through */
    case NS_SOCK_EXIT:      NS_FALL_THROUGH; /* fall through */
    case NS_SOCK_NONE:      NS_FALL_THROUGH; /* fall through */
    case NS_SOCK_WRITE:     NS_FALL_THROUGH; /* fall through */
    case NS_SOCK_EXCEPTION: NS_FALL_THROUGH; /* fall through */
    case NS_SOCK_AGAIN:
    Ns_Log(Warning, "CloseWaitProc: unexpected condition %04x httpPtr->task:%p closeWait:%p"
           " task:%p key <%s> flags %.6x",
           why, (void*)httpPtr->task, (void*)httpPtr->closeWaitTask, (void*)task,
           httpPtr->persistentKey, httpPtr->flags);
    break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * HttpCleanupPerRequestData --
 *
 *        Cleanup per-request data. This is in essence everything inside
 *        NsHttpTask except the keep-alive specific connection data.
 *
 * Results:
 *        None
 *
 * Side effects:
 *        Free up memory
 *
 *----------------------------------------------------------------------
 */
static void
HttpCleanupPerRequestData(
    NsHttpTask *httpPtr
) {

    NS_NONNULL_ASSERT(httpPtr != NULL);

    if (httpPtr->spoolFileName != NULL) {
        ns_free((void *)httpPtr->spoolFileName);
        httpPtr->spoolFileName = NULL;
    }
    if (httpPtr->doneCallback != NULL) {
        ns_free((void *)httpPtr->doneCallback);
        httpPtr->doneCallback = NULL;
    }
    if (httpPtr->spoolFd != NS_INVALID_FD) {
        (void)ns_close(httpPtr->spoolFd);
        httpPtr->spoolFd = NS_INVALID_SOCKET;
    }
    if (httpPtr->bodyFileFd != NS_INVALID_FD) {
        (void)ns_close(httpPtr->bodyFileFd);
        httpPtr->bodyFileFd = NS_INVALID_SOCKET;
    }
    if (httpPtr->bodyChan != NULL) {
        (void)Tcl_Close(NULL, httpPtr->bodyChan);
        httpPtr->bodyChan = NULL;
    }
    if (httpPtr->spoolChan != NULL) {
        (void)Tcl_Close(NULL, httpPtr->spoolChan);
        httpPtr->spoolChan = NULL;
    }
    if (httpPtr->compress != NULL) {
        (void)Ns_InflateEnd(httpPtr->compress);
        ns_free((void *)httpPtr->compress);
        httpPtr->compress = NULL;
    }
    if (httpPtr->infoObj != NULL) {
        Tcl_DecrRefCount(httpPtr->infoObj);
        httpPtr->infoObj = NULL;
    }
    if (httpPtr->replyHeaders != NULL) {
        Ns_SetFree(httpPtr->replyHeaders);
        httpPtr->replyHeaders = NULL;
    }
    if (httpPtr->timeout != NULL) {
        ns_free((void *)httpPtr->timeout);
        httpPtr->timeout = NULL;
    }
    ns_free((void *)httpPtr->url);
    httpPtr->url = NULL;

    ns_free((void *)httpPtr->method);
    httpPtr->method = NULL;

    Ns_MutexDestroy(&httpPtr->lock); /* Should not be held locked here! */
    Tcl_DStringFree(&httpPtr->ds);

    if (httpPtr->chunk != NULL) {
        Tcl_DStringFree(&httpPtr->chunk->ds);
        ns_free((void *)httpPtr->chunk);
        httpPtr->chunk = NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * HttpClose --
 *
 *        Finish task and cleanup memory
 *
 * Results:
 *        None
 *
 * Side effects:
 *        Free up memory
 *
 *----------------------------------------------------------------------
 */
static void
HttpClose(
    NsHttpTask *httpPtr
) {
    NS_NONNULL_ASSERT(httpPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "HttpClose: http:%p, task:%p closeWait:%p sock %d",
           (void*)httpPtr, (void*)httpPtr->task, (void*)httpPtr->closeWaitTask,
           httpPtr->sock);

    /*
     * When HttpConnect runs into a failure, it might not have httpPtr->task
     * set. We cannot be sure, the task is always set.
     */

    /*Ns_Log(Notice, "HttpClose bodyfileFd %d spoolFd %d",
      httpPtr->bodyFileFd,  httpPtr->spoolFd);*/

    if (httpPtr->task != NULL) {

        if (httpPtr->sock != NS_INVALID_SOCKET
            && (httpPtr->flags & NS_HTTP_KEEPALIVE) != 0u
           ) {
            const char *reason;

            if (PersistentConnectionAdd(httpPtr, &reason)) {
                httpPtr->closeWaitTask = Ns_TaskTimedCreate(httpPtr->sock, CloseWaitProc, httpPtr,
                                                            &httpPtr->keepAliveTimeout);
                LogDebug("HttpClose", httpPtr, "keepalive");

                assert(taskQueue != NULL);

                if (Ns_TaskEnqueue(httpPtr->closeWaitTask, taskQueue) != NS_OK) {
                    Ns_Log(Error, "Could not enqueue CloseWait task");
                }

                Ns_Log(Ns_LogTaskDebug, "Added persistent connection entry <%s> for %p",
                       httpPtr->persistentKey, (void*)httpPtr);
            } else {
                Ns_Log(Warning, "Could not add persistent connection (reason %s, key %s, closeWait %d)",
                       reason, httpPtr->persistentKey, (httpPtr->persistentKey != NULL));
                /*
                 * Clear keep-alive flag.
                 */
                httpPtr->flags &= ~NS_HTTP_KEEPALIVE;

            }
            HttpCleanupPerRequestData(httpPtr);
            return;

        } else {
            Ns_Log(Ns_LogTaskDebug, "TaskFree %p in HttpClose", (void*)(httpPtr->closeWaitTask));
            LogDebug("HttpClose", httpPtr, "no keepalive");

            if (httpPtr->closeWaitTask != NULL) {
                Ns_Log(Warning, "ns_http close: no keepalive set, detected unexpected closeWaitTask for %s",
                       httpPtr->persistentKey);
                (void) Ns_TaskFree(httpPtr->closeWaitTask);
                httpPtr->closeWaitTask = NULL;
            }
            (void) Ns_TaskFree(httpPtr->task);
            httpPtr->task = NULL;
        }
    }
#ifdef HAVE_OPENSSL_EVP_H
    if (httpPtr->ssl != NULL) {
        SSL_shutdown(httpPtr->ssl);
        SSL_free(httpPtr->ssl);
        httpPtr->ssl = NULL;
    }
    if (httpPtr->ctx != NULL) {
        SSL_CTX_free(httpPtr->ctx);
        httpPtr->ctx = NULL;
    }
#endif
    if (httpPtr->sock != NS_INVALID_SOCKET) {
        ns_sockclose(httpPtr->sock);
        httpPtr->sock = NS_INVALID_SOCKET;
    }
    if (httpPtr->persistentKey != NULL) {
        if (PersistentConnectionDelete(httpPtr)) {
            /*Ns_Log(Warning, "persistent connections: deleting persistent key");*/
        }
        ns_free((void *)httpPtr->persistentKey);
        httpPtr->persistentKey = NULL;
    }
    HttpCleanupPerRequestData(httpPtr);

    ns_free((void *)httpPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCancel --
 *
 *        Mark the task as cancelled and wait (indefinitely)
 *        for the task to finish.
 *
 * Results:
 *        None.
 *
 * Side effects:
 *        May free task-related memory
 *
 *----------------------------------------------------------------------
 */

static void
HttpCancel(
    NsHttpTask *httpPtr
) {
    NS_NONNULL_ASSERT(httpPtr != NULL);
    assert(httpPtr->task != NULL);

    (void) Ns_TaskCancel(httpPtr->task);
    Ns_TaskWaitCompleted(httpPtr->task);

    if (httpPtr->closeWaitTask != NULL) {
        (void) Ns_TaskCancel(httpPtr->closeWaitTask);
        Ns_TaskWaitCompleted(httpPtr->closeWaitTask);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * HttpAddInfo --
 *
 *        Adds some task-related information
 *        in form of a Tcl dictionary.
 *
 * Results:
 *        None.
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static void
HttpAddInfo(
    NsHttpTask *httpPtr,
    const char *key,
    const char *value
) {
    Tcl_Obj *keyObj, *valObj;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    if (httpPtr->infoObj == NULL) {
        httpPtr->infoObj = Tcl_NewDictObj();
        Tcl_IncrRefCount(httpPtr->infoObj);
    }

    keyObj = Tcl_NewStringObj(key, TCL_INDEX_NONE);
    valObj = Tcl_NewStringObj(value, TCL_INDEX_NONE);

    Tcl_DictObjPut(NULL, httpPtr->infoObj, keyObj, valObj);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpTaskSend --
 *
 *        Send data via plain TCP or via OpenSSL.
 *        May send less data then requested.
 *
 * Results:
 *        Number of bytes sent or -1 on error.
 *
 * Side effects:
 *        If passed length of 0, will do nothing (and return 0).
 *        Otherwise, if unable to send data, will return 0,
 *        if the underlying socket is (still) not writable.
 *        In such cases, caller must repeat the operation after
 *        making sure (by whatever means) the socket is writable.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
HttpTaskSend(
    const NsHttpTask *httpPtr,
    const void *buffer,
    size_t length
) {
    ssize_t sent;
    struct  iovec iov;
    const struct iovec *bufs = &iov;
    int     nbufs = 1;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    iov.iov_base = (char*)buffer;
    iov.iov_len = length;

    if (httpPtr->ssl == NULL) {
        sent = Ns_SockSendBufs2(httpPtr->sock, bufs, nbufs, 0);
    } else {
#ifndef HAVE_OPENSSL_EVP_H
        sent = -1;
#else
        sent = Ns_SSLSendBufs2(httpPtr->ssl, bufs, nbufs);
#endif
    }

    Ns_Log(Ns_LogTaskDebug, "HttpTaskSend: sent %" PRIdz
           " bytes (out of %" PRIuz ")", sent, length);

    return sent;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpTaskRecv --
 *
 *        Receive data via plain TCP or via OpenSSL.
 *
 * Results:
 *        Number of bytes received or -1 on error
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
HttpTaskRecv(
    const NsHttpTask *httpPtr,
    char *buffer,
    size_t length,
    Ns_SockState *statePtr
) {
    ssize_t       recv;
    int           nbufs = 1;
    unsigned long recvErrorCode;
    struct  iovec iov, *bufs = &iov;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    iov.iov_base = (void *)buffer;
    iov.iov_len  = length;

    if (httpPtr->ssl == NULL) {
        recv = Ns_SockRecvBufs2(httpPtr->sock, bufs, nbufs, 0u, statePtr, &recvErrorCode);
    } else {
#ifndef HAVE_OPENSSL_EVP_H
        recv = -1;
#else
        recv = Ns_SSLRecvBufs2(httpPtr->ssl, bufs, nbufs, statePtr, &recvErrorCode);
#endif
    }

    Ns_Log(Ns_LogTaskDebug, "HttpTaskRecv: received %" PRIdz
           " bytes (buffer size %" PRIuz ")", recv, length);

    return recv;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpDoneCallback --
 *
 *        Evaluate the doneCallback. For the time being, this is
 *        executed in the default server context (may not be right!).
 *
 * Results:
 *        None
 *
 * Side effects:
 *        Many, depending on the callback.
 *        Http task is garbage collected.
 *
 *----------------------------------------------------------------------
 */
static void
HttpDoneCallback(
    NsHttpTask *httpPtr
) {
    int          result;
    Tcl_Interp  *interp;
    Tcl_DString  script;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    LogDebug("HttpDoneCallback", httpPtr, "");

    interp = NsTclAllocateInterp( httpPtr->servPtr);

    result = HttpGetResult(interp, httpPtr);

    Tcl_DStringInit(&script);
    Tcl_DStringAppend(&script, httpPtr->doneCallback, TCL_INDEX_NONE);
    Ns_DStringPrintf(&script, " %d ", result);
    Tcl_DStringAppendElement(&script, Tcl_GetStringResult(interp));

    /*
     * Splice body/spool channels into the callback interp.
     * All supplied channels must be closed by the callback.
     * Alternatively, the Tcl will close them at the point
     * of interp de-allocation, which might not be safe.
     */
    HttpSpliceChannels(interp, httpPtr);

    result = Tcl_EvalEx(interp, script.string, script.length, 0);

    if (result != TCL_OK) {
        (void) Ns_TclLogErrorInfo(interp, "\n(context: httptask)");
    }

    Tcl_DStringFree(&script);
    Ns_TclDeAllocateInterp(interp);

    HttpClose(httpPtr); /* This frees the httpPtr! */
}


/*
 *----------------------------------------------------------------------
 *
 * HttpProc --
 *
 *        Task callback for ns_http connections.
 *        This is a state-machine that Ns_Task is repeatedly
 *        calling to process various socket states.
 *
 * Results:
 *        None.
 *
 * Side effects:
 *        Calls Ns_TaskCallback and Ns_TaskDone to manage task state
 *
 *----------------------------------------------------------------------
 */

static void
HttpProc(
    Ns_Task *task,
    NS_SOCKET UNUSED(sock),
    void *arg,
    Ns_SockState why
) {
    NsHttpTask  *httpPtr;
    ssize_t      n = 0;
    bool         taskDone = NS_TRUE;
    Ns_SockState nextState;

    NS_NONNULL_ASSERT(task != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    httpPtr = (NsHttpTask *)arg;

    Ns_Log(Ns_LogTaskDebug, "HttpProc: enter socket state %.2x", why);

    switch (why) {
    case NS_SOCK_INIT:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_INIT timeout:%p", (void*)httpPtr->timeout);

        if (httpPtr->bodyChan != NULL) {
            HttpSpliceChannel(NULL, httpPtr->bodyChan);
        }
        if (httpPtr->spoolChan != NULL) {
            HttpSpliceChannel(NULL, httpPtr->spoolChan);
        }
        Ns_TaskCallback(task, NS_SOCK_WRITE, httpPtr->timeout);
        taskDone = NS_FALSE;

        break;

    case NS_SOCK_WRITE:

        nextState = why; /* We may switch to read state below */

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE sendSpoolMode:%d,"
               " fd:%d, chan:%s", httpPtr->sendSpoolMode, httpPtr->bodyFileFd,
               httpPtr->bodyChan ? Tcl_GetChannelName(httpPtr->bodyChan) : "(none)");

        if (httpPtr->sendSpoolMode == NS_FALSE) {
            size_t remain;

            /*
             * Send (next part of) the request from memory.
             * This may not include the request body, as it may have
             * to be spooled from the passed file or Tcl channel.
             * Decision whether to do this or not is done when we have
             * finished sending request line + all of the headers.
             */
            remain = (size_t)(httpPtr->requestLength - httpPtr->sent);

            Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE"
                   " will send dsPtr:%p, next:%p, remain:%" PRIuz,
                   (void*)httpPtr->ds.string, (void*)httpPtr->next, remain);

            if (remain > 0) {
                n = HttpTaskSend(httpPtr, httpPtr->next, remain);
            } else {
                n = 0;
            }

            if (n == -1) {
                httpPtr->error = "http send failed (initial send request)";
                Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE send failed");

            } else {
                ssize_t nb = 0;

                httpPtr->next += n;
                Ns_MutexLock(&httpPtr->lock);
                httpPtr->sent += (size_t)n;
                nb = (ssize_t)(httpPtr->sent - httpPtr->requestHeaderSize);
                if (nb > 0) {
                    httpPtr->sendBodySize = (size_t)nb;
                }
                Ns_MutexUnlock(&httpPtr->lock);
                remain = (size_t)(httpPtr->requestLength - httpPtr->sent);
                if (remain > 0) {

                    /*
                     * We still have something to be sent
                     * left in memory.
                     */
                    Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE"
                           " sent:%" PRIdz " bytes from memory,"
                           " remain:%" PRIuz, n, remain);
                } else {
                    const char *logMsg;

                    /*
                     * At this point we sent the line/headers
                     * and can now switch to sending the request
                     * body if any expected, or switch to the next
                     * socket state (read stuff from the remote).
                     */
                    logMsg = "HttpProc: NS_SOCK_WRITE headers sent";
                    httpPtr->next = NULL;
                    Tcl_DStringSetLength(&httpPtr->ds, 0);

                    if (httpPtr->bodyFileFd != NS_INVALID_FD) {
                        httpPtr->sendSpoolMode = NS_TRUE;
                        Ns_Log(Ns_LogTaskDebug, "%s, spool using fd:%d, size:%"
                               PRIuz, logMsg, httpPtr->bodyFileFd,
                               httpPtr->bodySize);

                    } else if (httpPtr->bodyChan != NULL) {
                        httpPtr->sendSpoolMode = NS_TRUE;
                        Ns_Log(Ns_LogTaskDebug, "%s, spool using chan:%s, size:%"
                               PRIuz, logMsg, Tcl_GetChannelName(httpPtr->bodyChan),
                               httpPtr->bodySize);

                    } else {
                        httpPtr->sendSpoolMode = NS_FALSE;
                        Ns_Log(Ns_LogTaskDebug, "%s, switch to read", logMsg);
                        nextState = NS_SOCK_READ;
                    }
                }

                taskDone = NS_FALSE;
            }

        } else {
            size_t toRead = CHUNK_SIZE;
            bool   onEof = NS_FALSE;

            /*
             * Send the request body from a file or from a Tcl channel.
             */

            Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE sendSpoolMode"
                   " buffersize:%" PRITcl_Size" buffer:%p next:%p sent:%" PRIuz,
                   httpPtr->ds.length, (void *)httpPtr->ds.string,
                   (void *)httpPtr->next, httpPtr->sent);

            if (httpPtr->next == NULL) {

                /*
                 * Read remaining body data in chunks
                 */
                Tcl_DStringSetLength(&httpPtr->ds, (TCL_SIZE_T)toRead);
                httpPtr->next = httpPtr->ds.string;
                if (toRead > httpPtr->bodySize) {
                    toRead = httpPtr->bodySize; /* At end of the body! */
                }
                if (toRead == 0) {
                    n = 0;
                } else if (httpPtr->bodyFileFd != NS_INVALID_FD) {
                    n = ns_read(httpPtr->bodyFileFd, httpPtr->next, toRead);
                } else if (httpPtr->bodyChan != NULL) {
                    n = (ssize_t)Tcl_Read(httpPtr->bodyChan, httpPtr->next, (TCL_SIZE_T)toRead);
                } else {
                    n = -1; /* Here we could read only from file or chan! */
                }

                if (toRead == 0 || (n > -1 && n < (ssize_t)toRead)) {

                    /*
                     * We have a short file/chan read which can only mean
                     * we are at the EOF (we are reading in blocking mode!).
                     */
                    onEof = NS_TRUE;
                    Tcl_DStringSetLength(&httpPtr->ds, (TCL_SIZE_T)n);
                }

                if (n > 0) {
                    assert((size_t)n <= httpPtr->bodySize);
                    httpPtr->bodySize -= (size_t)n;
                }

                Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE sendSpoolMode"
                       " got:%" PRIdz " wanted:%" PRIuz " bytes, eof:%d",
                       n, toRead, onEof);

            } else {

                /*
                 * The buffer has still some content left
                 */
                n = (ssize_t)(httpPtr->ds.length - (TCL_SIZE_T)(httpPtr->next - httpPtr->ds.string));

                Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE"
                       " remaining buffersize:%" PRIdz, n);
            }

            /*
             * We got some bytes from file/channel/memory
             * so send them to the remote.
             */

            if (unlikely(n == -1)) {
                httpPtr->error = "http read failed (initial data to send)";
                Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE read failed");

            } else {
                ssize_t toSend = n, sent = 0;

                if (toSend > 0) {
                    sent = HttpTaskSend(httpPtr, httpPtr->next, (size_t)toSend);
                }

                Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE sent"
                       " %" PRIdz " of %" PRIuz " bytes", sent, toSend);

                if (unlikely(sent == -1)) {
                    httpPtr->error = "http send failed (send request)";
                    Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE send failed");

                } else if (sent < toSend) {

                    /*
                     * We have sent less bytes than available
                     * in the buffer. At this point we may have
                     * sent zero bytes as well but this is very
                     * unlikely and would mean we were somehow
                     * wrongly signaled from the task handler
                     * (we wrote nothing to a writable sink?).
                     */

                    if (likely(sent > 0)) {
                        ssize_t nb = 0;

                        httpPtr->next += sent;
                        Ns_MutexLock(&httpPtr->lock);
                        httpPtr->sent += (size_t)sent;
                        nb = (ssize_t)(httpPtr->sent - httpPtr->requestHeaderSize);
                        if (nb > 0) {
                            httpPtr->sendBodySize = (size_t)nb;
                        }
                        Ns_MutexUnlock(&httpPtr->lock);
                    }
                    Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE partial"
                           " send, remain:%ld", (long)(toSend - sent));

                    taskDone = NS_FALSE;

                } else if (sent == toSend) {

                    /*
                     * We have sent the whole buffer
                     */
                    if (sent > 0) {
                        ssize_t nb = 0;

                        Ns_MutexLock(&httpPtr->lock);
                        httpPtr->sent += (size_t)sent;
                        nb = (ssize_t)(httpPtr->sent - httpPtr->requestHeaderSize);
                        if (nb > 0) {
                            httpPtr->sendBodySize = (size_t)nb;
                        }
                        Ns_MutexUnlock(&httpPtr->lock);
                        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE sent"
                               " full chunk, bytes:%" PRIdz, sent);
                    }

                    Tcl_DStringSetLength(&httpPtr->ds, 0);
                    httpPtr->next = NULL;

                    taskDone = NS_FALSE;

                    /*
                     * Check if on the last chunk,
                     * or on the premature EOF.
                     */
                    if (toRead < CHUNK_SIZE || onEof == NS_TRUE) {
                        if (httpPtr->bodySize == 0) {

                            /*
                             * That was the last chunk.
                             * All of the body was sent, switch state
                             */
                            Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE"
                                   " whole body sent, switch to read");
                            nextState = NS_SOCK_READ;

                        } else {

                            /*
                             * We read less than chunksize bytes, the source
                             * is on EOF, so what to do?  Since we can't
                             * rectify Content-Length, receiver expects us
                             * to send more...
                             * This situation can only happen:
                             * WHEN fed with the wrong (too large) bodySize
                             * OR when the file got truncated while we read it
                             * OR somebody tossed wrongly positioned channel.
                             * What can we do?
                             * We can pretend all is fine and go to reading
                             * state, expecting that either the peer's or
                             * our own timeout expires.
                             * Or, we can trigger the error immediately.
                             * We opt for the latter.
                             */
                            httpPtr->error = "http read failed (chunk data to send)";
                            taskDone = NS_TRUE;
                            Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_WRITE"
                                   " short read, left:%" PRIuz, httpPtr->bodySize);
                        }
                    }

                } else {

                    /*
                     * This is completely unexpected: we have send more
                     * then requested? There is something entirely wrong!
                     * I have no idea what would be the best to do here.
                     */
                    Ns_Log(Error, "HttpProc: NS_SOCK_WRITE bad state?");
                }
            }
        }

        /*
         * If the request is not finished, re-apply the timeout
         * for the next task iteration.
         */
        if (taskDone != NS_TRUE) {
            Ns_TaskCallback(task, nextState, httpPtr->timeout);
        }

        break;

    case NS_SOCK_READ:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_READ");

        nextState = why;

        if (httpPtr->sent == 0u) {
            Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_READ nothing sent?");

        } else {
            char         buf[CHUNK_SIZE];
            size_t       len = CHUNK_SIZE;
            Ns_SockState sockState = NS_SOCK_NONE;

            /*
             * FIXME:
             *
             * This part can be optimized to read the reply data
             * directly into DString instead in the stack buffer.
             */

            if (httpPtr->replyLength > 0) {
                size_t remain;

                remain = httpPtr->replyLength - httpPtr->replySize;
                if (len > remain)  {
                    len = remain;
                }
            }

            if (len > 0) {
                n = HttpTaskRecv(httpPtr, buf, len, &sockState);
            } else {
                n = 0;
            }

            if (unlikely(n == -1)) {

                /*
                 * Terminal case, some unexpected error.
                 * At this point we do not really know
                 * what kind of error it was.
                 */
                httpPtr->error = "http read failed (initial receive from server)";
                Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_READ receive failed");

            } else if (n > 0) {
                int result;

                /*
                 * Most likely case: we got some bytes.
                 */
                Ns_MutexLock(&httpPtr->lock);
                httpPtr->received += (size_t)n;
                Ns_MutexUnlock(&httpPtr->lock);

                result = HttpAppendContent(httpPtr, buf, (size_t)n);
                if (unlikely(result != TCL_OK)) {
                    httpPtr->error = "http read failed (chunk receive from server)";
                    Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_READ append failed");
                } else {
                    Ns_ReturnCode rc = NS_OK;

                  process_header:

                    if (httpPtr->replyHeaderSize == 0) {

                        /*
                         * Still not done receiving status/headers
                         */
                        HttpCheckHeader(httpPtr);
                    }

                    if (httpPtr->replyHeaderSize > 0 && httpPtr->status == 0) {
                        /*
                         * Parses received status/headers,
                         * decides where to spool content.
                         */
                        result = HttpCheckSpool(httpPtr);
                        if (result == TCL_CONTINUE) {
                            goto process_header;
                        }
                        rc = (result == TCL_OK ? NS_OK : NS_ERROR);
                    }
                    if (unlikely(rc != NS_OK)) {
                        httpPtr->error = "http read failed (check spool)";
                        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_READ spool failed");
                    } else {
                        /*
                         * At the point of reading response content (if any).
                         * Continue reading if any of the following is true:
                         *
                         *   - headers are not complete
                         *   - remote tells content length and it is not complete
                         *   - we received streaming HTML content (no content-length provided)
                         *   - chunked content not fully parsed
                         *   - caller tells it expects content
                         */
                        if (
                            ((httpPtr->flags & NS_HTTP_HEADERS_PENDING) != 0u)
                            || (httpPtr->replyLength > 0
                             && httpPtr->replySize < httpPtr->replyLength
                             && (httpPtr->flags & NS_HTTP_FLAG_EMPTY) == 0u)
                            || (httpPtr->flags & NS_HTTP_STREAMING) != 0u /* we rely on connection-close */
                            || ((httpPtr->flags & NS_HTTP_FLAG_CHUNKED) != 0u
                                && (httpPtr->flags & NS_HTTP_FLAG_CHUNKED_END) == 0u)
                            || ((httpPtr->flags & NS_HTTP_FLAG_CHUNKED) == 0u
                                && httpPtr->replyLength == 0
                                && httpPtr->replySize != 0
                                && (httpPtr->flags & NS_HTTP_FLAG_EMPTY) == 0u)
                        ) {
                            taskDone = NS_FALSE;
                        }
                        LogDebug("read ok", httpPtr, "");
                        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_READ httpPtr->replyLength %ld"
                               " httpPtr->replySize %ld flags %.6x %d %d %d %d -> done %d",
                               httpPtr->replyLength, httpPtr->replySize, httpPtr->flags,
                               (httpPtr->flags & NS_HTTP_STREAMING) != 0u,
                               (httpPtr->replyLength > 0
                                && httpPtr->replySize < httpPtr->replyLength
                                && (httpPtr->flags & NS_HTTP_FLAG_EMPTY) == 0u),
                               ((httpPtr->flags & NS_HTTP_FLAG_CHUNKED) != 0u
                                && (httpPtr->flags & NS_HTTP_FLAG_CHUNKED_END) == 0u),
                               ((httpPtr->flags & NS_HTTP_FLAG_CHUNKED) == 0u
                                && httpPtr->replyLength == 0
                                && httpPtr->replySize != 0
                                && (httpPtr->flags & NS_HTTP_FLAG_EMPTY) == 0u),
                               taskDone);
                    }
                }

            } else if (len > 0 && sockState == NS_SOCK_AGAIN) {

                /*
                 * Received zero bytes on a readable socket
                 * but it is not on EOD, it wants us to read more.
                 */
                taskDone = NS_FALSE;

            } else if (len == 0 /* Consumed all of the replyLength bytes */
                       || sockState == NS_SOCK_DONE /* EOD on read */
                       || ((httpPtr->flags & (NS_HTTP_FLAG_CHUNKED
                                              | NS_HTTP_FLAG_CHUNKED_END)) != 0u)) {

                taskDone = NS_TRUE; /* Just for illustrative purposes */

            } else {

                /*
                 * Some terminal error state
                 */
                Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_READ error,"
                       " sockState:%.2x", sockState);
            }
        }

        /*
         * If the request is not finished, re-apply the timeout
         * for the next task iteration.
         */
        if (taskDone != NS_TRUE) {
            Ns_TaskCallback(task, nextState, httpPtr->timeout);
        }

        break;

    case NS_SOCK_TIMEOUT:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_TIMEOUT");

        /*
         * Without a doneCallback, NS_SOCK_DONE must be handled
         * by the caller (normally, caller would cancel the task)
         * hence we leave the task in processing.
         *
         * With doneCallback, the caller is cut-off of the task ID
         * (i.e. there is no chance for cancel) hence we must mark
         * the task as completed (done) right here.
         */
        taskDone = (httpPtr->doneCallback != NULL);
        LogDebug("HttpProc: NS_SOCK_TIMEOUT", httpPtr, "");
        //Ns_TaskCancel(httpPtr->task);
        //taskDone = NS_FALSE;

        httpPtr->error = "http request timeout";

        break;

    case NS_SOCK_EXIT:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_EXIT");
        httpPtr->error = "http task queue shutdown";

        break;

    case NS_SOCK_CANCEL:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_CANCEL");
        httpPtr->error = "http request cancelled";

        break;

    case NS_SOCK_EXCEPTION:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_EXCEPTION");
        httpPtr->error = "unexpected http socket exception";

        break;

    case NS_SOCK_AGAIN:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_AGAIN");
        httpPtr->error = "unexpected http EOD";

        break;

    case NS_SOCK_DONE:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_DONE doneCallback:(%s)",
               httpPtr->doneCallback != NULL ? httpPtr->doneCallback : "none");

        if (httpPtr->bodyChan != NULL) {
            HttpCutChannel(NULL, httpPtr->bodyChan);
        }
        if (httpPtr->spoolChan != NULL) {
            HttpCutChannel(NULL, httpPtr->spoolChan);
        }
        if (httpPtr->doneCallback != NULL) {
            HttpDoneCallback(httpPtr); /* Does free on the httpPtr */
            httpPtr = NULL;
        }

        break;

    case NS_SOCK_NONE:

        Ns_Log(Ns_LogTaskDebug, "HttpProc: NS_SOCK_NONE");
        httpPtr->error = "unexpected http socket state";

        break;
    }

    if (httpPtr != NULL) {
        httpPtr->finalSockState = why;
        LogDebug("HttpProc: exit", httpPtr, taskDone ? "done" : "not done");
        if (taskDone == NS_TRUE) {
            Ns_GetTime(&httpPtr->etime);
            Ns_TaskDone(httpPtr->task);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * HttpSpliceChannels --
 *
 *        Convenience wrapper to splice-in body/spool channels
 *        in the given interp.
 *
 * Results:
 *        None.
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static void
HttpSpliceChannels(
    Tcl_Interp *interp,
    NsHttpTask *httpPtr
) {
    NS_NONNULL_ASSERT(httpPtr != NULL);

    if (httpPtr->bodyChan != NULL) {
        HttpSpliceChannel(interp, httpPtr->bodyChan);
        httpPtr->bodyChan = NULL;
    }
    if (httpPtr->spoolChan != NULL) {
        HttpSpliceChannel(interp, httpPtr->spoolChan);
        httpPtr->spoolChan = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * HttpSpliceChannel --
 *
 *        Splice-in the channel in the given interp.
 *
 * Results:
 *        Nothing.
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static void
HttpSpliceChannel(
    Tcl_Interp *interp,
    Tcl_Channel chan
) {
    NS_NONNULL_ASSERT(chan != NULL);

    Tcl_SpliceChannel(chan);

    if (interp != NULL) {
        Tcl_RegisterChannel(interp, chan);
        Tcl_UnregisterChannel((Tcl_Interp *)NULL, chan);
    }

    Ns_Log(Ns_LogTaskDebug, "HttpSpliceChannel: interp:%p chan:%s",
           (void *)interp, Tcl_GetChannelName(chan));
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCutChannel --
 *
 *        Wrapper to cut-out the given channel from the interp/thread.
 *
 * Results:
 *        Standard Tcl result.
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
HttpCutChannel(
    Tcl_Interp *interp,
    Tcl_Channel chan
) {
    int result = TCL_OK;

    NS_NONNULL_ASSERT(chan != NULL);

    if (interp != NULL) {
        if (Tcl_IsChannelShared(chan)) {
            const char *errorMsg = "channel is shared";

            Tcl_SetResult(interp, (char*)errorMsg, TCL_STATIC);
            result = TCL_ERROR;
        } else {
            Tcl_DriverWatchProc *watchProc;

            /*
             * This effectively disables processing of pending
             * events which are ready to fire for the given
             * channel. If we do not do this, events will hit
             * the detached channel which is potentially being
             * owned by some other thread. This will wreck havoc
             * on our memory and eventually badly hurt us...
             */
            Tcl_ClearChannelHandlers(chan);
            watchProc = Tcl_ChannelWatchProc(Tcl_GetChannelType(chan));
            if (watchProc != NULL) {
                (*watchProc)(Tcl_GetChannelInstanceData(chan), 0);
            }

            /*
             * Artificially bump the channel reference count
             * which protects us from channel being closed
             * during the Tcl_UnregisterChannel().
             */
            Tcl_RegisterChannel((Tcl_Interp *) NULL, chan);
            Tcl_UnregisterChannel(interp, chan);
        }
    }

    if (result == TCL_OK) {
        Ns_Log(Ns_LogTaskDebug, "HttpCutChannel: interp:%p chan:%s",
               (void *)interp, Tcl_GetChannelName(chan));
        Tcl_CutChannel(chan);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpTunnel --
 *
 *        Dig a tunnel to the remote host over the given proxy.
 *
 * Results:
 *        Socket tunneled to the remote host/port.
 *        Should behave as a regular directly connected socket.
 *
 * Side effects:
 *        Runs an HTTP task for HTTP/1.1 connection to proxy.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
HttpTunnel(
    NsInterp *itPtr,
    const char *proxyhost,
    unsigned short proxyport,
    const char *host,
    unsigned short port,
    const Ns_Time *timeout
) {
    NsHttpTask *httpPtr;
    Ns_DString *dsPtr;
    Tcl_Interp *interp;
    NS_SOCKET   result = NS_INVALID_SOCKET;
    const char *url = "proxy-tunnel"; /* Not relevant; for logging purposes only */
    uint64_t    requestCount = 0u;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(proxyhost != NULL);
    NS_NONNULL_ASSERT(host != NULL);

    assert(proxyport > 0);
    assert(port > 0);
    /*
     * Setup the task structure. From this point on
     * if something goes wrong, we must HttpClose().
     */
    httpPtr = ns_calloc(1u, sizeof(NsHttpTask));
    httpPtr->chunk = ns_calloc(1u, sizeof(NsHttpChunk));
    httpPtr->bodyFileFd = NS_INVALID_FD;
    httpPtr->spoolFd = NS_INVALID_FD;
    httpPtr->sock = NS_INVALID_SOCKET;
    httpPtr->spoolLimit = -1;
    httpPtr->url = ns_strdup(url);
    httpPtr->flags |= NS_HTTP_FLAG_EMPTY; /* Do not expect response content */
    httpPtr->method = ns_strdup(connectMethod);
    httpPtr->replyHeaders = Ns_SetCreate(NS_SET_NAME_CLIENT_RESPONSE); /* Ignored */
    httpPtr->servPtr = itPtr->servPtr;

    if (timeout != NULL) {
        httpPtr->timeout = ns_calloc(1u, sizeof(Ns_Time));
        *httpPtr->timeout = *timeout;
    }

    Ns_GetTime(&httpPtr->stime);

    interp = itPtr->interp;
    dsPtr = &httpPtr->ds;
    Ns_DStringInit(&httpPtr->ds);
    Ns_DStringInit(&httpPtr->chunk->ds);

    Ns_MasterLock();
    requestCount = ++httpClientRequestCount;
    Ns_MasterUnlock();

    Ns_MutexInit(&httpPtr->lock);
    (void)ns_uint64toa(dsPtr->string, requestCount);
    Ns_MutexSetName2(&httpPtr->lock, "ns:httptask", dsPtr->string);

    /*
     * Now we are ready to attempt the connection.
     * If no timeout given, assume 10 seconds.
     */

    {
        Ns_ReturnCode rc;
        Ns_Time       def = {10, 0}, *toPtr = NULL;

        Ns_Log(Ns_LogTaskDebug, "HttpTunnel: connecting to proxy [%s]:%hu",
               proxyhost, proxyport);

        toPtr = (httpPtr->timeout != NULL) ? httpPtr->timeout : &def;
        httpPtr->sock = Ns_SockTimedConnect2(proxyhost, proxyport, NULL, 0, toPtr, &rc);
        if (httpPtr->sock == NS_INVALID_SOCKET) {
            Ns_SockConnectError(interp, proxyhost, proxyport, rc);
            if (rc == NS_TIMEOUT) {
                HttpClientLogWrite(httpPtr, "connecttimeout");
                Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
            }
            goto fail;
        }
        if (Ns_SockSetNonBlocking(httpPtr->sock) != NS_OK) {
            Ns_TclPrintfResult(interp, "can't set socket nonblocking mode");
            goto fail;
        }
        rc = HttpWaitForSocketEvent(httpPtr->sock, POLLOUT, httpPtr->timeout);
        if (rc != NS_OK) {
            if (rc == NS_TIMEOUT) {
                Ns_TclPrintfResult(interp, "timeout waiting for writable socket");
                HttpClientLogWrite(httpPtr, "writetimeout");
                Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
            } else {
                Ns_TclPrintfResult(interp, "waiting for writable socket: %s",
                                   ns_sockstrerror(ns_sockerrno));
            }
            goto fail;
        }
    }

    /*
     * At this point we are connected.
     * Construct CONNECT request line.
     */
    Ns_DStringSetLength(dsPtr, 0);
    Ns_DStringPrintf(dsPtr, "%s %s:%d HTTP/1.1\r\n", httpPtr->method, host, port);
    Ns_DStringPrintf(dsPtr, "%s: %s:%d\r\n", hostHeader, host, port);
    Ns_DStringNAppend(dsPtr, "\r\n", 2);

    httpPtr->requestLength = (size_t)dsPtr->length;
    httpPtr->next = dsPtr->string;

    /*
     * Run the task, on success hijack the socket.
     */
    httpPtr->task = Ns_TaskCreate(httpPtr->sock, HttpProc, httpPtr);
    Ns_TaskRun(httpPtr->task);
    if (httpPtr->status == 200) {
        result = httpPtr->sock;
        httpPtr->sock = NS_INVALID_SOCKET;
    } else {
        Ns_TclPrintfResult(interp, "can't open http tunnel, response status: %d",
                           httpPtr->status);
    }

fail:
    HttpClose(httpPtr);
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseCRProc --
 *
 *        Handler for chunked-encoding state machine that parses
 *        the chunk framing element CR.
 *
 * Results:
 *        TCL_OK: CR element parsed OK
 *        TCL_ERROR: error in chunked format
 *        TCL_BREAK; not enough data (stop parsing but remain in state)
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
ParseCRProc(
    NsHttpTask *UNUSED(httpPtr),
    char **buffer,
    size_t *size
) {
    char  *buf = *buffer;
    int    result = TCL_OK;
    size_t len = *size;

    Ns_Log(Ns_LogTaskDebug, "--- ParseCRProc char %c len %lu", *buf, len);

    if (len == 0) {
        result = TCL_BREAK;
    } else if (*(buf) == '\r') {
        len--;
        buf++;
    } else {
        result = TCL_ERROR;
    }

    if (result != TCL_ERROR) {
        *buffer = buf;
        *size = len;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseLFProc
 *
 *        Handler for chunked-encoding state machine that parses
 *        the chunk framing element LF.
 *
 * Results:
 *        TCL_OK: CR element parsed OK
 *        TCL_ERROR: error in chunked format
 *        TCL_BREAK; not enough data (stop parsing but remain in state)
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
ParseLFProc(
    NsHttpTask *UNUSED(httpPtr),
    char **buffer,
    size_t *size
) {
    char  *buf  = *buffer;
    int    result = TCL_OK;
    size_t len = *size;

    Ns_Log(Ns_LogTaskDebug, "--- ParseLFProc");

    if (len == 0) {
        result = TCL_BREAK;
    } else if (*(buf) == '\n') {
        len--;
        buf++;
    } else {
        result = TCL_ERROR;
    }

    if (result != TCL_ERROR) {
        *buffer = buf;
        *size = len;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseLengthProc --
 *
 *        Handler for chunked-encoding state machine that parses
 *        the chunk length/size element.
 *
 * Results:
 *        TCL_OK: size element parsed OK
 *        TCL_ERROR: error in chunked format
 *        TCL_BREAK; not enough data (stop parsing but remain in state)
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
ParseLengthProc(
    NsHttpTask *httpPtr,
    char **buffer,
    size_t *size
) {
    char        *buf = *buffer;
    int          result = TCL_OK;
    size_t       len = *size;
    NsHttpChunk *chunkPtr = httpPtr->chunk;
    Tcl_DString *dsPtr = &chunkPtr->ds;

    /*
     * Collect all that looks as a hex digit
     */
    while (len > 0 && *(buf) > 0 && isxdigit(*(buf))) {
        Tcl_DStringAppend(dsPtr, buf, 1);
        len--;
        buf++;
    }
    Ns_Log(Ns_LogTaskDebug, "--- ParseLengthProc hex digits <%s>", dsPtr->string);

    if (len == 0) {
        result = TCL_BREAK;
    } else {
        Tcl_WideInt cl = 0;

        if (Ns_StrToWideInt(dsPtr->string, &cl) != NS_OK || cl < 0) {
            result = TCL_ERROR;
        } else {
            chunkPtr->length = (size_t)cl;

            /*
             * According to the RFC, the chunk size may be followed
             * by a variable number of chunk extensions, separated
             * by a semicolon, up to the terminating frame delimiter.
             * For the time being, we simply discard extensions.
             * We might possibly declare a special parser proc for this.
             */
            while (len > 0 && *(buf) > 0 && *(buf) != '\r') {
                len--;
                buf++;
            }
        }
    }

    if (result != TCL_ERROR) {
        *buffer = buf;
        *size = len;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseBodyProc --
 *
 *        Handler for chunked-encoding state machine that parses
 *        the chunk body.
 *
 * Results:
 *        TCL_OK: body parsed OK
 *        TCL_ERROR: error in chunked format
 *        TCL_BREAK: stop/reset state machine (no data or on last chunk)
 *
 * Side effects:
 *        May change state of the parsing state machine.
 *
 *----------------------------------------------------------------------
 */

static int
ParseBodyProc(
    NsHttpTask *httpPtr,
    char **buffer,
    size_t *size
) {
    char        *buf = *buffer;
    int          result = TCL_OK;
    size_t       len = *size;
    NsHttpChunk *chunkPtr = httpPtr->chunk;

    Ns_Log(Ns_LogTaskDebug, "--- ParseBodyProc chunk length %ld", chunkPtr->length);

    if (chunkPtr->length == 0) {
        Ns_Set     *headersPtr;
        const char *trailer;

        /*
         * We are on the last chunk. Check if we will get some
         * trailers and switch the state accordingly.
         */
        headersPtr = httpPtr->replyHeaders;
        trailer = Ns_SetIGet(headersPtr, trailersHeader);
        if (trailer != NULL) {
            Ns_Log(Ns_LogTaskDebug, "... switch to trailer parsers");
            chunkPtr->parsers = TrailerParsers;
        } else {
            Ns_Log(Ns_LogTaskDebug, "... switch to end parsers");
            chunkPtr->parsers = EndParsers;
        }

        chunkPtr->callx = 0;
        result = TCL_BREAK;

    } else if (len == 0) {
        result = TCL_BREAK;
    } else {
        size_t remain, append;

        remain = chunkPtr->length - chunkPtr->got;
        append = remain < len ? remain : len;

        if (append > 0) {
            HttpAppendBuffer(httpPtr, (const char*)buf, append);
            chunkPtr->got += append;
            len -= append;
            buf += append;
            remain -= append;
        }

        if (remain > 0 && len == 0) {

            /*
             * Not enough data in the passed buffer to
             * consume whole chunk, break state parsing
             * but remain in the current state and go
             * and get new blocks from the source.
             */
            result = TCL_BREAK;
        }
    }

    if (result != TCL_ERROR) {
        *buffer = buf;
        *size = len;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseTrailerProc --
 *
 *        Handler for chunked-encoding state machine that parses
 *        optional trailers. Trailers look like regular headers
 *        (string)(crlf).
 *
 *
 * Results:
 *        TCL_OK: trailer parsed OK
 *        TCL_ERROR: error in chunked format
 *        TCL_BREAK: stop state machine, last trailer encountered
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
ParseTrailerProc(
    NsHttpTask *httpPtr,
    char **buffer,
    size_t *size
) {
    char        *buf = *buffer;
    int          result = TCL_OK;
    size_t       len = *size;
    NsHttpChunk *chunkPtr = httpPtr->chunk;
    Tcl_DString *dsPtr = &chunkPtr->ds;

    while (len > 0 && *(buf) > 0 && *(buf) != '\r') {
        Tcl_DStringAppend(dsPtr, buf, 1);
        len--;
        buf++;
    }

    if (len == 0) {
        result = TCL_BREAK;
    } else if (*(buf) == '\r') {
        if (dsPtr->length == 0) {

            /*
             * This was the last header (== o header, zero-size)
             */
            chunkPtr->parsers = EndParsers;
            chunkPtr->callx = 0;
            result = TCL_BREAK;
        } else {
            Ns_Set     *headersPtr = httpPtr->replyHeaders;
            const char *trailer = dsPtr->string;

            if (Ns_ParseHeader(headersPtr, trailer, NULL, ToLower, NULL) != NS_OK) {
                result = TCL_ERROR;
            }
        }
    } else {
        result = TCL_ERROR;
    }

    if (result != TCL_ERROR) {
        *buffer = buf;
        *size = len;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseEndProc --
 *
 *        Handler for chunked-encoding state machine that terminates
 *        chunk parsing state.
 *
 * Results:
 *        TCL_BREAK
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
ParseEndProc(
    NsHttpTask *httpPtr,
    char **UNUSED(buffer),
    size_t *size
) {
    Ns_Log(Ns_LogTaskDebug, "--- ParseEndProc");

    *size = 0;
    httpPtr->flags |= NS_HTTP_FLAG_CHUNKED_END;

    return TCL_BREAK;
}


/*
 *----------------------------------------------------------------------
 *
 * ChunkInitProc --
 *
 *        Handler for chunked-encoding state machine that initializes
 *        chunk parsing state.
 *
 * Results:
 *        TCL_OK
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
ChunkInitProc(
    NsHttpTask *httpPtr,
    char **UNUSED(buffer),
    size_t *UNUSED(size)
) {
    NsHttpChunk *chunkPtr = httpPtr->chunk;
    Tcl_DString *dsPtr = &chunkPtr->ds;

    Ns_Log(Ns_LogTaskDebug, "--- ChunkInitProc");

    chunkPtr->length = 0;
    chunkPtr->got = 0;
    Tcl_DStringSetLength(dsPtr, 0);
    Tcl_DStringAppend(dsPtr, "0x", 2);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * TrailerInitProc --
 *
 *        Handler for chunked-encoding state machine that initializes
 *        trailers parsing
 *
 * Results:
 *        TCL_OK always
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

static int
TrailerInitProc(
    NsHttpTask *httpPtr,
    char **UNUSED(buffer),
    size_t *UNUSED(size)
) {
    NsHttpChunk *chunkPtr = httpPtr->chunk;
    Tcl_DString *dsPtr = &chunkPtr->ds;

    Tcl_DStringSetLength(dsPtr, 0);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PersistentConnectionLookup --
 *
 *        Check, if for the connection key (host + port) an already open
 *        connection exists in the form of a task in theclose wait queue.  On
 *        success, delete the connection entry and return it to the
 *        caller. This prevents double-reuses.
 *
 * Results:
 *        Boolean value indicating success.
 *
 * Side effects:
 *        Potentially free up memory.

 *----------------------------------------------------------------------
 */
static bool
PersistentConnectionLookup(NsHttpTask *httpPtr, NsHttpTask **waitingHttpPtrPtr)
{
    Tcl_HashEntry *hPtr;
    NsServer      *servPtr = httpPtr->servPtr;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(waitingHttpPtrPtr != NULL);

    if (unlikely(httpPtr->servPtr == NULL)) {
        hPtr = NULL;
    } else {
        assert(httpPtr->persistentKey != NULL);

        Ns_MutexLock(&servPtr->httpclient.lock);
        hPtr = Tcl_FindHashEntry(&servPtr->httpclient.pconns, httpPtr->persistentKey);
        if (hPtr != NULL) {
            NsHttpTask *waitingHttpPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);

            if (likely(waitingHttpPtr->closeWaitTask != NULL)) {
                Ns_Log(Ns_LogTaskDebug, "Forcing cancel on wait task %p", (void*)waitingHttpPtr->closeWaitTask);
                Ns_TaskCancel(waitingHttpPtr->closeWaitTask);
                waitingHttpPtr->closeWaitTask = NULL;
                /*Ns_Log(Notice, "persistent key %s has already a waiting structure"
                  " with an assigend task (forced cancel)", httpPtr->persistentKey);*/
            } else {
                /* TODO: should be removed before the release */
                Ns_Log(Notice, "========================= persistent key %s has a waiting structure"
                       " without an assigend task", httpPtr->persistentKey);
            }
            /*
             * Delete the entry which is to be reused. This prevents concurrent
             * double reuse.
             */
            Tcl_DeleteHashEntry(hPtr);
            *waitingHttpPtrPtr = waitingHttpPtr;

        }
        Ns_MutexUnlock(&servPtr->httpclient.lock);
    }

    return (hPtr != NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * PersistentConnectionDelete --
 *
 *        Delete potentially the connection key from the lookup table.
 *
 * Results:
 *        Boolean value indicating success.
 *
 * Side effects:
 *        Potentially free up memory.

 *----------------------------------------------------------------------
 */
static bool
PersistentConnectionDelete(NsHttpTask *httpPtr)
{
    Tcl_HashEntry *hPtr;
    NsServer      *servPtr;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    servPtr = httpPtr->servPtr;
    if (unlikely(servPtr == NULL)) {
        hPtr = NULL;
    } else {
        /*
         * Make sure to delete the persistent connection entry, don't care about
         * the rest.
         */
        assert(httpPtr->persistentKey != NULL);

        Ns_MutexLock(&servPtr->httpclient.lock);
        hPtr = Tcl_FindHashEntry(&servPtr->httpclient.pconns, httpPtr->persistentKey);
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&servPtr->httpclient.lock);
    }
    return (hPtr != NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * PersistentConnectionAdd --
 *
 *        Add the persistent connection keys to the lookup table.
 *
 * Results:
 *        Boolean value indicating this is a fresh entry.
 *
 * Side effects:
 *        Adding an entry to the lookup table.

 *----------------------------------------------------------------------
 */
static bool
PersistentConnectionAdd(NsHttpTask *httpPtr, const char **reasonPtr)
{
    NsServer *servPtr = httpPtr->servPtr;
    int       isNew = 0;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    if (likely(httpPtr->servPtr != NULL)) {
        if (likely(httpPtr->persistentKey != NULL)) {
            Tcl_HashEntry *hPtr;

            Ns_MutexLock(&servPtr->httpclient.lock);
            hPtr = Tcl_CreateHashEntry(&servPtr->httpclient.pconns, httpPtr->persistentKey, &isNew);

            if (likely(isNew != 0)) {
                Tcl_SetHashValue(hPtr, httpPtr);
            } else {
                *reasonPtr = "duplicate persistent key";
            }
            Ns_MutexUnlock(&servPtr->httpclient.lock);
        } else {
            Ns_Log(Error, "PersistentConnectionAdd: persistent key of %p already deleted."
                   " Code must be debugged with Debug(task) turned on and sent to the"
                   " NaviServer developers", (void*)httpPtr);
            *reasonPtr = "key deleted";
        }
    } else {
        *reasonPtr = "no server defined for this task";
    }
    return (isNew != 0);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * eval: (c-guess)
 * End:
 */
