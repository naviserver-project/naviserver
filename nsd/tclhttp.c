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

/* 
 * The maximum chunk size from TLS is 2^14 => 16384 (see RFC 5246). OpenSSL
 * can't send more than this number of bytes in one attempt.
 */
#define CHUNK_SIZE 16384 

/*
 * Local functions defined in this file
 */

static int HttpQueueCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, int run)
    NS_GNUC_NONNULL(1);
static int HttpConnect(Tcl_Interp *interp, const char *method, const char *url,
                       Ns_Set *hdrPtr, Tcl_Obj *bodyPtr, const char *bodyFileName,
                       const char *cert, const char *caFile, const char *caPath, bool verify,
                       bool keep_host_header, Ns_HttpTask **httpPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(12);

static bool HttpGet(NsInterp *itPtr, const char *id, Ns_HttpTask **httpPtrPtr, bool removeRequest)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void HttpClose(Ns_HttpTask *httpPtr)  NS_GNUC_NONNULL(1);
static void HttpCancel(const Ns_HttpTask *httpPtr) NS_GNUC_NONNULL(1);
static void HttpAbort(Ns_HttpTask *httpPtr)  NS_GNUC_NONNULL(1);

static int HttpAppendRawBuffer(Ns_HttpTask *httpPtr, const char *buffer, size_t outSize) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void ProcessReplyHeaderFields(Ns_HttpTask *httpPtr) 
    NS_GNUC_NONNULL(1);

static NS_SOCKET WaitWritable(NS_SOCKET sock);

static ssize_t HttpTaskSend(const Ns_HttpTask *httpPtr, const void *buffer, size_t length)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static ssize_t HttpTaskRecv(const Ns_HttpTask *httpPtr, char *buffer, size_t length)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void HttpTaskShutdown(const Ns_HttpTask *httpPtr)
    NS_GNUC_NONNULL(1);

static Ns_TaskProc HttpProc;

static Tcl_ObjCmdProc HttpCancelObjCmd;
static Tcl_ObjCmdProc HttpCleanupObjCmd;
static Tcl_ObjCmdProc HttpListObjCmd;
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
HttpRunObjCmd(ClientData clientData, Tcl_Interp *UNUSED(interp), int objc, Tcl_Obj *CONST* objv)
{
    return HttpQueueCmd(clientData, objc, objv, 1);
}

static int
HttpQueueObjCmd(ClientData clientData, Tcl_Interp *UNUSED(interp), int objc, Tcl_Obj *CONST* objv)
{
    return HttpQueueCmd(clientData, objc, objv, 0);
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
HttpWaitObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp    *itPtr = clientData;
    Tcl_Obj     *valPtr, 
	        *elapsedVarPtr = NULL, *resultVarPtr = NULL, 
	        *statusVarPtr = NULL, *fileVarPtr = NULL;
    Ns_Time     *timeoutPtr = NULL;
    const char  *id = NULL;
    Ns_Set      *hdrPtr = NULL;
    Ns_HttpTask *httpPtr = NULL;
    Ns_Time      diff;
    int          result = TCL_ERROR, spoolLimit = -1, decompress = 0;

    Ns_ObjvSpec opts[] = {
        {"-timeout",    Ns_ObjvTime,   &timeoutPtr,    NULL},
        {"-headers",    Ns_ObjvSet,    &hdrPtr,        NULL},
        {"-elapsed",    Ns_ObjvObj,    &elapsedVarPtr, NULL},
        {"-result",     Ns_ObjvObj,    &resultVarPtr,  NULL},
        {"-status",     Ns_ObjvObj,    &statusVarPtr,  NULL},
        {"-file",       Ns_ObjvObj,    &fileVarPtr,    NULL},
        {"-spoolsize",  Ns_ObjvInt,    &spoolLimit,    NULL},
        {"-decompress", Ns_ObjvBool,   &decompress,    INT2PTR(NS_TRUE)},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"id",       Ns_ObjvString, &id, NULL},
        {NULL, NULL, NULL, NULL}
    };

    NS_NONNULL_ASSERT(itPtr != NULL);

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (HttpGet(itPtr, id, &httpPtr, NS_TRUE) == NS_FALSE) {
	return TCL_ERROR;
    }
    if (decompress != 0) {
      httpPtr->flags |= NS_HTTP_FLAG_DECOMPRESS;
    }

    if (hdrPtr == NULL) {
      /*
       * If no output headers are provided, we create our
       * own. The ns_set is needed for checking the content
       * length of the reply.
       */
      hdrPtr = Ns_SetCreate("outputHeaders");
      if (unlikely(Ns_TclEnterSet(interp, hdrPtr, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
          Ns_SetFree(hdrPtr);
          return TCL_ERROR;
      }
    }
    httpPtr->spoolLimit = spoolLimit;
    httpPtr->replyHeaders = hdrPtr;

    Ns_HttpCheckSpool(httpPtr);

    if (Ns_TaskWait(httpPtr->task, timeoutPtr) != NS_OK) {
	HttpCancel(httpPtr);
        Ns_TclPrintfResult(interp, "timeout waiting for task");
	return TCL_ERROR;
    }

    if (elapsedVarPtr != NULL) {
    	Ns_DiffTime(&httpPtr->etime, &httpPtr->stime, &diff);
	valPtr = Tcl_NewObj();
    	Ns_TclSetTimeObj(valPtr, &diff);
    	if (Ns_SetNamedVar(interp, elapsedVarPtr, valPtr) == NS_FALSE) {
	    goto err;
	}
    }

    if (httpPtr->error != NULL) {
        Ns_TclPrintfResult(interp, "ns_http failed: %s", httpPtr->error);
	goto err;
    }

    if (httpPtr->replyHeaderSize == 0) {
	Ns_HttpCheckHeader(httpPtr);
    }
    Ns_HttpCheckSpool(httpPtr);

    if (statusVarPtr != NULL 
	&& Ns_SetNamedVar(interp, statusVarPtr, Tcl_NewIntObj(httpPtr->status)) == NS_FALSE) {
	goto err;
    }

    if (httpPtr->spoolFd > 0)  {
	(void) ns_close(httpPtr->spoolFd);
	valPtr = Tcl_NewObj();
    } else {
        bool binary = NS_TRUE;

        if (hdrPtr != NULL) {
            const char *contentEncoding = Ns_SetIGet(hdrPtr, "Content-Encoding");
            
            /*
             * Does the contentEncoding allow text transfers? Not, if the
             * content is compressed.
             */

            if (contentEncoding == NULL || strncmp(contentEncoding, "gzip", 4u) != 0) {
                const char *contentType = Ns_SetIGet(hdrPtr, "Content-Type");
                
                if (contentType != NULL) {
                    /*
                     * Determine binary via contentType
                     */
                    binary = Ns_IsBinaryMimeType(contentType);
                }
            }
        }

        if (binary)  {
            valPtr = Tcl_NewByteArrayObj((unsigned char*)httpPtr->ds.string + httpPtr->replyHeaderSize, 
                                         (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
        } else {
            valPtr = Tcl_NewStringObj(httpPtr->ds.string + httpPtr->replyHeaderSize, 
                                      (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
        }
    }

    if (fileVarPtr != NULL 
	&& httpPtr->spoolFd > 0 
	&& Ns_SetNamedVar(interp, fileVarPtr, Tcl_NewStringObj(httpPtr->spoolFileName, -1)) == NS_FALSE) {
	goto err;
    }

    if (resultVarPtr == NULL) {
	Tcl_SetObjResult(interp, valPtr);
    } else {
	if (Ns_SetNamedVar(interp, resultVarPtr, valPtr) == NS_FALSE) {
	    goto err;
	}
	Tcl_SetBooleanObj(Tcl_GetObjResult(interp), 1);
    }

    result = TCL_OK;

err:
    HttpClose(httpPtr);
    return result;
}


static int
HttpCancelObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp    *itPtr = clientData;
    const char  *idString;
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
 *	Aborting requests and reinitializiung hash table.
 *
 *----------------------------------------------------------------------
 */
static int
HttpCleanupObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
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
HttpListObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
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
NsTclHttpObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"cancel",   HttpCancelObjCmd},
        {"cleanup",  HttpCleanupObjCmd},
        {"list",     HttpListObjCmd},
        {"queue",    HttpQueueObjCmd},
        {"run",      HttpRunObjCmd},
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
HttpQueueCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, int run)
{
    Tcl_Interp    *interp;
    int            verifyInt = 0, result = TCL_OK;
    Ns_HttpTask   *httpPtr;
    const char    *cert = NULL, *caFile = NULL, *caPath = NULL;
    const char    *method = "GET", *url = NULL, *bodyFileName = NULL;
    Ns_Set        *hdrPtr = NULL;
    Tcl_Obj       *bodyPtr = NULL;
    const Ns_Time *timeoutPtr = NULL;
    int            keepInt = 0;

    Ns_ObjvSpec opts[] = {
        {"-body",             Ns_ObjvObj,    &bodyPtr,      NULL},
        {"-body_file",        Ns_ObjvString, &bodyFileName, NULL},
        {"-cafile",           Ns_ObjvString, &caFile,       NULL},
        {"-capath",           Ns_ObjvString, &caPath,       NULL},
        {"-cert",             Ns_ObjvString, &cert,         NULL},
        {"-headers",          Ns_ObjvSet,    &hdrPtr,       NULL},
        {"-keep_host_header", Ns_ObjvBool,   &keepInt,      INT2PTR(NS_TRUE)},
        {"-method",           Ns_ObjvString, &method,       NULL},
        {"-timeout",          Ns_ObjvTime,   &timeoutPtr,   NULL},
        {"-verify",           Ns_ObjvBool,   &verifyInt,    NULL},
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

    } else if (HttpConnect(interp, method, url, hdrPtr, bodyPtr, bodyFileName,
                           cert, caFile, caPath,
                           verifyInt == 1 ? NS_TRUE : NS_FALSE,
                           keepInt == 1 ? NS_TRUE : NS_FALSE,
                           &httpPtr) != TCL_OK) {
	result = TCL_ERROR;

    } else {
        Tcl_HashEntry *hPtr;
        char           buf[TCL_INTEGER_SPACE + 4];
        int            isNew, i;

        Ns_GetTime(&httpPtr->stime);
        httpPtr->timeout = httpPtr->stime;
    
        if (timeoutPtr != NULL) {
            Ns_IncrTime(&httpPtr->timeout, timeoutPtr->sec, timeoutPtr->usec);
        } else {
            Ns_IncrTime(&httpPtr->timeout, 2, 0);
        }
    
        httpPtr->task = Ns_TaskCreate(httpPtr->sock, HttpProc, httpPtr);
        if (run != 0) {
            Ns_TaskRun(httpPtr->task);
        } else {
            if (session_queue == NULL) {
                Ns_MasterLock();
                if (session_queue == NULL) {
                    session_queue = Ns_CreateTaskQueue("tclhttp");
                }
                Ns_MasterUnlock();
            }
            if (Ns_TaskEnqueue(httpPtr->task, session_queue) != NS_OK) {
                HttpClose(httpPtr);
                Ns_TclPrintfResult(interp, "could not queue http task");
                return TCL_ERROR;
            }
        }

        /*
         * Create a unique ID for this interp
         */
        i = itPtr->httpRequests.numEntries;
        do {
            snprintf(buf, sizeof(buf), "http%d", i++);
            hPtr = Tcl_CreateHashEntry(&itPtr->httpRequests, buf, &isNew);
        } while (isNew == 0);
        Tcl_SetHashValue(hPtr, httpPtr);

        Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, -1));
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * HttpParseReplyHeader --
 *
 *	Parse header fields of a response into NsSet replyHeaders and 
 *      update the status.
 *
 * Results:
 *	none
 *
 * Side effects:
 *	none.
 *
 *----------------------------------------------------------------------
 */
static void
HttpParseHeaders(char *response, Ns_Set *hdrPtr, int *statusPtr) 
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void
HttpParseHeaders(char *response, Ns_Set *hdrPtr, int *statusPtr)
{
    char *p, *eol;
    int firsthdr = 1, major, minor;

    NS_NONNULL_ASSERT(hdrPtr != NULL);
    NS_NONNULL_ASSERT(response != NULL);
    NS_NONNULL_ASSERT(statusPtr != NULL);

    sscanf(response, "HTTP/%2d.%2d %3d", &major, &minor, statusPtr);
    p = response;
    while ((eol = strchr(p, INTCHAR('\n'))) != NULL) {
	size_t len;
	
	*eol++ = '\0';
	len = strlen(p);
	if (len > 0u && p[len - 1u] == '\r') {
	    p[len - 1u] = '\0';
	}
	if (firsthdr != 0) {
	    if (hdrPtr->name != NULL) {
		ns_free((char *)hdrPtr->name);
	    }
	    hdrPtr->name = ns_strdup(p);
	    firsthdr = 0;
	} else if (Ns_ParseHeader(hdrPtr, p, ToLower) != NS_OK) {
	    break;
	}
	p = eol;
    }
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
ProcessReplyHeaderFields(Ns_HttpTask *httpPtr) 
{
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
 *	Update potentially a lf the ds.string terminating the header
 *	by a '\0'
 *
 *----------------------------------------------------------------------
 */

void
Ns_HttpCheckHeader(Ns_HttpTask *httpPtr)
{
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
		    Ns_Log(Warning, "HttpCheckHeader: http client reply contains no crlf, this should not happen");
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
 *	Update potentially a lf the ds.string terminating the header
 *	by a '\0'
 *
 *----------------------------------------------------------------------
 */
void
Ns_HttpCheckSpool(Ns_HttpTask *httpPtr)
{
    NS_NONNULL_ASSERT(httpPtr != NULL);

    /*
     * There is a header, but it is not parsed yet. We are already waiting for
     * the reply, indicated by the available replyHeaders.
     */
    if (httpPtr->replyHeaderSize > 0 && httpPtr->status == 0 && httpPtr->replyHeaders != NULL) {
	size_t contentSize = (size_t)httpPtr->ds.length - (size_t)httpPtr->replyHeaderSize;

	Ns_MutexLock(&httpPtr->lock);
	if (httpPtr->replyHeaderSize > 0 && httpPtr->status == 0) {
	    Tcl_WideInt length;

	    assert(httpPtr->replyHeaders != NULL);
	    HttpParseHeaders(httpPtr->ds.string, httpPtr->replyHeaders, &httpPtr->status);
	    if (httpPtr->status == 0) {
		Ns_Log(Warning, "ns_http: Parsing reply header failed");
	    }
	    ProcessReplyHeaderFields(httpPtr);

	    if (httpPtr->spoolLimit > -1) {
	        const char *s = Ns_SetIGet(httpPtr->replyHeaders, "content-length");

		if ((s != NULL
		     && Ns_StrToWideInt(s, &length) == NS_OK
                     && length > 0 
		     && length >= httpPtr->spoolLimit
		     ) || (int)contentSize >= httpPtr->spoolLimit
		    ) {
		    int fd;
                    char *spoolFileName;
                    
		    /*
		     * We have a valid reply length, which is larger
		     * than the spool limit, or the we have an actual
		     * content larger than the limit. Create a
		     * temporary spool file and rember its fd finally
		     * in httpPtr->spoolFd to flag that later receives
		     * will write there.
		     */
		    spoolFileName = ns_malloc(strlen(nsconf.tmpDir) + 13u);
		    sprintf(spoolFileName, "%s/http.XXXXXX", nsconf.tmpDir);
                    httpPtr->spoolFileName = spoolFileName;
		    fd = ns_mkstemp(spoolFileName);
		    
		    if (fd == NS_INVALID_FD) {
		      Ns_Log(Error, "ns_http: cannot create spool file with template '%s': %s", 
			     httpPtr->spoolFileName, strerror(errno));
		    }
		    
		    if (fd != 0) {
		      /*Ns_Log(Ns_LogTaskDebug, "ns_http: we spool %d bytes to fd %d", contentSize, fd); */
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
	    Tcl_DStringTrunc(&httpPtr->ds, httpPtr->replyHeaderSize);
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
HttpGet(NsInterp *itPtr, const char *id, Ns_HttpTask **httpPtrPtr, bool removeRequest)
{
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
 *	Build a http location string following the IP literation notation in
 *	RFC 3986 section 3.2.2 if needed and return in in the provided
 *	DString. In case protoString is non-null, perpend the protocol. In
 *	case port != defPort, append the the port.
 *
 * Results:
 *
 *	location strings such as e.g.
 *          [2001:db8:1f70::999:de8:7648:6e8]:8000    (IP-literal notation)
 *          https://openacs.org                       (reg-name notation)
 *
 * Side effects:
 *
 *	Updating DString
 *
 *----------------------------------------------------------------------
 */
char *
Ns_HttpLocationString(Tcl_DString *dsPtr, const char *protoString, const char *hostString,
                      unsigned short port, unsigned short defPort)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(hostString != NULL);

    if (protoString != NULL) {
        Ns_DStringVarAppend(dsPtr, protoString, "://", NULL);
    }
    if (strchr(hostString, INTCHAR(':')) != NULL) {
        Ns_DStringVarAppend(dsPtr, "[", hostString, "]", NULL);
    } else {
        Ns_DStringVarAppend(dsPtr, hostString, NULL);
    }
    if (port != defPort) {
        (void) Ns_DStringPrintf(dsPtr, ":%d", port);
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
Ns_HttpParseHost(char *hostString, char **hostStart, char **portStart)
{
    bool ipv6 = NS_FALSE;
    
    NS_NONNULL_ASSERT(hostString != NULL);
    NS_NONNULL_ASSERT(portStart != NULL);
    
    if (*hostString == '[') {
        char *p;
        
        /*
         * Maybe this is an IPv6 address in square braces
         */
        p = strchr(hostString + 1, INTCHAR(']'));
        if (p != NULL) {
            ipv6 = NS_TRUE;
            
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
    if (!ipv6) {
        *portStart = strchr(hostString, INTCHAR(':'));
        if (hostStart != NULL) {
            *hostStart = hostString;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * WaitWritable --
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
static NS_SOCKET
WaitWritable(NS_SOCKET sock) {
    if (sock != NS_INVALID_SOCKET) {
          struct pollfd  pollfd;
          int retval;

          pollfd.events = POLLOUT;
          pollfd.fd = sock;

          for (;;) {
              /*fprintf(stderr, "# call poll on %d\n", sock);*/
              retval = ns_poll(&pollfd, 1, 1000);
              /*fprintf(stderr, "# call poll on %d => %d\n", sock, retval);*/
              if (retval != 0) {
                  break;
              }
          }
          if (retval != 1) {
              /*fprintf(stderr, "# ... make sock %d invalid \n", sock);*/
              sock = NS_INVALID_SOCKET;
          }
    }
    return sock;
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
HttpConnect(Tcl_Interp *interp, const char *method, const char *url,
            Ns_Set *hdrPtr, Tcl_Obj *bodyPtr, const char *bodyFileName,
            const char *cert, const char *caFile, const char *caPath, bool verify,
            bool keep_host_header, Ns_HttpTask **httpPtrPtr)
{
    NS_SOCKET      sock = NS_INVALID_SOCKET;
    Ns_HttpTask   *httpPtr;
    int            result, uaFlag = -1, bodyFileSize = 0, bodyFileFd = 0;
    unsigned short defaultPort = 0u, portNr;
    char          *url2, *protocol, *host, *portString, *path, *tail;
    const char    *contentType = NULL;
    Tcl_DString   *dsPtr;

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
     * free urls before leaving this function.
     */
    url2 = ns_strdup(url);
    if (Ns_ParseUrl(url2, &protocol, &host, &portString, &path, &tail) != NS_OK) {
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
            Ns_TclPrintfResult(interp, "https-specific parameters are only allowed for https urls");
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

    sock = Ns_SockAsyncConnect(host, portNr);
    if (sock == NS_INVALID_SOCKET) {
	Ns_TclPrintfResult(interp, "connect to \"%s\" failed: %s",
                           url, ns_sockstrerror(ns_sockerrno));
        goto fail;
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
            Ns_TclPrintfResult(interp, "header field Content-Type is required when body is provided");
            goto fail;
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

    /*
     * Prevent sockclose in fail cases.
     */
    sock = NS_INVALID_SOCKET;
    Ns_MutexInit(&httpPtr->lock);
    /*Ns_MutexSetName(&httpPtr->lock, name, buffer);*/

    dsPtr = &httpPtr->ds;
    Tcl_DStringInit(dsPtr);

    /*
     * Initialize OpenSSL context and establish SSL/TLS connection for https.
     */
    if (defaultPort == 443u) {
        NS_TLS_SSL_CTX *ctx;
        NS_TLS_SSL     *ssl;

        result = Ns_TLS_CtxClientCreate(interp, cert, caFile, caPath, verify, &ctx);
        httpPtr->ctx = ctx;
        
        if (likely(result == TCL_OK)) {
            /*
             * Make sure, the socket is in a writable state.
             */
            httpPtr->sock = WaitWritable(httpPtr->sock);
            /*
             * Establish the SSL/TLS connection.
             */
            result = Ns_TLS_SSLConnect(interp, httpPtr->sock, ctx, &ssl);
            httpPtr->ssl = ssl;
        }
        if (unlikely(result != TCL_OK)) {
            HttpClose(httpPtr);
            goto fail;
        }
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
        (void)Ns_HttpLocationString(dsPtr, NULL, host, portNr, 80u);
        Ns_DStringNAppend(dsPtr, "\r\n", 2);
    }

    /*
     * The body of the request might be specified via Tcl_Obj containing the
     * content, or via filename.
     */
    if ((bodyPtr != NULL) || (bodyFileName != NULL)) {

        if (bodyFileName == NULL) {
            int length = 0;
            const char *bodyString;
            bool binary = NsTclObjIsByteArray(bodyPtr);

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
            Ns_DStringPrintf(dsPtr, "Content-Length: %d\r\n\r\n", bodyFileSize);
        }

    } else {
        Tcl_DStringAppend(dsPtr, "\r\n", 2);
    }

    httpPtr->next = dsPtr->string;
    httpPtr->len = (size_t)dsPtr->length;

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
 *        descriptor or the the DString. HttpAppendRawBuffer appends
 *        data without any decompression.
 *
 * Results:
 *        Tcl result code
 *
 * Side effects:
 *        Writing to the spool file or appending to the DString
 *
 *----------------------------------------------------------------------
 */

static int
HttpAppendRawBuffer(Ns_HttpTask *httpPtr, const char *buffer, size_t outSize) 
{
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
Ns_HttpAppendBuffer(Ns_HttpTask *httpPtr, const char *buffer, size_t inSize) 
{
    int result = TCL_OK;

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
HttpClose(Ns_HttpTask *httpPtr)
{
    NS_NONNULL_ASSERT(httpPtr != NULL);

    if (httpPtr->task != NULL)          {(void) Ns_TaskFree(httpPtr->task);}
#ifdef HAVE_OPENSSL_EVP_H    
    if (httpPtr->ssl != NULL) {
        SSL_shutdown(httpPtr->ssl);
        SSL_free(httpPtr->ssl);
    }
    if (httpPtr->ctx != NULL)           {SSL_CTX_free(httpPtr->ctx);}
#endif    
    if (httpPtr->sock > 0)              {ns_sockclose(httpPtr->sock);}
    if (httpPtr->spoolFileName != NULL) {ns_free((char*)httpPtr->spoolFileName);}
    if (httpPtr->spoolFd > 0)           {(void) ns_close(httpPtr->spoolFd);}
    if (httpPtr->bodyFileFd > 0)        {(void) ns_close(httpPtr->bodyFileFd);}
    if (httpPtr->compress != NULL)      {
	(void) Ns_InflateEnd(httpPtr->compress);
	ns_free(httpPtr->compress);
    }
    Ns_MutexDestroy(&httpPtr->lock);
    Tcl_DStringFree(&httpPtr->ds);
    ns_free((char *)httpPtr->url);
    ns_free(httpPtr);
}


static void
HttpCancel(const Ns_HttpTask *httpPtr)
{
    NS_NONNULL_ASSERT(httpPtr != NULL);

    (void) Ns_TaskCancel(httpPtr->task);
    (void) Ns_TaskWait(httpPtr->task, NULL);
}


static void
HttpAbort(Ns_HttpTask *httpPtr)
{
    NS_NONNULL_ASSERT(httpPtr != NULL);

    HttpCancel(httpPtr);
    HttpClose(httpPtr);
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
HttpTaskSend(const Ns_HttpTask *httpPtr, const void *buffer, size_t length)
{
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
        
            n = SSL_write(httpPtr->ssl, iov.iov_base, (int)iov.iov_len);
            err = SSL_get_error(httpPtr->ssl, n);
            if (err == SSL_ERROR_WANT_WRITE) {
                Ns_Time timeout = { 0, 10000 }; /* 10ms */
                
                (void) Ns_SockTimedWait(httpPtr->sock, NS_SOCK_WRITE, &timeout);
                continue;
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
HttpTaskRecv(const Ns_HttpTask *httpPtr, char *buffer, size_t length)
{
    ssize_t       received;

    NS_NONNULL_ASSERT(httpPtr != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (httpPtr->ssl == NULL) {
        received = ns_recv(httpPtr->sock, buffer, length, 0);
    } else {
#ifdef HAVE_OPENSSL_EVP_H
        /*fprintf(stderr, "### SSL_read want %lu\n", length);*/

	received = 0;
        for (;;) {
            int n, err;
            
	    n = SSL_read(httpPtr->ssl, buffer+received, (int)(length - (size_t)received));
	    err = SSL_get_error(httpPtr->ssl, n);
	    /*fprintf(stderr, "### SSL_read n %ld got %lu err %d\n", n, received, err);*/
	    switch (err) {
	    case SSL_ERROR_NONE: 
		if (n < 0) { 
		    Ns_Log(Error, "SSL_read failed but no error, should not happen"); 
		    break;
		}
		received += n;
		break;

	    case SSL_ERROR_WANT_READ: 
		/*fprintf(stderr, "### partial read, n %d\n", (int)n); */
		received += n;
		continue;
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
HttpTaskShutdown(const Ns_HttpTask *httpPtr)
{
    NS_NONNULL_ASSERT(httpPtr != NULL);

#ifdef HAVE_OPENSSL_EVP_H
    if (httpPtr->ssl != NULL) {
        SSL_set_shutdown(httpPtr->ssl, SSL_SENT_SHUTDOWN);
    }
#endif    
}


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
HttpProc(Ns_Task *task, NS_SOCKET UNUSED(sock), void *arg, Ns_SockState why)
{
    Ns_HttpTask *httpPtr;
    char         buf[CHUNK_SIZE];
    ssize_t      n;
    bool         taskDone = NS_TRUE;

    NS_NONNULL_ASSERT(task != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    httpPtr = arg;

    switch (why) {
    case NS_SOCK_INIT:
	Ns_TaskCallback(task, NS_SOCK_WRITE, &httpPtr->timeout);
        taskDone = NS_FALSE;
        break;

    case NS_SOCK_WRITE:
        /*
         * Send the request data either from the DString, or from a file. The
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
                        HttpTaskShutdown(httpPtr);
                        Tcl_DStringTrunc(&httpPtr->ds, 0);
                        Ns_TaskCallback(task, NS_SOCK_READ, &httpPtr->timeout);
                    }
                    taskDone = NS_FALSE;
                }
            }
        } else {
            n = HttpTaskSend(httpPtr, httpPtr->next, httpPtr->len);
            if (n < 0) {
                httpPtr->error = "send failed";
            } else {
                httpPtr->next += n;
                httpPtr->len -= (size_t)n;
                Ns_Log(Ns_LogTaskDebug, "HttpProc sent %ld bytes from memory, remaining %lu", n, httpPtr->len);

                if (httpPtr->len == 0u) {
                    /*
                     * All data from ds has been sent. Check, if there is a file to
                     * append, and if yes, switch to sendSpoolMode.
                     */
                    if (httpPtr->bodyFileFd > 0) {
                        httpPtr->sendSpoolMode = NS_TRUE;
                        Ns_Log(Ns_LogTaskDebug, "HttpProc all data sent, switch to spool mode using fd %d", httpPtr->bodyFileFd);
                        Tcl_DStringTrunc(&httpPtr->ds, CHUNK_SIZE);
                    } else {
                        Ns_Log(Ns_LogTaskDebug, "HttpProc all data sent, switch to read reply");
                        HttpTaskShutdown(httpPtr);
                        Tcl_DStringTrunc(&httpPtr->ds, 0);
                        Ns_TaskCallback(task, NS_SOCK_READ, &httpPtr->timeout);
                    }
                }
                taskDone = NS_FALSE;
            }
        }
	break;

    case NS_SOCK_READ:
    	n = HttpTaskRecv(httpPtr, buf, sizeof(buf));

    	if (likely(n > 0)) {

	    /* 
	     * Spooling is only activated after (a) having processed
	     * the headers, and (b) after the wait command has
	     * required to spool. Once we know spoolFd, there is no
	     * need to HttpCheckHeader() again.
	     */
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
            Ns_Log(Warning, "client http request: receive failed, error: %s\n", strerror(errno));
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
 * End:
 */
