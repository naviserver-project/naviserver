/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
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
 * tclhttp.c --
 *
 *      Support for the ns_http command.
 *      Uses the Ns_Task interface to run/queue HTTP tasks.
 */

#include "nsd.h"

#ifdef HAVE_OPENSSL_EVP_H
#include <openssl/err.h>
#endif

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
 * String equivalents of some header keys
 */
static const char *transferEncodingHeader = "Transfer-Encoding";
static const char *contentEncodingHeader  = "Content-Encoding";
static const char *contentTypeHeader      = "Content-Type";
static const char *contentLengthHeader    = "Content-Length";
static const char *connectionHeader       = "Connection";
static const char *trailersHeader         = "Trailers";
static const char *hostHeader             = "Host";
static const char *userAgentHeader        = "User-Agent";

/*
 * Attempt to maintain Tcl errorCode variable.
 * This is still not done thoroughly through the code.
 */
static const char *errorCodeTimeoutString = "NS_TIMEOUT";

/*
 * Local functions defined in this file
 */

static int HttpQueue(
    NsInterp *itPtr,
    int objc,
    Tcl_Obj *const*
    objv,
    bool run
) NS_GNUC_NONNULL(1);

static int HttpConnect(
    Tcl_Interp *interp,
    const char *method,
    const char *url,
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
    NsHttpTask **httpPtrPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(16);

static bool HttpGet(
    NsInterp *itPtr,
    const char *taskID,
    NsHttpTask **httpPtrPtr,
    bool remove
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void HttpClose(
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

static Ns_ReturnCode HttpWaitForSocketEvent(
    NS_SOCKET sock,
    short events,
    Ns_Time *timeout
);

static void HttpAddInfo(
    NsHttpTask *httpPtr,
    const char *key,
    const char *value
)  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(2);

static void HttpCheckHeader(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static Ns_ReturnCode HttpCheckSpool(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static ssize_t HttpTaskSend(
    NsHttpTask *httpPtr,
    const void *buffer,
    size_t length
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static ssize_t HttpTaskRecv(
    NsHttpTask *httpPtr,
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
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int HttpGetResult(
    Tcl_Interp *interp,
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void HttpDoneCallback(
    NsHttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static Ns_TaskProc HttpProc;

/*
 * Function implementing the Tcl interface.
 */
static Tcl_ObjCmdProc HttpCancelObjCmd;
static Tcl_ObjCmdProc HttpCleanupObjCmd;
static Tcl_ObjCmdProc HttpListObjCmd;
static Tcl_ObjCmdProc HttpStatsObjCmd;
static Tcl_ObjCmdProc HttpQueueObjCmd;
static Tcl_ObjCmdProc HttpRunObjCmd;
static Tcl_ObjCmdProc HttpWaitObjCmd;

static NsHttpParseProc ParseCRProc;
static NsHttpParseProc ParseLFProc;
static NsHttpParseProc ParseLengthProc;
static NsHttpParseProc ChunkInitProc;
static NsHttpParseProc ParseBodyProc;
static NsHttpParseProc TrailerInitProc;
static NsHttpParseProc ParseTrailerProc;
static NsHttpParseProc ParseEndProc;

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


/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpParseHost --
 *
 *      Obtain the host name from a writable string
 *      using syntax as specified in RFC 3986 section 3.2.2.
 *
 *      Examples:
 *
 *          [2001:db8:1f70::999:de8:7648:6e8]:8000 (IP-literal notation)
 *          openacs.org:80                         (reg-name notation)
 *
 * Results:
 *      If a port is indicated after the host name, the "portStart"
 *      will contain a string starting with ":", otherwise NULL.
 *
 *      If "hostStart" is non-null, a pointer will point to the host name,
 *      which will be terminated by '\0' in case of a IPv6 address in
 *      IP-literal notation.
 *
 * Side effects:
 *      May write a '\0' into the passed hostSting.
 *
 *----------------------------------------------------------------------
 */

void
Ns_HttpParseHost(
    char *hostString,
    char **hostStart,
    char **portStart
) {
    bool ipLiteral = NS_FALSE;

    NS_NONNULL_ASSERT(hostString != NULL);
    NS_NONNULL_ASSERT(portStart != NULL);

    if (*hostString == '[') {
        char *p;

        /*
         * Maybe this is an address in IP-literal notation in square braces
         */
        p = strchr(hostString + 1, INTCHAR(']'));
        if (p != NULL) {
            ipLiteral = NS_TRUE;

            /*
             * Terminate the IP-literal if hostStart is given.
             */
            if (hostStart != NULL) {
                *p = '\0';
                *hostStart = hostString + 1;
            }
            p++;
            if (*p == ':') {
                *portStart = p;
            } else {
                *portStart = NULL;
            }
        }
    }
    if (ipLiteral == NS_FALSE) {
        char *slash = strchr(hostString, INTCHAR('/')),
             *colon = strchr(hostString, INTCHAR(':'));
        if (slash != NULL && colon != NULL && slash < colon) {

            /*
             * Found a colon after the first slash, ignore this colon.
             */
            *portStart = NULL;
        } else {
            *portStart = colon;
        }
        if (hostStart != NULL) {
            *hostStart = hostString;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpLocationString --
 *
 *      Build a HTTP location string following the IP literation
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
 *      Parse a HTTP message (response line and headers).
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
                if (hdrPtr->name != NULL) {
                    ns_free((void *)hdrPtr->name);
                }
                hdrPtr->name = ns_strdup(p);
                firsthdr = 0;
            } else if (len < 2 || Ns_ParseHeader(hdrPtr, p, ToLower) != NS_OK) {
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
 *      Implements the [ns_http] command for handling HTTP requests.
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
    int objc,
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
 *      Implements the [ns_http run] command
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
    int objc,
    Tcl_Obj *const* objv
) {
    return HttpQueue(clientData, objc, objv, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpQueueObjCmd
 *
 *      Implements the [ns_http queue] command
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
    int objc,
    Tcl_Obj *const* objv
) {
    return HttpQueue(clientData, objc, objv, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * HttpWaitObjCmd --
 *
 *      Implements [ns_http wait] command.
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
    int         objc,
    Tcl_Obj    *const* objv
) {
    NsInterp   *itPtr = clientData;
    NsHttpTask *httpPtr = NULL;

    char       *id = NULL, *outputFileName = NULL;
    int         result = TCL_OK, decompress = 0, spoolLimit = -1;

    Tcl_Obj    *elapsedVarObj = NULL,
               *resultVarObj = NULL,
               *statusVarObj = NULL,
               *fileVarObj = NULL;

    Ns_Set     *replyHeaders = NULL;
    Ns_Time    *timeoutPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-elapsed",    Ns_ObjvObj,     &elapsedVarObj,   NULL},
        {"-result",     Ns_ObjvObj,     &resultVarObj,    NULL},
        {"-status",     Ns_ObjvObj,     &statusVarObj,    NULL},
        {"-file",       Ns_ObjvObj,     &fileVarObj,      NULL},
        {"-timeout",    Ns_ObjvTime,    &timeoutPtr,      NULL},
        {"-headers",    Ns_ObjvSet,     &replyHeaders,    NULL},
        {"-outputfile", Ns_ObjvString,  &outputFileName,  NULL},
        {"-spoolsize",  Ns_ObjvMemUnit, &spoolLimit,      NULL},
        {"-decompress", Ns_ObjvBool,    &decompress,      INT2PTR(NS_TRUE)},
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
            Ns_Log(Warning, "ns_http_wait: -decompress option is deprecated");
            httpPtr->flags |= NS_HTTP_FLAG_DECOMPRESS;
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

        if (likely(rc == NS_OK)) {
            result = HttpGetResult(interp, httpPtr);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(httpPtr->error, -1));
            if (rc == NS_TIMEOUT) {
                Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
            }
            HttpCancel(httpPtr);
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
                Ns_Set  *headers = NULL;
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
 *      Implements [ns_http cancel] command.
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
    int         objc,
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
 *      Implements the [ns_http cleanup] command
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
    int         objc,
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

            NsHttpTask *httpPtr = NULL;
            char       *taskName = NULL;

            httpPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);
            NS_NONNULL_ASSERT(httpPtr != NULL);

            taskName = Tcl_GetHashKey(&itPtr->httpRequests, hPtr);

            Ns_Log(Ns_LogTaskDebug, "HttpCleanup cleans task %s (doneCB <%s>)",
                   taskName,
                   (httpPtr->doneCallback != NULL) ? httpPtr->doneCallback : "");

            if (httpPtr->doneCallback != NULL) {

                /*
                 * The callback should be doing all of the
                 * necessary cleanup, including closing
                 * the registered channels and finally
                 * garbage-collecting the task.
                 * So for now, just initiate the cancel
                 * and let the callback do the rest.
                 */
                HttpCancel(httpPtr);

            } else {

                Ns_Log(Warning, "HttpCleanup: cancel task %s", taskName);

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
            }

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
 *      Implements the [ns_http list] command
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
    int         objc,
    Tcl_Obj    *const* objv
) {
    NsInterp      *itPtr = clientData;
    char          *idString = NULL;
    int            result = TCL_OK;
    Tcl_Obj       *resultObj = NULL;
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

        char *taskString = NULL;

        taskString = Tcl_GetHashKey(&itPtr->httpRequests, hPtr);

        if (idString == NULL || STREQ(taskString, idString)) {
            const char *taskState = NULL;
            NsHttpTask *httpPtr = NULL;

            httpPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);
            NS_NONNULL_ASSERT(httpPtr != NULL);

            if (Ns_TaskCompleted(httpPtr->task) == NS_TRUE) {
                taskState = "done";
            } else if (httpPtr->error != NULL) {
                taskState = "error";
            } else {
                taskState = "running";
            }

            Tcl_ListObjAppendElement
                (interp, resultObj, Tcl_NewStringObj(taskString, -1));

            Tcl_ListObjAppendElement
                (interp, resultObj, Tcl_NewStringObj(httpPtr->url, -1));

            Tcl_ListObjAppendElement
                (interp, resultObj, Tcl_NewStringObj(taskState, -1));
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
 *      Implements the [ns_http stats] command.
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
    int         objc,
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
                 Tcl_NewStringObj(taskString, -1));

            (void) Tcl_DictObjPut
                (interp, entryObj, Tcl_NewStringObj("url", 3),
                 Tcl_NewStringObj(httpPtr->url, -1));

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
 * HttpQueue --
 *
 *      Enqueues the HTTP task and optionally returns the taskID
 *      in the interp result. This ID can be used by other
 *      commands to cancel or wait for the task to finish.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May queue an HTTP request.
 *      The tasID is not returned if the -doneCallback" option
 *      is specified. In that case, the task is finished and
 *      garbage collected by the thread executing the task.
 *
 *----------------------------------------------------------------------
 */

static int
HttpQueue(
    NsInterp *itPtr,
    int objc,
    Tcl_Obj *const* objv,
    bool run
) {
    Tcl_Interp *interp;
    int         result = TCL_OK, decompress = 0, spoolLimit = -1;
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
    Tcl_Obj    *bodyObj = NULL;
    Ns_Time    *timeoutPtr = NULL;
    Ns_Time    *expirePtr = NULL;
    Tcl_WideInt bodySize = 0;
    Tcl_Channel bodyChan = NULL, spoolChan = NULL;
    Ns_ObjvValueRange sizeRange = {0, LLONG_MAX};

    Ns_ObjvSpec opts[] = {
        {"-body",             Ns_ObjvObj,     &bodyObj,        NULL},
        {"-body_size",        Ns_ObjvWideInt, &bodySize,       &sizeRange},
        {"-body_file",        Ns_ObjvString,  &bodyFileName,   NULL},
        {"-body_chan",        Ns_ObjvString,  &bodyChanName,   NULL},
        {"-cafile",           Ns_ObjvString,  &caFile,         NULL},
        {"-capath",           Ns_ObjvString,  &caPath,         NULL},
        {"-cert",             Ns_ObjvString,  &cert,           NULL},
        {"-decompress",       Ns_ObjvBool,    &decompress,     INT2PTR(NS_TRUE)},
        {"-donecallback",     Ns_ObjvString,  &doneCallback,   NULL},
        {"-headers",          Ns_ObjvSet,     &requestHdrPtr,  NULL},
        {"-hostname",         Ns_ObjvString,  &sniHostname,    NULL},
        {"-keep_host_header", Ns_ObjvBool,    &keepHostHdr,    INT2PTR(NS_TRUE)},
        {"-method",           Ns_ObjvString,  &method,         NULL},
        {"-outputfile",       Ns_ObjvString,  &outputFileName, NULL},
        {"-outputchan",       Ns_ObjvString,  &outputChanName, NULL},
        {"-spoolsize",        Ns_ObjvMemUnit, &spoolLimit,     NULL},
        {"-timeout",          Ns_ObjvTime,    &timeoutPtr,     NULL},
        {"-expire",           Ns_ObjvTime,    &expirePtr,      NULL},
        {"-verify",           Ns_ObjvBool,    &verifyCert,     INT2PTR(NS_FALSE)},
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
        result  = HttpConnect(interp,
                              method,
                              url,
                              requestHdrPtr,
                              bodySize,
                              bodyObj,
                              bodyFileName,
                              cert,
                              caFile,
                              caPath,
                              sniHostname,
                              (verifyCert  == 1) ? NS_TRUE : NS_FALSE,
                              (keepHostHdr == 1) ? NS_TRUE : NS_FALSE,
                              timeoutPtr,
                              expirePtr,
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
        if (decompress != 0) {
            httpPtr->flags |= NS_HTTP_FLAG_DECOMPRESS;
        }

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
            static Ns_TaskQueue *taskQueue = NULL; /* MT: static variable! */

            /*
             * Enqueue the task, optionally returning the taskID
             */

            if (taskQueue == NULL) {
                Ns_MasterLock();
                if (taskQueue == NULL) {
                    taskQueue = Ns_CreateTaskQueue("tclhttp");
                }
                Ns_MasterUnlock();
            }

            if (Ns_TaskEnqueue(httpPtr->task, taskQueue) != NS_OK) {
                HttpSpliceChannels(interp, httpPtr);
                HttpClose(httpPtr);
                Ns_TclPrintfResult(interp, "could not queue HTTP task");
                result = TCL_ERROR;

            } else if (httpPtr->doneCallback != NULL) {

                /*
                 * There is nothing to wait on when the doneCallback
                 * was declared, since the callback garbage-collects
                 * the task. Hence we do not create the taskID.
                 */
                Ns_Log(Ns_LogTaskDebug, "no taskID returned");

            } else {
                Tcl_HashEntry *hPtr = NULL;
                uint32_t       ii;
                int            len;
                char           buf[TCL_INTEGER_SPACE + 4];

                /*
                 * Create taskID to be used for [ns_http_wait] et al.
                 */
                memcpy(buf, "http", 4u);
                for (ii = (uint32_t)itPtr->httpRequests.numEntries; ; ii++) {
                    int new = 0;

                    len = ns_uint32toa(&buf[4], ii);
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
    Tcl_Interp  *interp,
    NsHttpTask *httpPtr
) {
    int      result = TCL_OK;
    Ns_Time  diff;
    Tcl_Obj *statusObj       = NULL,
            *replyBodyObj    = NULL,
            *fileNameObj     = NULL,
            *resultObj       = NULL,
            *replyHeadersObj = NULL,
            *elapsedTimeObj  = NULL;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(httpPtr != NULL);

    if (httpPtr->error != NULL) {
        if (httpPtr->finalSockState == NS_SOCK_TIMEOUT) {
            Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
        }
        Tcl_SetObjResult(interp, Tcl_NewStringObj(httpPtr->error, -1));
        result = TCL_ERROR;
        goto err;
    }

    if (httpPtr->recvSpoolMode == NS_FALSE) {
        bool   binary = NS_FALSE;
        int    cSize;
        char  *cData;

        /*
         * Determine type (binary/text) of the received data
         * and decide what kind of object we should create
         * to return the content to the Tcl.
         * We have a choice between binary and string objects.
         * Unfortunately, this is mostly whole lotta guess-work...
         */
        if ((httpPtr->flags & NS_HTTP_FLAG_GZIP_ENCODING) != 0u) {
            if ((httpPtr->flags & NS_HTTP_FLAG_DECOMPRESS) == 0) {

                /*
                 * Gzipped but not inflated content
                 * is automatically of a binary-type.
                 * This is pretty straight-forward.
                 */
                binary = NS_TRUE;
            }
        }
        if (binary == NS_FALSE) {
            char  *cType = NULL;

            cType = Ns_SetIGet(httpPtr->replyHeaders, contentTypeHeader);
            if (cType != NULL) {

                /*
                 * Caveat Emptor:
                 * This call may return true even for
                 * completely regular text formats!
                 */
                binary = Ns_IsBinaryMimeType(cType);
            }
        }

        cData = httpPtr->ds.string + httpPtr->replyHeaderSize;
        cSize = (int)httpPtr->replyBodySize;

        if (binary == NS_TRUE)  {
            replyBodyObj = Tcl_NewByteArrayObj((unsigned char *)cData, cSize);
        } else {
            replyBodyObj = Tcl_NewStringObj(cData, cSize);
        }
    }

    statusObj = Tcl_NewIntObj(httpPtr->status);

    if (httpPtr->spoolFd != NS_INVALID_FD) {
        fileNameObj = Tcl_NewStringObj(httpPtr->spoolFileName, -1);
    }

    Ns_DiffTime(&httpPtr->etime, &httpPtr->stime, &diff);
    elapsedTimeObj = Tcl_NewObj();
    Ns_TclSetTimeObj(elapsedTimeObj, &diff);

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
    if (httpPtr->infoObj != NULL) {
        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("https", 5),
                       httpPtr->infoObj);
    }
    if (httpPtr->bodyChan != NULL) {
        const char *chanName = Tcl_GetChannelName(httpPtr->bodyChan);

        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("body_chan", 9),
                       Tcl_NewStringObj(chanName, -1));
    }
    if (httpPtr->spoolChan != NULL) {
        const char *chanName = Tcl_GetChannelName(httpPtr->spoolChan);

        Tcl_DictObjPut(interp, resultObj, Tcl_NewStringObj("outputchan", 10),
                       Tcl_NewStringObj(chanName, -1));
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
 *      Check whether we have full HTTP response incl. headers.
 *      If yes, record the total size of the response
 *      (including the lone CR/LF delimiter) in the NsHttpTask
 *      structure, as to avoid subsequent checking.
 *      Terminate the response string by eliminating the lone
 *      CR/LF delimiter (put a NULL byte at the CR place).
 *      This way it is easy to calculate size of the optional
 *      body content following the response line/headers.
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
        httpPtr->replyHeaderSize = (int)(eoh - httpPtr->ds.string) + 4;
        *(eoh + 2) = '\0';
    } else {
        eoh = strstr(httpPtr->ds.string, "\n\n");
        if (eoh != NULL) {
            Ns_Log(Warning, "HttpCheckHeader: client reply contains"
                   " LF instead of CR/LF trailer which should not happen");
            httpPtr->replyHeaderSize = (int)(eoh - httpPtr->ds.string) + 2;
            *(eoh + 1) = '\0';
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCheckSpool --
 *
 *      Determine, whether the received data should be left in
 *      the memory or whether it should be spooled to a file
 *      or channel, depending on the size of the returned content
 *      and the configuration settings.
 *
 * Results:
 *      Ns_ReturnCode.
 *
 * Side effects:
 *      Handles the partial response content located in memory.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
HttpCheckSpool(
    NsHttpTask *httpPtr
) {
    Ns_ReturnCode result = NS_OK;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    /*
     * At this point, we already identified the end of the
     * response/headers but haven not yet parsed it because
     * we still do not know the value of the response status.
     *
     * The Ns_DString in httpPtr->ds contains, at this point:
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
                            NULL,
                            NULL,
                            &httpPtr->status,
                            NULL) != NS_OK || httpPtr->status == 0) {

        Ns_Log(Warning, "ns_http: parsing reply failed");
        result = NS_ERROR;

    } else {
        const char *header = NULL;
        Tcl_WideInt replyLength = 0;

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
                Ns_SetIDeleteKey(httpPtr->replyHeaders, transferEncodingHeader);
            }
        }

        /*
         * See if we are handling compressed content.
         * Turn-on auto-decompress if requested.
         */
        header = Ns_SetIGet(httpPtr->replyHeaders, contentEncodingHeader);
        if (header != NULL && Ns_Match(header, "gzip") != NULL) {
            httpPtr->flags |= NS_HTTP_FLAG_GZIP_ENCODING;
            if ((httpPtr->flags & NS_HTTP_FLAG_GUNZIP) != 0u) {
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
                int fd = NS_INVALID_FD;

                if (httpPtr->spoolFileName != NULL) {
                    int flags;

                    flags = O_WRONLY|O_CREAT|O_CLOEXEC;
                    fd = ns_open(httpPtr->spoolFileName, flags, 0644);
                } else {
                    const char *tmpDir, *tmpFile = "http.XXXXXX";
                    size_t tmpLen;

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
                    sprintf(httpPtr->spoolFileName, "%s/%s", tmpDir, tmpFile);
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
                    result = NS_ERROR;
                }
            }
        }
    }

    if (result == NS_OK) {
        size_t cSize = 0;

        cSize = (size_t)(httpPtr->ds.length - httpPtr->replyHeaderSize);
        if (cSize > 0) {
            char buf[CHUNK_SIZE], *cData = NULL;

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
                result = NS_ERROR;
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
    Tcl_HashEntry *hPtr = NULL;
    bool           status = NS_TRUE;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(taskID != NULL);
    NS_NONNULL_ASSERT(httpPtrPtr != NULL);

    hPtr = Tcl_FindHashEntry(&itPtr->httpRequests, taskID);
    if (hPtr == NULL) {
        Ns_TclPrintfResult(itPtr->interp, "no such request: %s", taskID);
        status = NS_FALSE;
    } else {
        *httpPtrPtr = (NsHttpTask *)Tcl_GetHashValue(hPtr);
        if (remove) {
            Tcl_DeleteHashEntry(hPtr);
        }
        status = NS_TRUE;
    }

    return status;
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
    Ns_Time *timeoutPtr
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
        ms = (long)(timeoutPtr->sec*1000 + timeoutPtr->usec/1000 + 1);
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
    Tcl_Interp *interp,
    const char *method,
    const char *url,
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
    NsHttpTask **httpPtrPtr
) {
    NsHttpTask      *httpPtr;
    Ns_DString      *dsPtr;
    bool             haveUserAgent = NS_FALSE;
    unsigned short   portNr, defPortNr;
    char            *port = (char*)NS_EMPTY_STRING;
    char            *url2, *proto, *host, *path, *tail;
    const char      *contentType = NULL;

    static uint64_t  httpClientRequestCount = 0u; /* MT: static variable! */
    uint64_t         requestCount = 0u;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(httpPtrPtr != NULL);

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
    httpPtr->replyHeaders = Ns_SetCreate("replyHeaders");

    if (timeoutPtr != NULL) {
        httpPtr->timeout = ns_calloc(1u, sizeof(Ns_Time));
        *httpPtr->timeout = *timeoutPtr;
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
    if (Ns_ParseUrl(url2, &proto, &host, &port, &path, &tail) != NS_OK
        || proto == NULL
        || host  == NULL
        || path  == NULL
        || tail  == NULL) {

        Ns_TclPrintfResult(interp, "invalid URL \"%s\"", url);
        goto fail;
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
    if (STREQ("http", proto)) {
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
    else if (STREQ("https", proto)) {
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
    if (port != NULL) {
        portNr = (unsigned short) strtol(port, NULL, 10);
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
     * Now we are ready to attempt the connection.
     * If no timeout given, assume 10 seconds.
     */

    {
        Ns_ReturnCode rc;
        Ns_Time       def = {10, 0}, *toPtr = NULL;

        Ns_Log(Ns_LogTaskDebug, "connecting to [%s]:%hu", host, portNr);

        /*
         * Open the socket to remote, assure it's writable
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
            toPtr = &def;
        }
        httpPtr->sock = Ns_SockTimedConnect2(host, portNr, NULL, 0, toPtr, &rc);
        if (httpPtr->sock == NS_INVALID_SOCKET) {
            Ns_SockConnectError(interp, host, portNr, rc);
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
                Tcl_SetErrorCode(interp, errorCodeTimeoutString, (char *)0L);
            } else {
                Ns_TclPrintfResult(interp, "waiting for writable socket: %s",
                                   ns_sockstrerror(ns_sockerrno));
            }
            goto fail;
        }

        /*
         * Optionally setup a SSL connection
         */
        if (defPortNr == 443u) {
            NS_TLS_SSL_CTX *ctx = NULL;
            NS_TLS_SSL     *ssl = NULL;
            int             result = TCL_OK;

            result = Ns_TLS_CtxClientCreate(interp, cert, caFile, caPath,
                                            verifyCert, &ctx);
            if (likely(result == TCL_OK)) {
                httpPtr->ctx = ctx;
                result = Ns_TLS_SSLConnect(interp, httpPtr->sock, ctx,
                                           sniHostname, &ssl);
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

    /*
     * At this point we are connected.
     * Construct HTTP request line.
     */
    Ns_DStringSetLength(dsPtr, 0);
    Ns_DStringAppend(dsPtr, method);
    Ns_StrToUpper(Ns_DStringValue(dsPtr));
    Ns_DStringNAppend(dsPtr, " /", 2);
    if (*path != '\0') {
        Ns_DStringNAppend(dsPtr, path, -1);
        Ns_DStringNAppend(dsPtr, "/", 1);
    }
    Ns_DStringNAppend(dsPtr, tail, -1);
    Ns_DStringNAppend(dsPtr, " HTTP/1.1\r\n", 11);

    /*
     * Add provided headers, remove headers we are providing explicitly,
     * check User-Agent header existence.
     */
    if (hdrPtr != NULL) {
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
     * Disable keep-alive connections
     * FIXME: why?
     */
    Ns_DStringPrintf(dsPtr, "%s: close\r\n", connectionHeader);

    /*
     * Optionally, add our own Host header
     */
    if (keepHostHdr == NS_FALSE) {
        (void)Ns_DStringVarAppend(dsPtr, hostHeader, ": ", (char *)0L);
        (void)Ns_HttpLocationString(dsPtr, NULL, host, portNr, defPortNr);
        Ns_DStringNAppend(dsPtr, "\r\n", 2);
    }

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

        if (hdrPtr != NULL) {
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
            int   bodyLen = 0;
            char *bodyStr = NULL;
            bool  binary;

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
                bodyStr = (char *)Tcl_GetByteArrayFromObj(bodyObj, &bodyLen);
            } else {
                bodyStr = Tcl_GetStringFromObj(bodyObj, &bodyLen);
            }

            httpPtr->bodySize = (size_t)bodyLen;
            Ns_DStringPrintf(dsPtr, "%s: %d\r\n\r\n", contentLengthHeader,
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
        Ns_Log(Ns_LogRequestDebug, "full request (len %d) <%s>",
               dsPtr->length,
               Ns_DStringAppendPrintable(&d, NS_TRUE,dsPtr->string,
                                         (size_t)dsPtr->length));
        Tcl_DStringFree(&d);
    }

    return TCL_OK;

 fail:
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
    ssize_t written = -1;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (httpPtr->recvSpoolMode == NS_TRUE) {
        if (httpPtr->spoolFd != NS_INVALID_FD) {
            written = ns_write(httpPtr->spoolFd, buffer, size);
        } else if (httpPtr->spoolChan != NULL) {
            written = (ssize_t)Tcl_Write(httpPtr->spoolChan, buffer, (int)size);
        } else {
            written = -1;
        }
    } else {
        Tcl_DStringAppend(&httpPtr->ds, buffer, (int)size);
        written = (ssize_t)size;
    }

    if (written > -1) {
        result = TCL_OK;
    } else {
        Ns_Log(Error, "task: spooling of received content failed");
        result = TCL_ERROR;
    }

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

    Ns_Log(Ns_LogTaskDebug, "HttpAppendBuffer: got: %" PRIuz " bytes flags:%.6x",
           size, httpPtr->flags);

    if (likely((httpPtr->flags & NS_HTTP_FLAG_GUNZIP) == 0u)) {

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
 *        to oarse any character sequencer, including the chunked.
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
 *        independent from NsHttp and re-used elsewhere.
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
    NsHttpChunk *chunkPtr = NULL;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    chunkPtr = httpPtr->chunk;
    NS_NONNULL_ASSERT(chunkPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "HttpAppendChunked free httpPtr:%p, task:%p",
           (void*)httpPtr, (void*)httpPtr->task);

    while (len > 0 && result != TCL_ERROR) {
        NsHttpParseProc *parseProcPtr = NULL;

        Ns_Log(Ns_LogTaskDebug, "... len %lu ", len);

        parseProcPtr = *(chunkPtr->parsers + chunkPtr->callx);
        while (len > 0 && parseProcPtr != NULL) {
            result = (*parseProcPtr)(httpPtr, &buf, &len);
            Ns_Log(Ns_LogTaskDebug, "... parseproc returns %d ", result);
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

    if (result != TCL_ERROR) {
        result = TCL_OK;
    }

    return result;
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

    Ns_Log(Ns_LogTaskDebug, "HttpClose free httpPtr:%p, task:%p",
           (void*)httpPtr, (void*)httpPtr->task);

    /*
     * When HttpConnect runs into a failure, it might not have httpPtr->task
     * set. We cannot be sure, the task is always set.
     */

    if (httpPtr->task != NULL) {
        (void) Ns_TaskFree(httpPtr->task);
        httpPtr->task = NULL;
    }
#ifdef HAVE_OPENSSL_EVP_H
    if (httpPtr->ssl != NULL) {
        SSL_shutdown(httpPtr->ssl);
        SSL_free(httpPtr->ssl);
    }
    if (httpPtr->ctx != NULL) {
        SSL_CTX_free(httpPtr->ctx);
    }
#endif
    if (httpPtr->sock != NS_INVALID_SOCKET) {
        ns_sockclose(httpPtr->sock);
    }
    if (httpPtr->spoolFileName != NULL) {
        ns_free((void *)httpPtr->spoolFileName);
    }
    if (httpPtr->doneCallback != NULL) {
        ns_free((void *)httpPtr->doneCallback);
    }
    if (httpPtr->spoolFd != NS_INVALID_FD) {
        (void)ns_close(httpPtr->spoolFd);
    }
    if (httpPtr->bodyFileFd != NS_INVALID_FD) {
        (void)ns_close(httpPtr->bodyFileFd);
    }
    if (httpPtr->bodyChan != NULL) {
        (void)Tcl_Close(NULL, httpPtr->bodyChan);
    }
    if (httpPtr->spoolChan != NULL) {
        (void)Tcl_Close(NULL, httpPtr->spoolChan);
    }
    if (httpPtr->compress != NULL) {
        (void)Ns_InflateEnd(httpPtr->compress);
        ns_free((void *)httpPtr->compress);
    }
    if (httpPtr->infoObj != NULL) {
        Tcl_DecrRefCount(httpPtr->infoObj);
        httpPtr->infoObj = NULL;
    }
    if (httpPtr->replyHeaders != NULL) {
        Ns_SetFree(httpPtr->replyHeaders);
    }
    if (httpPtr->timeout != NULL) {
        ns_free((void *)httpPtr->timeout);
    }

    ns_free((void *)httpPtr->url);

    Ns_MutexDestroy(&httpPtr->lock); /* Should not be held locked here! */
    Tcl_DStringFree(&httpPtr->ds);

    if (httpPtr->chunk != NULL) {
        Tcl_DStringFree(&httpPtr->chunk->ds);
        ns_free((void *)httpPtr->chunk);
    }

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

    (void) Ns_TaskCancel(httpPtr->task);

    /*
     * Wait (potentially infinitely) on task to finish
     * to make sure it cannot be referenced later.
     */

    (void) Ns_TaskWait(httpPtr->task, NULL);
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
    Tcl_Obj *keyObj = NULL;
    Tcl_Obj *valObj = NULL;

    if (httpPtr->infoObj == NULL) {
        httpPtr->infoObj = Tcl_NewDictObj();
        Tcl_IncrRefCount(httpPtr->infoObj);
    }

    keyObj = Tcl_NewStringObj(key, -1);
    valObj = Tcl_NewStringObj(value, -1);

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
    NsHttpTask *httpPtr,
    const void *buffer,
    size_t length
) {
    ssize_t sent;
    struct  iovec iov, *bufs = &iov;
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

    Ns_Log(Ns_LogTaskDebug, "HttpTaskSend sent %" PRIdz
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
    NsHttpTask *httpPtr,
    char *buffer,
    size_t length,
    Ns_SockState *statePtr
) {
    ssize_t recv = 0;
    int     nbufs = 1;
    struct  iovec iov, *bufs = &iov;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    iov.iov_base = (void *)buffer;
    iov.iov_len  = length;

    if (httpPtr->ssl == NULL) {
        recv = Ns_SockRecvBufs2(httpPtr->sock, bufs, nbufs, 0u, statePtr);
    } else {
#ifndef HAVE_OPENSSL_EVP_H
        recv = -1;
#else
        recv = Ns_SSLRecvBufs2(httpPtr->ssl, bufs, nbufs, statePtr);
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
    NsServer    *servPtr;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "HttpDoneCallback finalSockState:%.2x, err:%s",
           httpPtr->finalSockState,
           (httpPtr->error != NULL) ? httpPtr->error : "(none)");

    servPtr = NsGetServer(nsconf.defaultServer); /* FIXME */
    interp = NsTclAllocateInterp(servPtr);

    result = HttpGetResult(interp, httpPtr);

    Tcl_DStringInit(&script);
    Tcl_DStringAppend(&script, httpPtr->doneCallback, -1);
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

    HttpClose(httpPtr);
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

    Ns_Log(Ns_LogTaskDebug, "HttpProc enter socketState %.2x", why);

    switch (why) {
    case NS_SOCK_INIT:

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_INIT");

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

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE sendSpoolMode:%d,"
               " fd:%d, chan:%s", httpPtr->sendSpoolMode, httpPtr->bodyFileFd,
               httpPtr->bodyChan ? Tcl_GetChannelName(httpPtr->bodyChan) : "(none)");

        if (httpPtr->sendSpoolMode == NS_FALSE) {
            size_t remain = 0;

            /*
             * Send (next part of) the request from memory.
             * This may not include the request body, as it may have
             * to be spooled from the passed file or Tcl channel.
             * Decision whether to do this or not is done when we have
             * finished sending request line + all of the headers.
             */
            remain = (size_t)(httpPtr->requestLength - httpPtr->sent);

            Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE"
                   " will send dsPtr:%p, next:%p, remain:%" PRIuz,
                   (void*)httpPtr->ds.string, (void*)httpPtr->next, remain);

            if (remain > 0) {
                n = HttpTaskSend(httpPtr, httpPtr->next, remain);
            } else {
                n = 0;
            }

            if (n == -1) {
                httpPtr->error = "http send failed";
                Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE send failed");

            } else {
                ssize_t nb = 0;

                httpPtr->next += n;
                nb = (ssize_t)(httpPtr->sent - httpPtr->requestHeaderSize);
                Ns_MutexLock(&httpPtr->lock);
                httpPtr->sent += (size_t)n;
                if (nb > 0) {
                    httpPtr->sendBodySize = (size_t)nb;
                }
                Ns_MutexUnlock(&httpPtr->lock);
                remain = (size_t)(httpPtr->requestLength - httpPtr->sent);
                if (remain > 0) {

                    /*
                     * We still have something to be send
                     * left in memory.
                     */
                    Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE"
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
                    logMsg = "HttpProc NS_SOCK_WRITE headers sent";
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

            Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE sendSpoolMode"
                   " buffersize:%d buffer:%p next:%p sent:%" PRIuz,
                   httpPtr->ds.length, (void *)httpPtr->ds.string,
                   (void *)httpPtr->next, httpPtr->sent);

            if (httpPtr->next == NULL) {

                /*
                 * Read remaining body data in chunks
                 */
                Tcl_DStringSetLength(&httpPtr->ds, (int)toRead);
                httpPtr->next = httpPtr->ds.string;
                if (toRead > httpPtr->bodySize) {
                    toRead = httpPtr->bodySize; /* At end of the body! */
                }
                if (toRead == 0) {
                    n = 0;
                } else if (httpPtr->bodyFileFd != NS_INVALID_FD) {
                    n = ns_read(httpPtr->bodyFileFd, httpPtr->next, toRead);
                } else if (httpPtr->bodyChan != NULL) {
                    n = Tcl_Read(httpPtr->bodyChan, httpPtr->next, (int)toRead);
                } else {
                    n = -1; /* Here we could read only from file or chan! */
                }

                if (toRead == 0 || (n > -1 && n < (ssize_t)toRead)) {

                    /*
                     * We have a short file/chan read which can only mean
                     * we are at the EOF (we are reading in blocking mode!).
                     */
                    onEof = NS_TRUE;
                    Tcl_DStringSetLength(&httpPtr->ds, (int)n);
                }

                if (n > 0) {
                    assert((size_t)n <= httpPtr->bodySize);
                    httpPtr->bodySize -= (size_t)n;
                }

                Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE sendSpoolMode"
                       " got:%" PRIdz " wanted:%" PRIuz " bytes, eof:%d",
                       n, toRead, onEof);

            } else {

                /*
                 * The buffer has still some content left
                 */
                n = httpPtr->ds.length - (httpPtr->next - httpPtr->ds.string);

                Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE"
                       " remaining buffersize:%" PRIdz, n);
            }

            /*
             * We got some bytes from file/channel/memory
             * so send them to the remote.
             */

            if (unlikely(n == -1)) {
                httpPtr->error = "http read failed";
                Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE read failed");

            } else {
                ssize_t toSend = n, sent = 0;

                if (toSend > 0) {
                    sent = HttpTaskSend(httpPtr, httpPtr->next, (size_t)toSend);
                }

                Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE sent"
                       " %" PRIdz " of %" PRIuz " bytes", sent, toSend);

                if (unlikely(sent == -1)) {
                    httpPtr->error = "http send failed";
                    Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE send failed");

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
                        nb = (ssize_t)(httpPtr->sent - httpPtr->requestHeaderSize);
                        Ns_MutexLock(&httpPtr->lock);
                        httpPtr->sent += (size_t)sent;
                        if (nb > 0) {
                            httpPtr->sendBodySize = (size_t)nb;
                        }
                        Ns_MutexUnlock(&httpPtr->lock);
                    }
                    Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE partial"
                           " send, remain:%ld", (long)(toSend - sent));

                    taskDone = NS_FALSE;

                } else if (sent == toSend) {

                    /*
                     * We have sent the whole buffer
                     */
                    if (sent > 0) {
                        ssize_t nb = 0;

                        nb = (ssize_t)(httpPtr->sent - httpPtr->requestHeaderSize);
                        Ns_MutexLock(&httpPtr->lock);
                        httpPtr->sent += (size_t)sent;
                        if (nb > 0) {
                            httpPtr->sendBodySize = (size_t)nb;
                        }
                        Ns_MutexUnlock(&httpPtr->lock);
                        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE sent"
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
                            Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE"
                                   " whole body sent, switch to read");
                            nextState = NS_SOCK_READ;

                        } else {

                            /*
                             * We read less then chunksize bytes, the source
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
                            httpPtr->error = "http read failed";
                            taskDone = NS_TRUE;
                            Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_WRITE"
                                   " short read, left:%" PRIuz, httpPtr->bodySize);
                        }
                    }

                } else {

                    /*
                     * This is completely unexpected: we have send more
                     * then requested? There is something entirely wrong!
                     * I have no idea what would be the best to do here.
                     */
                    Ns_Log(Error, "HttpProc NS_SOCK_WRITE bad state?");
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

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_READ");

        nextState = why;

        if (httpPtr->sent == 0u) {
            Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_READ nothing sent?");

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
                httpPtr->error = "http read failed";
                Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_READ receive failed");

            } else if (n > 0) {
                int result = TCL_OK;

                /*
                 * Most likely case: we got some bytes.
                 */
                Ns_MutexLock(&httpPtr->lock);
                httpPtr->received += (size_t)n;
                Ns_MutexUnlock(&httpPtr->lock);
                result = HttpAppendContent(httpPtr, buf, (size_t)n);
                if (unlikely(result != TCL_OK)) {
                    httpPtr->error = "http read failed";
                    Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_READ append failed");
                } else {
                    Ns_ReturnCode rc = NS_OK;
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
                        rc = HttpCheckSpool(httpPtr);
                    }
                    if (unlikely(rc != NS_OK)) {
                        httpPtr->error = "http read failed";
                        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_READ spool failed");
                    } else {
                        taskDone = NS_FALSE;
                    }
                }

            } else if (len > 0 && sockState == NS_SOCK_AGAIN) {

                /*
                 * Received zero bytes on a readable socket
                 * but it's not on EOD, it wants us to read more.
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
                Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_READ error,"
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

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_TIMEOUT");

        /*
         * Without a doneCallback, NS_SOCK_DONE must be handled
         * by the caller (normally, caller would cancel the task)
         * hence we leave the task in processing.
         *
         * With doneCallback, the caller is cut-off of the task ID
         * (i.e. there is no chance for cancel) hence we must mark
         * the task as completed (done) right here.
         */
        taskDone = (httpPtr->doneCallback == NULL) ? NS_FALSE : NS_TRUE;
        httpPtr->error = "http request timeout";

        break;

    case NS_SOCK_EXIT:

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_EXIT");
        httpPtr->error = "http task queue shutdown";

        break;

    case NS_SOCK_CANCEL:

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_CANCEL");
        httpPtr->error = "http request cancelled";

        break;

    case NS_SOCK_EXCEPTION:

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_EXCEPTION");
        httpPtr->error = "unexpected http socket exception";

        break;

    case NS_SOCK_AGAIN:

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_AGAIN");
        httpPtr->error = "unexpected http EOD";

        break;

    case NS_SOCK_DONE:

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_DONE doneCallback:%s",
               httpPtr->doneCallback != NULL ? httpPtr->doneCallback : "(none)");

        if (httpPtr->bodyChan != NULL) {
            HttpCutChannel(NULL, httpPtr->bodyChan);
        }
        if (httpPtr->spoolChan != NULL) {
            HttpCutChannel(NULL, httpPtr->spoolChan);
        }
        if (httpPtr->doneCallback != NULL) {
            HttpDoneCallback(httpPtr); /* Does free on the httpPtr */
            httpPtr = NULL;
            taskDone = NS_FALSE; /* Callback terminates the task */
        }

        break;

    case NS_SOCK_NONE:

        Ns_Log(Ns_LogTaskDebug, "HttpProc NS_SOCK_NONE");
        httpPtr->error = "unexpected http socket state";

        break;
    }

    if (httpPtr != NULL) {
        httpPtr->finalSockState = why;
        Ns_Log(Ns_LogTaskDebug, "HttpProc exit taskDone:%d, finalSockState:%.2x,"
               " error:%s", taskDone, httpPtr->finalSockState,
               httpPtr->error != NULL ? httpPtr->error : "(none)");
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

    Ns_Log(Ns_LogTaskDebug, "--- ParseLengthProc");

    /*
     * Collect all that looks as a hex digit
     */
    while (len > 0 && *(buf) > 0 && isxdigit(*(buf))) {
        Tcl_DStringAppend(dsPtr, buf, 1);
        len--;
        buf++;
    }

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

    Ns_Log(Ns_LogTaskDebug, "--- ParseBodyProc");

    if (chunkPtr->length == 0) {
        Ns_Set *headersPtr = NULL;
        char *trailer = NULL;

        /*
         * We are on the last chunk. Check if we will get some
         * trailers and switch the state accordingly.
         */
        headersPtr = httpPtr->replyHeaders;
        trailer = Ns_SetIGet(headersPtr, trailersHeader);
        if (trailer != NULL) {
            chunkPtr->parsers = TrailerParsers;
        } else {
            chunkPtr->parsers = EndParsers;
        }

        chunkPtr->callx = 0;
        result = TCL_BREAK;

    } else if (len == 0) {
        result = TCL_BREAK;
    } else {
        size_t remain = 0u, append = 0u;

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
            Ns_Set *headersPtr = NULL;
            char   *trailer = NULL;

            headersPtr = httpPtr->replyHeaders;
            trailer = dsPtr->string;
            if (Ns_ParseHeader(headersPtr, trailer, ToLower) != NS_OK) {
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
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * eval: (c-guess)
 * End:
 */
