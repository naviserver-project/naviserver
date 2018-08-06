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
 *	Support for the ns_http command.
 */

#include "nsd.h"
#ifdef HAVE_OPENSSL_EVP_H
# include <openssl/err.h>
#endif
/*
 * The maximum chunk size from TLS is 2^14 => 16384 (see RFC 5246). OpenSSL
 * can't send more than this number of bytes in one attempt.
 */
#define CHUNK_SIZE 16384

/*
 * Local functions defined in this file
 */

static int HttpQueueCmd(
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
    Tcl_Obj *bodyPtr,
    const char *bodyFileName,
    const char *cert,
    const char *caFile,
    const char *caPath,
    const char *sni_hostname,
    bool verify,
    bool keep_host_header,
    const Ns_Time *timeoutPtr,
    Ns_HttpTask **httpPtrPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(14);

static bool HttpGet(
    NsInterp *itPtr,
    const char *id,
    Ns_HttpTask **httpPtrPtr,
    bool removeRequest
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void HttpClose(
    Ns_HttpTask *httpPtr
)  NS_GNUC_NONNULL(1);

static void HttpCancel(
    const Ns_HttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static void HttpAbort(
    Ns_HttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static int HttpAppendRawBuffer(
    Ns_HttpTask *httpPtr,
    const char *buffer,
    size_t outSize
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void ProcessReplyHeaderFields(
    Ns_HttpTask *httpPtr
) NS_GNUC_NONNULL(1);

static Ns_ReturnCode WaitState(
    NS_SOCKET sock,
    short events,
    Ns_Time *timeout
);
static int EnsureWritable(
    Tcl_Interp  *interp,
    Ns_HttpTask *httpPtr,
    const char *url
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(2);

static void
HttpTaskAddInfo(
    Ns_HttpTask *httpPtr,
    const char *attribute,
    const char *value
)  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(2);


static ssize_t HttpTaskSend(
    const Ns_HttpTask *httpPtr,
    const void *buffer,
    size_t length
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static ssize_t HttpTaskRecv(
    const Ns_HttpTask *httpPtr,
    char *buffer,
    size_t length
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#if 0
static void HttpTaskShutdown(
    const Ns_HttpTask *httpPtr,
    int mode
) NS_GNUC_NONNULL(1);
#endif

static int
CheckReplyHeaders(
    Tcl_Interp  *interp,
    Ns_Set     **replyHdrPtrPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int
SetResult(
    Tcl_Interp  *interp,
    Ns_HttpTask *httpPtr,
    Tcl_Obj     *replyHeadersObj,
    Tcl_Obj     *elapsedVarPtr,
    Tcl_Obj     *resultVarPtr,
    Tcl_Obj     *statusVarPtr,
    Tcl_Obj     *fileVarPtr
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)  NS_GNUC_NONNULL(3);

static Ns_TaskProc HttpProc;

static Tcl_ObjCmdProc HttpCancelObjCmd;
static Tcl_ObjCmdProc HttpCleanupObjCmd;
static Tcl_ObjCmdProc HttpListObjCmd;
static Tcl_ObjCmdProc HttpStatsObjCmd;
static Tcl_ObjCmdProc HttpQueueObjCmd;
static Tcl_ObjCmdProc HttpRunObjCmd;
static Tcl_ObjCmdProc HttpWaitObjCmd;


/*
 * Static variables defined in this file.
 */
static Ns_TaskQueue *session_queue;


/*
 *----------------------------------------------------------------------
 *
 * CheckReplyHeaders --
 *
 *	Check, if reply headers are provided. If not, create on the fly
 *	automatically new reply headers and enter these to the interpreter.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	May create a dynamic ns_set in the provided interpreter.
 *
 *----------------------------------------------------------------------
 */

static int
CheckReplyHeaders(
    Tcl_Interp  *interp,
    Ns_Set     **replyHdrPtrPtr
) {
    int result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(replyHdrPtrPtr != NULL);

    if (*replyHdrPtrPtr == NULL) {
        *replyHdrPtrPtr = Ns_SetCreate("replyHeaders");
        result = Ns_TclEnterSet(interp, *replyHdrPtrPtr, NS_TCL_SET_DYNAMIC);
    } else {
        result = TCL_OK;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * SetResult --
 *
 *	Get the result of the Task and set it as the interpreted result in
 *	form of a dict (attribute / value pairs). The *VarPtr arguments are
 *	optional and can point to variable names, which can be set by this
 *	function.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Potentially setting Tcl variables provided via *VarPtr arguments.
 *
 *----------------------------------------------------------------------
 */

static int
SetResult(
    Tcl_Interp  *interp,
    Ns_HttpTask *httpPtr,
    Tcl_Obj     *replyHeadersObj,
    Tcl_Obj     *elapsedVarPtr,
    Tcl_Obj     *resultVarPtr,
    Tcl_Obj     *statusVarPtr,
    Tcl_Obj     *fileVarPtr
) {
    int      result;
    Ns_Time  diff;
    Tcl_Obj *statusObj    = NULL,
            *replyBodyObj = NULL,
            *fileNameObj  = NULL,
            *elapsedTimeObj,
            *resultObj;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(replyHeadersObj != NULL);

    Ns_DiffTime(&httpPtr->etime, &httpPtr->stime, &diff);
    elapsedTimeObj = Tcl_NewObj();
    Ns_TclSetTimeObj(elapsedTimeObj, &diff);

    if (elapsedVarPtr != NULL) {
        if (Ns_SetNamedVar(interp, elapsedVarPtr, elapsedTimeObj) == NS_FALSE) {
            result = TCL_ERROR;
            goto err;
        }
    }

    if (httpPtr->spoolFd > 0)  {
        (void) ns_close(httpPtr->spoolFd);
    } else {
        bool binary = NS_TRUE;

        if (httpPtr->replyHeaders != NULL) {
            const char *contentEncoding = Ns_SetIGet(httpPtr->replyHeaders, "Content-Encoding");

            /*
             * Is the content gzipped encoded? If so, it is binary. If not
             * determine binary requirements from the content type.
             */

            if (contentEncoding == NULL || strncmp(contentEncoding, "gzip", 4u) != 0) {
                const char *contentType = Ns_SetIGet(httpPtr->replyHeaders, "Content-Type");

                if (contentType != NULL) {
                    /*
                     * Determine binary via content type
                     */
                    binary = Ns_IsBinaryMimeType(contentType);
                }
            }
        }

        if (binary)  {
            replyBodyObj = Tcl_NewByteArrayObj((unsigned char*)httpPtr->ds.string + httpPtr->replyHeaderSize,
                                               (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
        } else {
            replyBodyObj = Tcl_NewStringObj(httpPtr->ds.string + httpPtr->replyHeaderSize,
                                            (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
        }
    }

    /*
     * Set Tcl_Objs for result elements and set optionally variables, if such
     * names were provided.
     */
    statusObj = Tcl_NewIntObj(httpPtr->status);
    if (statusVarPtr != NULL
        && Ns_SetNamedVar(interp, statusVarPtr, statusObj) == NS_FALSE) {
        result = TCL_ERROR;
        goto err;
    }

    if (httpPtr->spoolFd > 0) {
        fileNameObj = Tcl_NewStringObj(httpPtr->spoolFileName, -1);
        if (fileVarPtr != NULL
            && Ns_SetNamedVar(interp, fileVarPtr, fileNameObj) == NS_FALSE) {
            result = TCL_ERROR;
            goto err;
        }
    }

    if (replyBodyObj != NULL && resultVarPtr != NULL) {
        if (Ns_SetNamedVar(interp, resultVarPtr, replyBodyObj) == NS_FALSE) {
            result = TCL_ERROR;
            goto err;
        }
    }

    /*
     * Assemble the resulting list of attribute value pairs
     */
    resultObj = Tcl_NewListObj(0, NULL);
    Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("status", 6));
    Tcl_ListObjAppendElement(interp, resultObj, statusObj);
    Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("time", 4));
    Tcl_ListObjAppendElement(interp, resultObj, elapsedTimeObj);
    Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("headers", 7));
    Tcl_ListObjAppendElement(interp, resultObj, replyHeadersObj);
    if (fileNameObj != NULL) {
        Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("file", 4));
        Tcl_ListObjAppendElement(interp, resultObj, fileNameObj);
    }
    if (replyBodyObj != NULL) {
        Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("body", 4));
        Tcl_ListObjAppendElement(interp, resultObj, replyBodyObj);
    }
    if (httpPtr->infoObj != NULL) {
        Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("https", 5));
        Tcl_ListObjAppendElement(interp, resultObj, httpPtr->infoObj);
    }
    /*
     * Return this list as result.
     */
    Tcl_SetObjResult(interp, resultObj);
    result = TCL_OK;

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
 * HttpRunObjCmd, HttpQueueObjCmd - subcommands of "ns_http"
 *
 *	Implements the "ns_http run|queue"
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	May queue an HTTP request.
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
    return HttpQueueCmd(clientData, objc, objv, NS_TRUE);
}

static int
HttpQueueObjCmd(
    ClientData clientData,
    Tcl_Interp *UNUSED(interp),
    int objc,
    Tcl_Obj *const* objv
) {
    return HttpQueueCmd(clientData, objc, objv, NS_FALSE);
}



/*
 *----------------------------------------------------------------------
 *
 * HttpWaitObjCmd --
 *
 *	Implements "ns_http wait" subcommand.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Typically closing request.
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
    NsInterp    *itPtr = clientData;
    Tcl_Obj     *elapsedVarPtr = NULL,
                *resultVarPtr = NULL,
                *statusVarPtr = NULL,
        *fileVarPtr = NULL,
        *replyHeadersObj = NULL;

    Ns_Time     *timeoutPtr = NULL;
    char        *id = NULL,
                *outputFileName = NULL;
    Ns_Set      *replyHdrPtr = NULL;
    Ns_HttpTask *httpPtr = NULL;
    int          result, spoolLimit = -1, decompress = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout",    Ns_ObjvTime,   &timeoutPtr,      NULL},
        {"-headers",    Ns_ObjvObj,    &replyHeadersObj, NULL},
        {"-elapsed",    Ns_ObjvObj,    &elapsedVarPtr,   NULL},
        {"-result",     Ns_ObjvObj,    &resultVarPtr,    NULL},
        {"-status",     Ns_ObjvObj,    &statusVarPtr,    NULL},
        {"-file",       Ns_ObjvObj,    &fileVarPtr,      NULL},
        {"-outputfile", Ns_ObjvString, &outputFileName,  NULL},
        {"-spoolsize",  Ns_ObjvInt,    &spoolLimit,      NULL},
        {"-decompress", Ns_ObjvBool,   &decompress,      INT2PTR(NS_TRUE)},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"id",       Ns_ObjvString, &id, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (HttpGet(itPtr, id, &httpPtr, NS_TRUE) == NS_FALSE) {
        result = TCL_ERROR;

    } else if (replyHeadersObj != NULL
               && Ns_TclGetSet2(interp, Tcl_GetString(replyHeadersObj), &replyHdrPtr) != TCL_OK
              ) {
        result = TCL_ERROR;

    } else if (CheckReplyHeaders(interp, &replyHdrPtr) != NS_OK) {
        Ns_TclPrintfResult(interp, "ns_http: automatic generation of output headers failed");
        result = TCL_ERROR;

    } else {

        if (replyHeadersObj == NULL) {
            /*
             * When no replyHeadersObj was specified, we got the automatically
             * generated headers ns_set from CheckReplyHeaders(), which enters
             * it to the interp and returns it as result.
             */
            replyHeadersObj = Tcl_GetObjResult(interp);
        }
        Tcl_IncrRefCount(replyHeadersObj);

        if (decompress != 0) {
            httpPtr->flags |= NS_HTTP_FLAG_DECOMPRESS;
        }

        assert(replyHdrPtr != NULL);
        httpPtr->replyHeaders = replyHdrPtr;

        httpPtr->spoolLimit = spoolLimit;

        /*
         * When the outputFileName is given store it in the task structure. It
         * will be used in case output is spooled to a file.
         */
        if (outputFileName != NULL) {
            if (httpPtr->spoolFileName != NULL) {
                Ns_Log(Warning, "ns_http wait: the -outputfile was already "
                       "set in the 'queue' subcommand', ignore here");
            } else {
                httpPtr->spoolFileName = ns_strdup(outputFileName);
            }
        }

        //Ns_HttpCheckSpool(httpPtr);

        /*
         * Do the task wait operation
         */
        if (Ns_TaskWait(httpPtr->task, timeoutPtr) != NS_OK) {
            /*
             * Task wait failed.
             */
            HttpCancel(httpPtr);
            Ns_TclPrintfResult(interp, "timeout waiting for task");
            result = TCL_ERROR;

        } else {

            /*
             * Make sure that all task elements are updated.
             */
            if (httpPtr->replyHeaderSize == 0) {
                Ns_HttpCheckHeader(httpPtr);
            }
            Ns_HttpCheckSpool(httpPtr);

            if (httpPtr->error != NULL) {
                if (httpPtr->finalSockState == NS_SOCK_TIMEOUT) {
                    Tcl_SetErrorCode(interp, "NS_TIMEOUT", (char *)0L);
                }
                Ns_TclPrintfResult(interp, "ns_http failed: %s", httpPtr->error);
                result = TCL_ERROR;
            } else {
                result = SetResult(interp, httpPtr, replyHeadersObj, elapsedVarPtr,
                                   resultVarPtr, statusVarPtr, fileVarPtr);
            }
        }

        Tcl_DecrRefCount(replyHeadersObj);
    }

    if (httpPtr != NULL) {
        HttpClose(httpPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCancelObjCmd --
 *
 *	Implements "ns_http cancel" subcommand.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Typically aborting and closing request.
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
        {"id", Ns_ObjvString,  &idString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_HttpTask *httpPtr = NULL;

        if (HttpGet(itPtr, Tcl_GetString(objv[2]), &httpPtr, NS_TRUE) == NS_FALSE) {
            result = TCL_ERROR;
        } else {
            HttpAbort(httpPtr);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpCleanupObjCmd - subcommand of "ns_http"
 *
 *	Implements the "ns_http cleanup"
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	Aborting requests and reinitializing hash table.
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
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;

        for (hPtr = Tcl_FirstHashEntry(&itPtr->httpRequests, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search) ) {
            Ns_HttpTask *httpPtr = Tcl_GetHashValue(hPtr);

            HttpAbort(httpPtr);
        }
        Tcl_DeleteHashTable(&itPtr->httpRequests);
        Tcl_InitHashTable(&itPtr->httpRequests, TCL_STRING_KEYS);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpListObjCmd - subcommand of "ns_http"
 *
 *	Implements the "ns_http list"
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	None.
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
    NsInterp    *itPtr = clientData;
    int          result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;
        Tcl_DString          ds;

        Tcl_DStringInit(&ds);
        for (hPtr = Tcl_FirstHashEntry(&itPtr->httpRequests, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search) ) {
            const Ns_HttpTask *httpPtr = Tcl_GetHashValue(hPtr);

            Tcl_DStringAppend(&ds,  Tcl_GetHashKey(&itPtr->httpRequests, hPtr), -1);
            Tcl_DStringAppend(&ds, " ", 1);
            Tcl_DStringAppend(&ds,  httpPtr->url, -1);
            Tcl_DStringAppend(&ds, " ", 1);
            Tcl_DStringAppend(&ds,  Ns_TaskCompleted(httpPtr->task) == NS_TRUE ? "done" : "running", -1);
            Tcl_DStringAppend(&ds, " ", 1);
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpListObjCmd - subcommand of "ns_http"
 *
 *	Implements the "ns_http list"
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	None.
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
    NsInterp    *itPtr = clientData;
    int          result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;
        Tcl_Obj             *resultList = Tcl_NewListObj(0, NULL);

        for (hPtr = Tcl_FirstHashEntry(&itPtr->httpRequests, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search) ) {
            const Ns_HttpTask *httpPtr = Tcl_GetHashValue(hPtr);
            const char        *key = Tcl_GetHashKey(&itPtr->httpRequests, hPtr);
            Tcl_Obj           *entryObj = Tcl_NewListObj(0, NULL);

            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj("task", 4));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj(key, -1));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj("url", 3));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj(httpPtr->url, -1));

            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj("requestlength", 13));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewLongObj((long)httpPtr->requestLength));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj("sent", 4));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewLongObj((long)httpPtr->sent));

            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj("replylength", 11));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewLongObj((long)httpPtr->replyLength));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewStringObj("received", 8));
            Tcl_ListObjAppendElement(interp, entryObj, Tcl_NewLongObj((long)httpPtr->received));

            Tcl_ListObjAppendElement(interp, resultList, entryObj);
        }
        Tcl_SetObjResult(interp,resultList);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHttpObjCmd --
 *
 *	Implements the new ns_http to handle HTTP requests.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	May queue an HTTP request.
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
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}



/*
 *----------------------------------------------------------------------
 *
 * HttpQueueCmd --
 *
 *	Implements "ns_http queue" subcommand.
 *
 * Results:
 *	Standard Tcl result.
 *
 * Side effects:
 *	May queue an HTTP request.
 *
 *----------------------------------------------------------------------
 */

static int
HttpQueueCmd(
    NsInterp *itPtr,
    int objc,
    Tcl_Obj *const* objv,
    bool run
) {
    Tcl_Interp    *interp;
    int            verifyInt = 0, result = TCL_OK;
    Ns_HttpTask   *httpPtr;
    char          *cert = NULL,
                  *caFile = NULL,
                  *caPath = NULL,
                  *sni_hostname = NULL,
                  *outputFileName = NULL,
                  *method = (char *)"GET",
                  *url = NULL,
                  *bodyFileName = NULL;
    Ns_Set        *requestHdrPtr = NULL,
                  *replyHdrPtr = NULL;
    Tcl_Obj       *bodyPtr = NULL;
    Ns_Time       *timeoutPtr = NULL;
    int            keepInt = 0;

    Ns_ObjvSpec opts[] = {
        {"-body",             Ns_ObjvObj,    &bodyPtr,        NULL},
        {"-body_file",        Ns_ObjvString, &bodyFileName,   NULL},
        {"-cafile",           Ns_ObjvString, &caFile,         NULL},
        {"-capath",           Ns_ObjvString, &caPath,         NULL},
        {"-cert",             Ns_ObjvString, &cert,           NULL},
        {"-headers",          Ns_ObjvSet,    &requestHdrPtr,  NULL},
        {"-hostname",         Ns_ObjvString, &sni_hostname,   NULL},
        {"-keep_host_header", Ns_ObjvBool,   &keepInt,        INT2PTR(NS_TRUE)},
        {"-method",           Ns_ObjvString, &method,         NULL},
        {"-outputfile",       Ns_ObjvString, &outputFileName, NULL},
        {"-timeout",          Ns_ObjvTime,   &timeoutPtr,     NULL},
        {"-verify",           Ns_ObjvBool,   &verifyInt,      NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",       Ns_ObjvString, &url, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);
    interp = itPtr->interp;

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result =  TCL_ERROR;

    } else if (run && CheckReplyHeaders(interp, &replyHdrPtr) != NS_OK) {
        Ns_TclPrintfResult(interp, "ns_http: automatic generation of output headers failed");
        result = TCL_ERROR;

    } else if (HttpConnect(interp, method, url, requestHdrPtr, bodyPtr, bodyFileName,
                           cert, caFile, caPath, sni_hostname,
                           (verifyInt == 1) ? NS_TRUE : NS_FALSE,
                           (keepInt == 1)   ? NS_TRUE : NS_FALSE,
                           timeoutPtr,
                           &httpPtr
                          ) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        /*
         * When the outputFileName is given store it in the task structure. It
         * will be used in case output is spooled to a file.
         */
        if (outputFileName != NULL) {
            httpPtr->spoolFileName = ns_strdup(outputFileName);
        }

        httpPtr->task = Ns_TaskCreate(httpPtr->sock, HttpProc, httpPtr);
        if (run) {
            /*
             * Run the task and set the result dict.
             */
            Tcl_Obj *replyHeadersObj;

            /*
             * Get the replyHeadersObj, which was returned as Tcl result
             * from CheckReplyHeaders()
             */
            replyHeadersObj = Tcl_GetObjResult(interp);
            Tcl_IncrRefCount(replyHeadersObj);

            assert(replyHdrPtr != NULL);

            httpPtr->replyHeaders = replyHdrPtr;
            Ns_TaskRun(httpPtr->task);

            if (httpPtr->error != NULL) {
                Ns_TclPrintfResult(interp, "ns_http failed: %s", httpPtr->error);
                result = TCL_ERROR;
            } else {
                result = SetResult(interp, httpPtr, replyHeadersObj, NULL,
                                   NULL, NULL, NULL);
            }
            Tcl_DecrRefCount(replyHeadersObj);

        } else {

            /*
             * Enqueue the task and return the id of the queued item.
             */
            if (session_queue == NULL) {
                Ns_MasterLock();
                if (session_queue == NULL) {
                    session_queue = Ns_CreateTaskQueue("tclhttp");
                }
                Ns_MasterUnlock();
            }
            if (Ns_TaskEnqueue(httpPtr->task, session_queue) != NS_OK) {
                HttpClose(httpPtr);
                Ns_TclPrintfResult(interp, "could not queue HTTP task");
                result = TCL_ERROR;

            } else {
                Tcl_HashEntry *hPtr;
                uint32_t       i;
                int            len;
                char           buf[TCL_INTEGER_SPACE + 4];

                /*
                 * Create a unique ID for this interp
                 */
                memcpy(buf, "http", 4u);
                for( i = (uint32_t)itPtr->httpRequests.numEntries; ; i++) {
                    int isNew;

                    len = ns_uint32toa(&buf[4], (uint32_t)i);
                    hPtr = Tcl_CreateHashEntry(&itPtr->httpRequests, buf, &isNew);
                    if (isNew != 0) {
                        break;
                    }
                }
                Tcl_SetHashValue(hPtr, httpPtr);
                Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, len+4));
            }
        }

    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpMessageParse --
 *
 *	Parse a HTTP message with its headers. The header fields are parsed
 *      into an Ns_Set (replyHeaders), the other information (major, minor
 *      version numbers, HTTP status, payload) is returned via output args.
 *
 * Results:
 *	Ns_ReturnCode
 *
 * Side effects:
 *	none.
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
                    ns_free((char *)hdrPtr->name);
                }
                hdrPtr->name = ns_strdup(p);
                firsthdr = 0;
            } else if (len < 2 || Ns_ParseHeader(hdrPtr, p, ToLower) != NS_OK) {
                //Ns_Log(Notice, "Ns_ParseHeader of <%s> fails len %lu", p, len);
                break;
            }
            p = eol;
        }
        parsed = (size_t)(p - message);
        // Ns_Log(Notice, "Ns_ParseHeader final p <%s> len %lu parsed %lu", p, size, parsed);

        if (payloadPtr != NULL && (size - parsed) >= 2u) {
            p += 2;
            //Ns_Log(Notice, "Ns_ParseHeader returns payload <%s>", p);
            if (payloadPtr != NULL) {
                *payloadPtr = p;
            }
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ProcessReplyHeaderFields --
 *
 *	Extract information from the reply header field for efficient
 *	processing.
 *
 * Results:
 *	none
 *
 * Side effects:
 *	mit setting flags, might allocate Ns_CompressStream
 *
 *----------------------------------------------------------------------
 */
static void
ProcessReplyHeaderFields(
    Ns_HttpTask *httpPtr
) {
    const char *encString;

    NS_NONNULL_ASSERT(httpPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "ProcessReplyHeaderFields %p", (void *)httpPtr->replyHeaders);

    encString = Ns_SetIGet(httpPtr->replyHeaders, "Content-Encoding");

    if (encString != NULL && strncmp("gzip", encString, 4u) == 0) {
      httpPtr->flags |= NS_HTTP_FLAG_GZIP_ENCODING;

      if ((httpPtr->flags & NS_HTTP_FLAG_GUNZIP) == NS_HTTP_FLAG_GUNZIP) {
          httpPtr->compress = ns_calloc(1u, sizeof(Ns_CompressStream));
          (void) Ns_InflateInit(httpPtr->compress);
      }
    }
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpCheckHeader --
 *
 *	Check, whether we have received a response containing the full
 *	HTTP header yet. If we have so, set the replyHeaderSize in the
 *	Ns_HttpTask structure (to avoid later checking) and terminate
 *	the header string by a '\0'.
 *
 * Results:
 *	none
 *
 * Side effects:
 *	Replace potentially a LF the ds.string terminating the header
 *	by a '\0'
 *
 *----------------------------------------------------------------------
 */

void
Ns_HttpCheckHeader(
    Ns_HttpTask *httpPtr
) {

    NS_NONNULL_ASSERT(httpPtr != NULL);

    if (httpPtr->replyHeaderSize == 0) {
        Ns_MutexLock(&httpPtr->lock);
        if (httpPtr->replyHeaderSize == 0) {
            char *eoh;

            eoh = strstr(httpPtr->ds.string, "\r\n\r\n");
            if (eoh != NULL) {
                httpPtr->replyHeaderSize = (int)(eoh - httpPtr->ds.string) + 4;
                eoh += 2;
                *eoh = '\0';
            } else {
                eoh = strstr(httpPtr->ds.string, "\n\n");
                if (eoh != NULL) {
                    Ns_Log(Warning, "HttpCheckHeader: HTTP client reply contains no CRLF, this should not happen");
                    httpPtr->replyHeaderSize = (int)(eoh - httpPtr->ds.string) + 2;
                    eoh += 1;
                    *eoh = '\0';
                }
            }
        }
        Ns_MutexUnlock(&httpPtr->lock);
    }
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpCheckSpool --
 *
 *	Determine, whether the input processing should result in a
 *	memory string or whether it should spool to a file depending
 *	on the size of the content and the configuration setting
 *	passed in spoolLimit.
 *
 * Results:
 *	none
 *
 * Side effects:
 *	Replace potentially a LF the ds.string terminating the header
 *	by a '\0'
 *
 *----------------------------------------------------------------------
 */
void
Ns_HttpCheckSpool(
    Ns_HttpTask *httpPtr
) {

    NS_NONNULL_ASSERT(httpPtr != NULL);

    /*
     * There is a header, but it is not parsed yet. We are already waiting for
     * the reply, indicated by the available replyHeaders.
     */
    if (httpPtr->replyHeaderSize > 0 && httpPtr->status == 0 && httpPtr->replyHeaders != NULL) {
        size_t contentSize = (size_t)httpPtr->ds.length - (size_t)httpPtr->replyHeaderSize;

        Ns_MutexLock(&httpPtr->lock);
        if (httpPtr->replyHeaderSize > 0 && httpPtr->status == 0) {
            Tcl_WideInt  replyLength = 0;
            const char  *headerField;

            assert(httpPtr->replyHeaders != NULL);

            if ((Ns_HttpMessageParse(httpPtr->ds.string, (size_t)httpPtr->ds.length,
                                     httpPtr->replyHeaders,
                                     NULL, NULL,
                                     &httpPtr->status,
                                     NULL) != NS_OK)
                || (httpPtr->status == 0)
                ) {
                Ns_Log(Warning, "ns_http: Parsing reply header failed");
            }
            ProcessReplyHeaderFields(httpPtr);

            headerField = Ns_SetIGet(httpPtr->replyHeaders, "content-length");
            if (headerField != NULL) {
                (void)Ns_StrToWideInt(headerField, &replyLength);
                /*
                 * Don't get fooled by an invalid content-length received from
                 * the server.
                 */
                if (replyLength < 0) {
                    replyLength = 0;
                }
            }

            /*
             * Either we have to spool (due to spool limit) or we want to
             * spool (a output file name was given).
             */
            httpPtr->replyLength = (size_t)replyLength;
            httpPtr->received = 0u;

            if ((httpPtr->spoolLimit > -1) || (httpPtr->spoolFileName != NULL)) {

                if ((replyLength > 0 && replyLength >= httpPtr->spoolLimit )
                    || (int)contentSize >= httpPtr->spoolLimit
                    || httpPtr->spoolFileName != NULL
                    ) {
                    int fd;

                    /*
                     * We have either
                     * - a valid reply length, which is larger
                     *   than the spool limit, or
                     * - the we have an actual content larger
                     *   than the spool limit.
                     */
                    if (httpPtr->spoolFileName != NULL) {
                        fd = ns_open(httpPtr->spoolFileName, O_WRONLY|O_CREAT, 0644);
                    } else {
                        /*
                         * Create a temporary spool file and remember its fd
                         * finally in httpPtr->spoolFd to flag that later
                         * receives will write there.
                         */
                        char * fileName;
                        size_t fileNameLength;

                        fileNameLength = strlen(nsconf.tmpDir) + 13u;
                        fileName = ns_malloc(fileNameLength);
                        snprintf(fileName, fileNameLength, "%s/http.XXXXXX", nsconf.tmpDir);
                        fd = ns_mkstemp(fileName);
                        httpPtr->spoolFileName = fileName;
                    }

                    if (fd == NS_INVALID_FD) {
                      Ns_Log(Error, "ns_http: cannot create spool file with template '%s': %s",
                             httpPtr->spoolFileName, strerror(errno));

                    } else {
                        Ns_Log(Ns_LogTaskDebug, "ns_http: we spool %lu bytes to fd %d", contentSize, fd);
                        httpPtr->spoolFd = fd;
                        Ns_HttpAppendBuffer(httpPtr,
                                            httpPtr->ds.string + httpPtr->replyHeaderSize,
                                            contentSize);
                    }
                }
            }
        }
        Ns_MutexUnlock(&httpPtr->lock);

        if (contentSize > 0u && httpPtr->spoolFd == 0) {
            Tcl_DString ds, *dsPtr = &ds;

            /*
             * We have in httpPtr->ds the header and some content. We
             * might have to decompress the first content chunk and to
             * replace the compressed content with the decompressed.
             */

            Ns_Log(Ns_LogTaskDebug, "ns_http: got header %d + %" PRIdz " bytes", httpPtr->replyHeaderSize, contentSize);

            Tcl_DStringInit(dsPtr);
            Tcl_DStringAppend(dsPtr, httpPtr->ds.string + httpPtr->replyHeaderSize, (int)contentSize);
            Tcl_DStringSetLength(&httpPtr->ds, httpPtr->replyHeaderSize);
            Ns_HttpAppendBuffer(httpPtr, dsPtr->string, contentSize);

            Tcl_DStringFree(dsPtr);
        }
    }
}



/*
 *----------------------------------------------------------------------
 *
 * HttpGet --
 *
 *	Locate and optionally remove the Http struct for a given id.
 *
 * Results:
 *	NS_TRUE on success, NS_FALSE otherwise.
 *
 * Side effects:
 *	Will update given httpPtrPtr with pointer to Http struct.
 *
 *----------------------------------------------------------------------
 */

static bool
HttpGet(
    NsInterp *itPtr,
    const char *id,
    Ns_HttpTask **httpPtrPtr,
    bool removeRequest
) {
    Tcl_HashEntry *hPtr;
    bool           success = NS_TRUE;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(id != NULL);
    NS_NONNULL_ASSERT(httpPtrPtr != NULL);

    hPtr = Tcl_FindHashEntry(&itPtr->httpRequests, id);
    if (hPtr == NULL) {
        Ns_TclPrintfResult(itPtr->interp, "no such request: %s", id);
        success = NS_FALSE;
    } else {
        *httpPtrPtr = Tcl_GetHashValue(hPtr);
        if (removeRequest) {
            Tcl_DeleteHashEntry(hPtr);
        }
    }
    return success;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpLocationString --
 *
 *	Build a HTTP location string following the IP literation notation in
 *	RFC 3986 section 3.2.2 if needed and return in the provided
 *	Tcl_DString. In case protoString is non-null, perpend the protocol. In
 *	case port != defPort, append the port.
 *
 * Results:
 *
 *	location strings such as e.g.
 *          [2001:db8:1f70::999:de8:7648:6e8]:8000    (IP-literal notation)
 *          https://openacs.org                       (reg-name notation)
 *
 * Side effects:
 *
 *	Updating Tcl_DString
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
 * Ns_HttpParseHost --
 *
 *	Obtain the host name from a writable string from a syntax as specified
 *	in RFC 3986 section 3.2.2.
 *      Examples:
 *          [2001:db8:1f70::999:de8:7648:6e8]:8000    (IP-literal notation)
 *          openacs.org:80                            (reg-name notation)
 *
 * Results:
 *
 *	As a result, "portStart" will point to the terminating
 *      char (e.g. ':') if the host name. If a port is indicated after the host
 *	name, the variable "portStart" will return a string starting with ":",
 *	otherwise NULL. If "hostStart" is non-null, a pointer will point to the
 *	host name, which will be terminated by char 0 in case of a IPv6 address
 *      in IP-literal notation.
 *
 * Side effects:
 *
 *	Will potentially write a null character into the string passed in
 *	"hostString".
 *
 *----------------------------------------------------------------------
 */
void
Ns_HttpParseHost(
    char *hostString,
    char **hostStart,
    char **portStart
) {
    bool ip_literal = NS_FALSE;

    NS_NONNULL_ASSERT(hostString != NULL);
    NS_NONNULL_ASSERT(portStart != NULL);

    if (*hostString == '[') {
        char *p;

        /*
         * Maybe this is an address in IP-literal notation in square braces
         */
        p = strchr(hostString + 1, INTCHAR(']'));
        if (p != NULL) {
            ip_literal = NS_TRUE;

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
    if (!ip_literal) {
        char *slash = strchr(hostString, INTCHAR('/')),
                *colon = strchr(hostString, INTCHAR(':'));
        if ((slash != NULL) && (colon != NULL) && (slash < colon)) {
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
 * WaitState --
 *
 *        Wait until the specified socket is writable.
 *
 * Results:
 *        Writable socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *        None
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
WaitState(
    NS_SOCKET sock,
    short events,
    Ns_Time *timeout

) {
    Ns_ReturnCode result = NS_ERROR;

    if (sock != NS_INVALID_SOCKET) {
          struct pollfd  pollfd;
          int            retval;

          pollfd.revents = 0;
          pollfd.events = events;
          pollfd.fd = sock;

          for (;;) {
              int pollto = 1000;
              //fprintf(stderr, "##### WaitState: timeout %p events %.4x\n", (void*)timeout, pollfd.events);

              if (timeout != NULL) {
                  Ns_Time diff, now;

                  /*
                   * A timeout is specified with an already set due time.
                   * Return NS_TIMEOUT, when we run into this timeout.
                   */
                  Ns_GetTime(&now);

                  if (Ns_DiffTime(timeout, &now, &diff) > 0) {
                      pollto = (int)(diff.sec * 1000 + diff.usec / 1000 + 1);
                  }
                  //fprintf(stderr, "##### WaitState: pollto %d events %.4x\n", pollto, pollfd.events);
                  retval = ns_poll(&pollfd, 1, pollto);
                  /*fprintf(stderr, "##### call poll on %d %" PRId64 ".%06ld events %.4x pollto %d => %d revents %.4x\n",
                    sock, (int64_t) timeout->sec, timeout->usec, pollfd.events, pollto, retval, pollfd.revents);*/
                  // ns_http run http://naviserver.sourceforge.net/n/naviserver/files/commandlist.htmlx
                  // ns_http run https://naviserver.sourceforge.net/n/naviserver/files/commandlist.html
                  // ns_http run https://naviserver.sourceforge.io/n/naviserver/files/commandlist.html
                  // ns_http run https://google.com/
                  if (retval < 1) {
                      result = NS_TIMEOUT;
                  }
                  break;
              } else {
                  /*
                   * No timeout is specified. Retry, until we run into an
                   * error or success.
                   */
                  retval = ns_poll(&pollfd, 1, pollto);
                  if (retval != 0) {
                      break;
                  }
              }
          }
          if (retval == 1) {
              result = NS_OK;
          }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * EnsureWritable --
 *
 *        Call WaitState() and perform Tcl error handling
 *
 * Results:
 *        Standard Tcl result code
 *
 * Side effects:
 *        Might set interp result.
 *
 *----------------------------------------------------------------------
 */
static int
EnsureWritable(
    Tcl_Interp  *interp,
    Ns_HttpTask *httpPtr,
    const char *url
) {
    Ns_ReturnCode rc;
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    /*
     * Make sure, the socket is in a writable state.
     */
    rc = WaitState(httpPtr->sock, POLLOUT, &httpPtr->timeout);
    if (rc != NS_OK) {
        if (rc == NS_TIMEOUT) {
            Ns_TclPrintfResult(interp, "ns_http failed: timeout");
            Tcl_SetErrorCode(interp, "NS_TIMEOUT", (char *)0L);
        } else {
            Ns_TclPrintfResult(interp, "connect to \"%s\" failed: %s",
                               url, ns_sockstrerror(ns_sockerrno));
        }
        httpPtr->sock = NS_INVALID_SOCKET;
        result = TCL_ERROR;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * HttpConnect --
 *
 *        Open a connection to the given URL host and construct
 *        an Http structure to fetch the file.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        Updates httpPtrPtr with newly allocated Http struct
 *        on success.
 *
 *----------------------------------------------------------------------
 */
static int
HttpConnect(
    Tcl_Interp *interp,
    const char *method,
    const char *url,
    Ns_Set *hdrPtr,
    Tcl_Obj *bodyPtr,
    const char *bodyFileName,
    const char *cert,
    const char *caFile,
    const char *caPath,
    const char *sni_hostname,
    bool verify,
    bool keep_host_header,
    const Ns_Time *timeoutPtr,
    Ns_HttpTask **httpPtrPtr
) {
    NS_SOCKET        sock = NS_INVALID_SOCKET;
    Ns_HttpTask     *httpPtr;
    int              result, uaFlag = -1, bodyFileFd = 0;
    off_t            bodyFileSize = 0;
    unsigned short   defaultPort, portNr;
    char            *url2, *protocol, *host, *portString, *path, *tail;
    const char      *contentType = NULL;
    Tcl_DString     *dsPtr;
    Ns_Time          timeoutConnect, *timeoutConnectPtr;
    static uint64_t  httpClientRequestCount = 0u;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(httpPtrPtr != NULL);

    /*
     * If host_keep_header set then "Host:" header field must be present.
     */
    if (keep_host_header) {
        if ( (hdrPtr == NULL) || (Ns_SetIFind(hdrPtr, "Host") == -1) ) {
            Ns_TclPrintfResult(interp, "keep_host_header specified but no Host header given");
            return TCL_ERROR;
        }
    }

    /*
     * Make a non-const copy of url, in which Ns_ParseUrl can replace the item
     * separating characters with '\0' characters. Make sure that we always
     * free URLs before leaving this function. We accept a fully qualified URL.
     */
    url2 = ns_strdup(url);
    if ((Ns_ParseUrl(url2, &protocol, &host, &portString, &path, &tail) != NS_OK)
        || (protocol == NULL) || (host == NULL) || (path == NULL) || (tail == NULL)
        ) {
        Ns_TclPrintfResult(interp, "invalid URL \"%s\"", url);
        goto fail;
    }

    assert(protocol != NULL);
    assert(host != NULL);
    assert(path != NULL);
    assert(tail != NULL);

    /*
     * Check used protocol and protocol-specific parameters
     */
    if (STREQ("http", protocol)) {
        if ( (cert != NULL) || (caFile != NULL) || (caPath != NULL) || verify ) {
            Ns_TclPrintfResult(interp, "https-specific parameters are only allowed for https URLs");
            goto fail;
        }
        defaultPort = 80u;
    }
#ifdef HAVE_OPENSSL_EVP_H
    else if (STREQ("https", protocol)) {
        defaultPort = 443u;
    }
#endif
    else {
        Ns_TclPrintfResult(interp, "invalid url: %s", url);
        goto fail;
    }

    /*
     * Use the specified port or the default port.
     */
    if (portString != NULL) {
        portNr = (unsigned short) strtol(portString, NULL, 10);
    } else {
        portNr = defaultPort;
    }

    Ns_Log(Ns_LogTaskDebug, "connect to [%s]:%hu", host, portNr);

    {
        Ns_ReturnCode status;

        if (timeoutPtr != NULL) {
            timeoutConnectPtr = (Ns_Time *)timeoutPtr;
        } else {
            timeoutConnectPtr = &timeoutConnect;
            timeoutConnect.sec = 2;
            timeoutConnect.usec = 0;
        }
        sock = Ns_SockTimedConnect2(host, portNr, NULL, 0, timeoutConnectPtr, &status);

        if (sock == NS_INVALID_SOCKET) {
            Ns_SockConnectError(interp, host, portNr, status);
            goto fail;
        }

        if (Ns_SockSetNonBlocking(sock) != NS_OK) {
            Ns_Log(Warning, "attempt to set socket nonblocking failed");
        }
    }

    if ((bodyPtr != NULL) || (bodyFileName != NULL)) {
        if ((bodyPtr != NULL) && (bodyFileName != NULL)) {
            Ns_TclPrintfResult(interp, "either -body or -body_file may be specified");
            goto fail;
        }

        if (hdrPtr != NULL) {
            contentType = Ns_SetIGet(hdrPtr, "Content-Type");
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
                contentType = "application/octet-stream";
            }
        }
        if (bodyFileName != NULL) {
            struct stat bodyStat;

            if (Ns_Stat(bodyFileName, &bodyStat) == NS_FALSE) {
                Ns_TclPrintfResult(interp, "cannot stat file: %s ", bodyFileName);
                goto fail;
            }
            bodyFileSize = bodyStat.st_size;

            bodyFileFd = ns_open(bodyFileName, O_RDONLY, 0);
            if (unlikely(bodyFileFd == NS_INVALID_FD)) {
                Ns_TclPrintfResult(interp, "cannot open file %s", bodyFileName);
                goto fail;
            }
        }
    }

    /*
     * All error checking from parameter processing is done, allocate the
     * Ns_HttpTask structure.
     */

    httpPtr = ns_calloc(1u, sizeof(Ns_HttpTask));
    httpPtr->sock           = sock;
    httpPtr->spoolLimit     = -1;
    httpPtr->url            = ns_strdup(url);
    httpPtr->bodyFileFd     = bodyFileFd;
    httpPtr->sendSpoolMode  = NS_FALSE;
    httpPtr->infoObj        = NULL;

    /*
     * Handling timeouts
     */
    Ns_GetTime(&httpPtr->stime);
    httpPtr->timeout = httpPtr->stime;

    if (timeoutPtr != NULL) {
        Ns_IncrTime(&httpPtr->timeout, timeoutPtr->sec, timeoutPtr->usec);
    } else {
        Ns_IncrTime(&httpPtr->timeout, 2, 0);
    }

    /*
     * Prevent sockclose attempts in fail cases.
     */
    sock = NS_INVALID_SOCKET;


    dsPtr = &httpPtr->ds;
    Tcl_DStringInit(dsPtr);

    /*
     * Initialize the mutex. Provide a name for the task and use the static
     * part of the dsPtr as temporary storage.
     */
    Ns_MutexInit(&httpPtr->lock);

    Ns_MasterLock();
    httpClientRequestCount++;
    Ns_MasterUnlock();
    (void)ns_uint64toa(dsPtr->string, httpClientRequestCount);
    Ns_MutexSetName2(&httpPtr->lock, "ns:httptask", dsPtr->string);
    
    /*
     * Determine if connection is HTTP or HTTPS via default port.
     */
    if (defaultPort == 443u) {
        NS_TLS_SSL_CTX *ctx;
        NS_TLS_SSL     *ssl;

        /*
         * We have a HTTPS URL; initialize OpenSSL context and establish SSL/TLS connection
         */
        result = Ns_TLS_CtxClientCreate(interp, cert, caFile, caPath, verify, &ctx);
        httpPtr->ctx = ctx;

        if (likely(result == TCL_OK)) {
            /*
             * Make sure, the socket is in a writable state.
             */
            result = EnsureWritable(interp, httpPtr, url);
            /*fprintf(stderr, "### Ns_TLS_CtxClientCreate ok and writable\n");*/
        }
        if (likely(result == TCL_OK)) {
            /*
             * Establish the SSL/TLS connection.
             */
            result = Ns_TLS_SSLConnect(interp, httpPtr->sock, ctx, sni_hostname, &ssl);
            httpPtr->ssl = ssl;
            /*fprintf(stderr, "### Ns_TLS_CtxClientCreate connection established => %s\n",  result == TCL_OK ? "OK" : "ERROR");*/
            HttpTaskAddInfo(httpPtr, "sslversion", SSL_get_version(ssl));
            HttpTaskAddInfo(httpPtr, "cipher", SSL_get_cipher(ssl));
        }
    } else {
        /*
         * We have a HTTP URL.
         */

        result = EnsureWritable(interp, httpPtr, url);
    }

    if (unlikely(result != TCL_OK)) {
        /*
         * Finish everything up. HttpClose() frees httpPtr.
         */
        HttpClose(httpPtr);
        goto fail;
    }

    Ns_DStringAppend(dsPtr, method);
    Ns_StrToUpper(Ns_DStringValue(dsPtr));
    Tcl_DStringAppend(dsPtr, " /", 2);
    if (*path != '\0') {
        Tcl_DStringAppend(dsPtr, path, -1);
        Tcl_DStringAppend(dsPtr, "/", 1);
    }
    Tcl_DStringAppend(dsPtr, tail, -1);
    Tcl_DStringAppend(dsPtr, " HTTP/1.0\r\n", 11);

    /*
     * Submit provided headers
     */
    if (hdrPtr != NULL) {
        size_t i;

        /*
         * Remove the header fields, we are providing
         */
        if (!keep_host_header) {
            Ns_SetIDeleteKey(hdrPtr, "Host");
        }
        Ns_SetIDeleteKey(hdrPtr, "Connection");
        Ns_SetIDeleteKey(hdrPtr, "Content-Length");

        for (i = 0u; i < Ns_SetSize(hdrPtr); i++) {
            const char *key = Ns_SetKey(hdrPtr, i);
            if (uaFlag != 0) {
                uaFlag = strcasecmp(key, "User-Agent");
            }
            Ns_DStringPrintf(dsPtr, "%s: %s\r\n", key, Ns_SetValue(hdrPtr, i));
        }
    }

    /*
     * No keep-alive even in case of HTTP 1.1
     */
    Ns_DStringAppend(dsPtr, "Connection: close\r\n");

    /*
     * User-Agent header was not supplied, add our own
     */
    if (uaFlag != 0) {
        Ns_DStringPrintf(dsPtr, "User-Agent: %s/%s\r\n",
                         Ns_InfoServerName(),
                         Ns_InfoServerVersion());
    }

    if (!keep_host_header) {
        Ns_DStringNAppend(dsPtr, "Host: ", 6);
        (void)Ns_HttpLocationString(dsPtr, NULL, host, portNr, defaultPort);
        Ns_DStringNAppend(dsPtr, "\r\n", 2);
    }

    /*
     * The body of the request might be specified via Tcl_Obj containing the
     * content, or via filename.
     */
    if ((bodyPtr != NULL) || (bodyFileName != NULL)) {

        if (bodyFileName == NULL) {
            int         length = 0;
            const char *bodyString;
            bool        binary = NsTclObjIsByteArray(bodyPtr);

            if (contentType != NULL && !binary) {
                binary = Ns_IsBinaryMimeType(contentType);
            }

            if (binary) {
                bodyString = (void *)Tcl_GetByteArrayFromObj(bodyPtr, &length);
            } else {
                bodyString = Tcl_GetStringFromObj(bodyPtr, &length);
            }
            Ns_DStringPrintf(dsPtr, "Content-Length: %d\r\n\r\n", length);
            Tcl_DStringAppend(dsPtr, bodyString, length);

        } else {
            Ns_DStringPrintf(dsPtr, "Content-Length: %" PROTd "\r\n\r\n", bodyFileSize);
        }

    } else {
        Tcl_DStringAppend(dsPtr, "\r\n", 2);
    }

    {
#if 0
        char *reqString =
                "GET /HandInservice/HandInService.asmx HTTP/1.1\r\n"
                "Host: services.ephorus.com\r\n"
                "User-Agent: curl/7.61.0\r\n"
                "Accept: */*\r\n"
                "\r\n";
        dsPtr->string = ns_strdup(reqString);
#endif

        httpPtr->next = dsPtr->string;
        httpPtr->requestLength = (size_t)dsPtr->length;

        //httpPtr->requestLength = strlen(reqString);

        httpPtr->sent = 0u;

    }
    Ns_Log(Ns_LogTaskDebug, "full request <%s>", dsPtr->string);

    *httpPtrPtr = httpPtr;
    ns_free(url2);

    return TCL_OK;

 fail:
    ns_free(url2);
    if (sock != NS_INVALID_SOCKET) {
        ns_sockclose(sock);
    }
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HttpAppendBuffer, HttpAppendRawBuffer --
 *
 *        The HTTP client has received some content. Append this
 *        content either raw or uncompressed to either a file
 *        descriptor or the Tcl_DString. HttpAppendRawBuffer appends
 *        data without any decompression.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        Writing to the spool file or appending to the Tcl_DString
 *
 *----------------------------------------------------------------------
 */

static int
HttpAppendRawBuffer(
    Ns_HttpTask *httpPtr,
    const char *buffer,
    size_t outSize
) {
    int status = TCL_OK;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (httpPtr->spoolFd > 0) {
        ssize_t written = ns_write(httpPtr->spoolFd, buffer, outSize);

        if (written == -1) {
            Ns_Log(Error, "task: spooling of received content failed");
            status = TCL_ERROR;
        }
    } else {
        Tcl_DStringAppend(&httpPtr->ds, buffer, (int)outSize);
    }

    return status;
}

int
Ns_HttpAppendBuffer(
    Ns_HttpTask *httpPtr,
    const char *buffer,
    size_t inSize
) {
    int result;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    Ns_Log(Ns_LogTaskDebug, "Ns_HttpAppendBuffer: got %" PRIdz " bytes flags %.6x", inSize, httpPtr->flags);

    if (likely((httpPtr->flags & NS_HTTP_FLAG_GUNZIP) != NS_HTTP_FLAG_GUNZIP)) {
        /*
         * Output raw content
         */
        result = HttpAppendRawBuffer(httpPtr, buffer, inSize);

    } else {
        char out[16384];

        out[0] = '\0';
        /*
         * Output decompressed content
         */
        (void) Ns_InflateBufferInit(httpPtr->compress, buffer, inSize);
        Ns_Log(Ns_LogTaskDebug, "InflateBuffer: got %" PRIdz " compressed bytes", inSize);
        do {
            size_t uncompressedLen = 0u;

            result = Ns_InflateBuffer(httpPtr->compress, out, sizeof(out), &uncompressedLen);
            Ns_Log(Ns_LogTaskDebug, "InflateBuffer status %d uncompressed %" PRIdz " bytes", result, uncompressedLen);

            if (HttpAppendRawBuffer(httpPtr, out, uncompressedLen) != TCL_OK) {
                result = TCL_ERROR;
            }

        } while(result == TCL_CONTINUE);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * HttpClose --
 *
 *        Finish Http Task and cleanup memory
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
    Ns_HttpTask *httpPtr
) {
    NS_NONNULL_ASSERT(httpPtr != NULL);

    if (httpPtr->task != NULL) {
        (void) Ns_TaskFree(httpPtr->task);
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
    if (httpPtr->sock > 0) {
        ns_sockclose(httpPtr->sock);
    }
    if (httpPtr->spoolFileName != NULL) {
        ns_free((char*)httpPtr->spoolFileName);
    }
    if (httpPtr->spoolFd > 0) {
        (void) ns_close(httpPtr->spoolFd);
    }
    if (httpPtr->bodyFileFd > 0) {
        (void) ns_close(httpPtr->bodyFileFd);
    }
    if (httpPtr->compress != NULL)      {
        (void) Ns_InflateEnd(httpPtr->compress);
        ns_free(httpPtr->compress);
    }
    if (httpPtr->infoObj != NULL) {
        Tcl_DecrRefCount(httpPtr->infoObj);
        httpPtr->infoObj = NULL;
    }
    Ns_MutexDestroy(&httpPtr->lock);
    Tcl_DStringFree(&httpPtr->ds);
    ns_free((char *)httpPtr->url);
    ns_free(httpPtr);
}


static void
HttpCancel(
    const Ns_HttpTask *httpPtr
) {
    NS_NONNULL_ASSERT(httpPtr != NULL);

    (void) Ns_TaskCancel(httpPtr->task);
    (void) Ns_TaskWait(httpPtr->task, NULL);
}


static void
HttpAbort(
    Ns_HttpTask *httpPtr
) {
    NS_NONNULL_ASSERT(httpPtr != NULL);

    HttpCancel(httpPtr);
    HttpClose(httpPtr);
}


static void
HttpTaskAddInfo(
    Ns_HttpTask *httpPtr,
    const char *attribute,
    const char *value
) {
    if (httpPtr->infoObj == NULL) {
        httpPtr->infoObj = Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(httpPtr->infoObj);
    }
    Tcl_ListObjAppendElement(NULL, httpPtr->infoObj, Tcl_NewStringObj(attribute, -1));
    Tcl_ListObjAppendElement(NULL, httpPtr->infoObj, Tcl_NewStringObj(value, -1));
}


/*
 *----------------------------------------------------------------------
 *
 * HttpTaskSend --
 *
 *        Send the specified buffer via plain TCP or via OpenSSL. The function
 *        is designed to with non-locking I/O (and therefore partial "send"
 *        operations). The function will terminate, when either the whole
 *        buffer was sent, or when an error occurs. The function is
 *        essentially a definition of the unix "ns_send" operation based for
 *        Ns_HttpTask.
 *
 * Results:
 *        sent size or -1 on error
 *
 * Side effects:
 *        Sending data.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
HttpTaskSend(
    const Ns_HttpTask *httpPtr,
    const void *buffer,
    size_t length
) {
    ssize_t       sent;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (httpPtr->ssl == NULL) {
        sent = ns_send(httpPtr->sock, httpPtr->ds.string, length, 0);
    } else {
#ifdef HAVE_OPENSSL_EVP_H
        struct iovec  iov;
        (void) Ns_SetVec(&iov, 0, buffer, length);

        sent = 0;
        for (;;) {
            int     err;
            ssize_t n;

            /*fprintf(stderr, "### HttpTaskSend wants to send %ld\n", iov.iov_len);*/
            n = SSL_write(httpPtr->ssl, iov.iov_base, (int)iov.iov_len);
            err = SSL_get_error(httpPtr->ssl, (int)n);
            /*fprintf(stderr, "### HttpTaskSend n %ld err %d\n", n, err);*/
            if (err == SSL_ERROR_WANT_WRITE) {
                Ns_Time timeout = { 0, 10000 }; /* 10ms */

                (void) Ns_SockTimedWait(httpPtr->sock, NS_SOCK_WRITE, &timeout);
                continue;
            } else if (err != 0) {
                Ns_Log(Ns_LogTaskDebug, "HttpTaskSend %d: got unexpected reply %d (url %s)",
                       httpPtr->sock, err, httpPtr->url);
            }
            if (likely(n > -1)) {
                sent += n;

                if (((size_t)n < iov.iov_len)) {
                    (void)Ns_ResetVec(&iov, 1, (size_t)n);
                    continue;
                }
            }
            break;
        }
#else
        sent = -1;
#endif
    }

    Ns_Log(Ns_LogTaskDebug, "HttpTaskSend sent %ld bytes (from %lu)", sent, length);
    return sent;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpTaskRecv --
 *
 *        Receive data plain TCP or via OpenSSL. The function essentially a
 *        generalization of "ns_recv" for Ns_HttpTask.
 *
 * Results:
 *        sent size or -1 on error
 *
 * Side effects:
 *        Sending data.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
HttpTaskRecv(
    const Ns_HttpTask *httpPtr,
    char *buffer,
    size_t length
) {
    ssize_t       received;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (httpPtr->ssl == NULL) {
        received = ns_recv(httpPtr->sock, buffer, length, 0);
    } else {
#ifdef HAVE_OPENSSL_EVP_H
        //fprintf(stderr, "### HttpTaskRecv SSL_read want %lu\n", length);

        received = 0;
        for (;;) {
            int n, err;

            n = SSL_read(httpPtr->ssl, buffer+received, (int)(length - (size_t)received));
            err = SSL_get_error(httpPtr->ssl, n);
            //fprintf(stderr, "### HttpTaskRecv SSL_read n %d err %d (pending %d)\n", n, err, SSL_pending(httpPtr->ssl));
            switch (err) {
            case SSL_ERROR_NONE:
                if (n < 0) {
                    Ns_Log(Error, "HttpTaskRecv SSL_read failed but no error, should not happen");
                    break;
                }
                received += n;
                break;

            case SSL_ERROR_WANT_READ: {
                Ns_Time timeout = { 0, 10000 }; /* 10ms */

                //fprintf(stderr, "### HttpTaskRecv partial read, n %d\n", (int)n);
                if (n > 0) {
                    received += n;
                }
                (void) Ns_SockTimedWait(httpPtr->sock, NS_SOCK_READ, &timeout);
                continue;
            }

            case SSL_ERROR_ZERO_RETURN:
                /*
                 * The TLS/SSL connection has been closed.
                 */
                break;
                
            case SSL_ERROR_SYSCALL:
                /*
                 * The TLS/SSL connection has been closed.
                 */
                Ns_Log(Notice, "HttpTaskRecv: connection probably closed by server (url %s)",
                       httpPtr->url);
                break;
                
            default: {
                char errorBuffer[256];

                Ns_Log(Warning, "HttpTaskRecv got unexpected error code %d message %s (url %s)", err,
                       ERR_error_string((unsigned long)err, errorBuffer), httpPtr->url);
                break;
            }
            }
            break;
        }
#else
        received = -1;
#endif
    }

    Ns_Log(Ns_LogTaskDebug, "HttpTaskRecv received %ld bytes (from %lu)", received, length);
    return received;
}

#if 0

/*
 *----------------------------------------------------------------------
 *
 * HttpTaskShutdown --
 *
 *        Shutdown sending data
 *
 * Results:
 *        None
 *
 * Side effects:
 *        effecting socket and SSL.
 *
 *----------------------------------------------------------------------
 */

static void
HttpTaskShutdown(
    const Ns_HttpTask *httpPtr,
    int mode
) {
    NS_NONNULL_ASSERT(httpPtr != NULL);

#ifdef HAVE_OPENSSL_EVP_H
    if (httpPtr->ssl != NULL) {
        SSL_set_shutdown(httpPtr->ssl, mode);
    }
#endif
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * HttpProc --
 *
 *        Task callback for ns_http connections.
 *
 * Results:
 *        None.
 *
 * Side effects:
 *        Will call Ns_TaskCallback and Ns_TaskDone to manage state
 *        of task.
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
    Ns_HttpTask *httpPtr;
    char         buf[CHUNK_SIZE];
    ssize_t      n;
    bool         taskDone = NS_TRUE;

    NS_NONNULL_ASSERT(task != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    httpPtr = arg;
    Ns_Log(Ns_LogTaskDebug, "HttpProc operation %.2x", why);

    switch (why) {
    case NS_SOCK_INIT:
        Ns_TaskCallback(task, NS_SOCK_WRITE, &httpPtr->timeout);
        taskDone = NS_FALSE;
        break;

    case NS_SOCK_WRITE:
        /*
         * Send the request data either from the Tcl_DString, or from a file. The
         * latter case is flagged via member "sendSpoolMode".
         */
        if (httpPtr->sendSpoolMode) {
            Ns_Log(Ns_LogTaskDebug, "HttpProc read data from file, buffer size %d", Tcl_DStringLength(&httpPtr->ds));
            n = ns_read(httpPtr->bodyFileFd, httpPtr->ds.string, CHUNK_SIZE);
            if (n < 0) {
                httpPtr->error = "read failed";
            } else {
                Ns_Log(Ns_LogTaskDebug, "HttpProc send read data from file");
                n = HttpTaskSend(httpPtr, httpPtr->ds.string, (size_t)n);
                if (n < 0) {
                    httpPtr->error = "send failed";
                } else {
                    if (n < CHUNK_SIZE) {
                        Ns_Log(Ns_LogTaskDebug, "HttpProc all data spooled, switch to read reply");
                        //HttpTaskShutdown(httpPtr, SSL_SENT_SHUTDOWN);  /* should actually work */
                        Tcl_DStringSetLength(&httpPtr->ds, 0);
                        Ns_TaskCallback(task, NS_SOCK_READ, &httpPtr->timeout);
                    }
                    taskDone = NS_FALSE;
                }
            }
        } else {
            n = HttpTaskSend(httpPtr, httpPtr->next, httpPtr->requestLength - httpPtr->sent);
            if (n < 0) {
                httpPtr->error = "send failed";
            } else {
                ssize_t remaining;

                httpPtr->next += n;
                httpPtr->sent += (size_t)n;

                remaining = (ssize_t)(httpPtr->requestLength - httpPtr->sent);

                if (remaining == 0) {
                    /*
                     * All data from ds has been sent. Check, if there is a file to
                     * append, and if yes, switch to sendSpoolMode.
                     */
                    if (httpPtr->bodyFileFd > 0) {
                        httpPtr->sendSpoolMode = NS_TRUE;
                        Ns_Log(Ns_LogTaskDebug, "HttpProc all data sent, switch to spool mode using fd %d",
                               httpPtr->bodyFileFd);
                        Tcl_DStringSetLength(&httpPtr->ds, CHUNK_SIZE);
                    } else {
                        Ns_Log(Ns_LogTaskDebug, "HttpProc all data sent, switch to read reply");
                        //HttpTaskShutdown(httpPtr, SSL_SENT_SHUTDOWN);  /* should actually work */
                        Tcl_DStringSetLength(&httpPtr->ds, 0);
                        Ns_TaskCallback(task, NS_SOCK_READ, &httpPtr->timeout);
                    }
                } else {
                    Ns_Log(Ns_LogTaskDebug, "HttpProc sent %ld bytes from memory, remaining %ld",
                           n, remaining);
                }
                taskDone = NS_FALSE;
            }
        }
        break;

    case NS_SOCK_READ:
        if (httpPtr->sent == 0u) {
            n = -1;
        } else {

            n = HttpTaskRecv(httpPtr, buf, sizeof(buf));
        }
        if (likely(n > 0)) {

            /*
             * Spooling is only activated after (a) having processed
             * the headers, and (b) after the wait command has
             * required to spool. Once we know spoolFd, there is no
             * need to HttpCheckHeader() again.
             */
            httpPtr->received += (size_t)n;

            if (httpPtr->spoolFd > 0) {
                (void) Ns_HttpAppendBuffer(httpPtr, buf, (size_t)n);
            } else {
                Ns_Log(Ns_LogTaskDebug, "Task got %d bytes", (int)n);

                (void) Ns_HttpAppendBuffer(httpPtr, buf, (size_t)n);

                if (unlikely(httpPtr->replyHeaderSize == 0)) {
                    Ns_HttpCheckHeader(httpPtr);
                }
                /*
                 * Ns_HttpCheckSpool might set httpPtr->spoolFd
                 */
                Ns_HttpCheckSpool(httpPtr);
                /*Ns_Log(Ns_LogTaskDebug, "Task got %d bytes, header = %d", (int)n, httpPtr->replyHeaderSize);*/
            }
            taskDone = NS_FALSE;
        }

        if (n < 0) {
            Ns_Log(Warning, "client HTTP request: receive failed, error: %s", strerror(errno));
            httpPtr->error = "recv failed";
        }
        break;

    case NS_SOCK_DONE:
        taskDone = NS_FALSE;
        break;

    case NS_SOCK_TIMEOUT:
        httpPtr->error = "timeout";
        break;

    case NS_SOCK_EXIT:
        httpPtr->error = "shutdown";
        break;

    case NS_SOCK_CANCEL:
        httpPtr->error = "cancelled";
        break;

    case NS_SOCK_EXCEPTION:
        httpPtr->error = "exception";
        break;
    }
    httpPtr->finalSockState = why;

    if (taskDone) {
        /*
         * Get completion time and mark task as done.
         */

        Ns_GetTime(&httpPtr->etime);
        Ns_TaskDone(httpPtr->task);
    }
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
