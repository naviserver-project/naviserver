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
 * Local functions defined in this file
 */

static int HttpWaitCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv)
    NS_GNUC_NONNULL(1);
static int HttpQueueCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, int run)
    NS_GNUC_NONNULL(1);
static int HttpConnect(Tcl_Interp *interp, const char *method, char *url,
			Ns_Set *hdrPtr, Tcl_Obj *bodyPtr, Ns_HttpTask **httpPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(6);

static bool HttpGet(NsInterp *itPtr, CONST char *id, Ns_HttpTask **httpPtrPtr, int remove)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void HttpClose(Ns_HttpTask *httpPtr)  NS_GNUC_NONNULL(1);
static void HttpCancel(const Ns_HttpTask *httpPtr) NS_GNUC_NONNULL(1);
static void HttpAbort(Ns_HttpTask *httpPtr)  NS_GNUC_NONNULL(1);

static int HttpAppendRawBuffer(Ns_HttpTask *httpPtr, CONST char *buffer, size_t outSize) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void ProcessReplyHeaderFields(Ns_HttpTask *httpPtr) 
    NS_GNUC_NONNULL(1);

static Ns_TaskProc HttpProc;

/*
 * Static variables defined in this file.
 */

static Ns_TaskQueue *queue;


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
NsTclHttpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp *itPtr = arg;
    Ns_HttpTask *httpPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    int result, opt, run = 0;

    static const char *opts[] = {
       "cancel", "cleanup", "run", "queue", "wait", "list",
       NULL
    };
    enum {
        HCancelIdx, HCleanupIdx, HRunIdx, HQueueIdx, HWaitIdx, HListIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?args ...?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    assert(itPtr != NULL);

    switch (opt) {
    case HRunIdx:
	run = 1;
	/* FALLTHROUGH */
    case HQueueIdx:
	result = HttpQueueCmd(itPtr, objc, objv, run);
        break;

    case HWaitIdx:
	result = HttpWaitCmd(itPtr, objc, objv);
        break;

    case HCancelIdx:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "id");
            result = TCL_ERROR;
        } else {
            result = TCL_OK;
            if (HttpGet(itPtr, Tcl_GetString(objv[2]), &httpPtr, 1) == NS_FALSE) {
                result = TCL_ERROR;
            }
	}
        if (result == TCL_OK) {
            HttpAbort(httpPtr);
        }
        break;

    case HCleanupIdx:
        hPtr = Tcl_FirstHashEntry(&itPtr->https, &search);
        while (hPtr != NULL) {
            httpPtr = Tcl_GetHashValue(hPtr);
            HttpAbort(httpPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&itPtr->https);
        Tcl_InitHashTable(&itPtr->https, TCL_STRING_KEYS);
        result = TCL_OK;
        break;

    case HListIdx:
        hPtr = Tcl_FirstHashEntry(&itPtr->https, &search);
        while (hPtr != NULL) {
            httpPtr = Tcl_GetHashValue(hPtr);
            Tcl_AppendResult(interp, Tcl_GetHashKey(&itPtr->https, hPtr), " ",
                             httpPtr->url, " ",
                             Ns_TaskCompleted(httpPtr->task) ? "done" : "running",
                             " ", NULL);
            hPtr = Tcl_NextHashEntry(&search);
        }
        result = TCL_OK;
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        result = TCL_ERROR;
        break;
    }
    return result;
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
    Tcl_Interp *interp;
    int isNew, i;
    Tcl_HashEntry *hPtr;
    Ns_HttpTask *httpPtr;
    char buf[TCL_INTEGER_SPACE + 4], *url = NULL;
    char *method = "GET";
    Ns_Set *hdrPtr = NULL;
    Tcl_Obj *bodyPtr = NULL;
    Ns_Time *incrPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-timeout",  Ns_ObjvTime,   &incrPtr,  NULL},
        {"-headers",  Ns_ObjvSet,    &hdrPtr,   NULL},
        {"-method",   Ns_ObjvString, &method,   NULL},
        {"-body",     Ns_ObjvObj,    &bodyPtr,  NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",       Ns_ObjvString, &url, NULL},
        {NULL, NULL, NULL, NULL}
    };

    assert(itPtr != NULL);
    interp = itPtr->interp;

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (HttpConnect(interp, method, url, hdrPtr, bodyPtr, &httpPtr) != TCL_OK) {
	return TCL_ERROR;
    }
    Ns_GetTime(&httpPtr->stime);
    httpPtr->timeout = httpPtr->stime;
    if (incrPtr != NULL) {
        Ns_IncrTime(&httpPtr->timeout, incrPtr->sec, incrPtr->usec);
    } else {
        Ns_IncrTime(&httpPtr->timeout, 2, 0);
    }
    httpPtr->task = Ns_TaskCreate(httpPtr->sock, HttpProc, httpPtr);
    if (run != 0) {
	Ns_TaskRun(httpPtr->task);
    } else {
	if (queue == NULL) {
	    Ns_MasterLock();
	    if (queue == NULL) {
		queue = Ns_CreateTaskQueue("tclhttp");
	    }
	    Ns_MasterUnlock();
	}
	if (Ns_TaskEnqueue(httpPtr->task, queue) != NS_OK) {
	    HttpClose(httpPtr);
	    Tcl_AppendResult(interp, "could not queue http task", NULL);
	    return TCL_ERROR;
	}
    }
    i = itPtr->https.numEntries;
    do {
        snprintf(buf, sizeof(buf), "http%d", i++);
        hPtr = Tcl_CreateHashEntry(&itPtr->https, buf, &isNew);
    } while (isNew == 0);
    Tcl_SetHashValue(hPtr, httpPtr);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, -1));
    return TCL_OK;
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

    assert(hdrPtr != NULL);
    assert(response != NULL);
    assert(statusPtr != NULL);

    sscanf(response, "HTTP/%2d.%2d %3d", &major, &minor, statusPtr);
    p = response;
    while ((eol = strchr(p, '\n')) != NULL) {
	size_t len;
	
	*eol++ = '\0';
	len = strlen(p);
	if (len > 0 && p[len - 1u] == '\r') {
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
    char *encString;

    assert(httpPtr != NULL);

    Ns_Log(Debug, "ProcessReplyHeaderFields %p", (void *)httpPtr->replyHeaders);

    encString = Ns_SetIGet(httpPtr->replyHeaders, "Content-Encoding");

    if (encString != NULL && strncmp("gzip", encString, 4U) == 0) {
      httpPtr->flags |= NS_HTTP_FLAG_GZIP_ENCODING;

      if ((httpPtr->flags & NS_HTTP_FLAG_GUNZIP) == NS_HTTP_FLAG_GUNZIP) {
	  httpPtr->compress = ns_calloc(1U, sizeof(Ns_CompressStream));
	  Ns_InflateInit(httpPtr->compress);
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
    char *eoh;

    assert(httpPtr != NULL);

    if (httpPtr->replyHeaderSize == 0) {
	Ns_MutexLock(&httpPtr->lock);
	if (httpPtr->replyHeaderSize == 0) {
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
    assert(httpPtr != NULL);

    /*
     * There is a header, but it is not parsed yet.
     */
    if (httpPtr->replyHeaderSize > 0 && httpPtr->status == 0) {
	int contentSize = httpPtr->ds.length - httpPtr->replyHeaderSize;

	Ns_MutexLock(&httpPtr->lock);
	if (httpPtr->replyHeaderSize > 0 && httpPtr->status == 0) {
	    Tcl_WideInt length;

	    assert(httpPtr->replyHeaders);
	    HttpParseHeaders(httpPtr->ds.string, httpPtr->replyHeaders, &httpPtr->status);
	    if (httpPtr->status == 0) {
		Ns_Log(Warning, "ns_http: Parsing reply header failed");
	    }
	    ProcessReplyHeaderFields(httpPtr);

	    if (httpPtr->spoolLimit > -1) {
	        char *s = Ns_SetIGet(httpPtr->replyHeaders, "content-length");

		if ((s != NULL
		     && Ns_StrToWideInt(s, &length) == NS_OK && length > 0 
		     && length >= httpPtr->spoolLimit
		     ) || contentSize >= httpPtr->spoolLimit
		    ) {
		    int fd;
		    /*
		     * We have a valid reply length, which is larger
		     * than the spool limit, or the we have an actual
		     * content larger than the limit. Create a
		     * temporary spool file and rember its fd finally
		     * in httpPtr->spoolFd to flag that later receives
		     * will write there.
		     */
		    httpPtr->spoolFileName = ns_malloc(strlen(nsconf.tmpDir) + 13U);
		    sprintf(httpPtr->spoolFileName, "%s/http.XXXXXX", nsconf.tmpDir);
		    fd = ns_mkstemp(httpPtr->spoolFileName);
		    
		    if (fd == -1) {
		      Ns_Log(Error, "ns_http: cannot create spool file with template '%s': %s", 
			     httpPtr->spoolFileName, strerror(errno));
		    }
		    
		    if (fd != 0) {
		      /*Ns_Log(Notice, "ns_http: we spool %d bytes to fd %d", contentSize, fd); */
			httpPtr->spoolFd = fd;
			Ns_HttpAppendBuffer(httpPtr, 
					    httpPtr->ds.string + httpPtr->replyHeaderSize, 
					    contentSize);
		    }
		}
	    }
	}
	Ns_MutexUnlock(&httpPtr->lock);

	if (contentSize > 0 && httpPtr->spoolFd == 0) {
	    Tcl_DString ds, *dsPtr = &ds;

	    /*
	     * We have in httpPtr->ds the header and some content. We
	     * might have to decompress the first content chunk and to
	     * replace the compressed content with the decompressed.
	     */

	    Ns_Log(Debug, "ns_http: got header %d + %d bytes", httpPtr->replyHeaderSize, contentSize);

	    Tcl_DStringInit(dsPtr);
	    Tcl_DStringAppend(dsPtr, httpPtr->ds.string + httpPtr->replyHeaderSize, contentSize);
	    Tcl_DStringTrunc(&httpPtr->ds, httpPtr->replyHeaderSize);
	    Ns_HttpAppendBuffer(httpPtr, dsPtr->string, contentSize);

	    Tcl_DStringFree(dsPtr);
	}
    }
}


/*
 *----------------------------------------------------------------------
 *
 * HttpWaitCmd --
 *
 *	Implements "ns_http wait" subcommand.
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
HttpWaitCmd(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv)
{
    Tcl_Interp  *interp;
    Tcl_Obj     *valPtr, 
	*elapsedVarPtr = NULL, *resultVarPtr = NULL, 
	*statusVarPtr = NULL, *fileVarPtr = NULL;
    Ns_Time     *timeoutPtr = NULL;
    char        *id = NULL;
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

    assert(itPtr != NULL);
    interp = itPtr->interp;

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (HttpGet(itPtr, id, &httpPtr, 1) == NS_FALSE) {
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
    }
    httpPtr->spoolLimit = spoolLimit;
    httpPtr->replyHeaders = hdrPtr;

    Ns_HttpCheckSpool(httpPtr);

    if (Ns_TaskWait(httpPtr->task, timeoutPtr) != NS_OK) {
	HttpCancel(httpPtr);
	Tcl_AppendResult(interp, "timeout waiting for task", NULL);
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
	Tcl_AppendResult(interp, "ns_http failed: ", httpPtr->error, NULL);
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
	ns_close(httpPtr->spoolFd);
	valPtr = Tcl_NewObj();
    } else {
	valPtr = Tcl_NewByteArrayObj((unsigned char*)httpPtr->ds.string + httpPtr->replyHeaderSize, 
				     (int)httpPtr->ds.length - httpPtr->replyHeaderSize);
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


/*
 *----------------------------------------------------------------------
 *
 * HttpGet --
 *
 *	Locate and remove the Http struct for a given id.
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
HttpGet(NsInterp *itPtr, CONST char *id, Ns_HttpTask **httpPtrPtr, int remove)
{
    Tcl_HashEntry *hPtr;

    assert(itPtr != NULL);
    assert(id != NULL);
    assert(httpPtrPtr != NULL);

    hPtr = Tcl_FindHashEntry(&itPtr->https, id);
    if (hPtr == NULL) {
        Tcl_AppendResult(itPtr->interp, "no such request: ", id, NULL);
        return NS_FALSE;
    }
    *httpPtrPtr = Tcl_GetHashValue(hPtr);
    if (remove != 0) {
        Tcl_DeleteHashEntry(hPtr);
    }
    return NS_TRUE;
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
HttpConnect(Tcl_Interp *interp, const char *method, char *url, Ns_Set *hdrPtr,
	    Tcl_Obj *bodyPtr, Ns_HttpTask **httpPtrPtr)
{
    NS_SOCKET    sock;
    Ns_HttpTask *httpPtr;
    int          len = 0, portNr, uaFlag = -1;
    char        *body, *host, *file, *port, *url2;
    char         hostBuffer[256];

    assert(interp != NULL);
    assert(method != NULL);
    assert(url != NULL);
    assert(httpPtrPtr != NULL);

    if (strncmp(url, "http://", 7U) != 0 || url[7] == '\0') {
	Tcl_AppendResult(interp, "invalid url: ", url, NULL);
        return TCL_ERROR;
    }
    host = url + 7;
    file = strchr(host, '/');
    if (file != NULL) {
        *file = '\0';
    }
    port = strchr(host, ':');
    if (port == NULL) {
        portNr = 80;
    } else {
        *port = '\0';
        portNr = (int) strtol(port+1, NULL, 10);
    }

    strncpy(hostBuffer, host, sizeof(hostBuffer));
    sock = Ns_SockAsyncConnect(hostBuffer, portNr);

    if (sock == NS_INVALID_SOCKET) {
	Tcl_AppendResult(interp, "connect to \"", url, "\" failed: ",
	 		 ns_sockstrerror(ns_sockerrno), NULL);
	return TCL_ERROR;
    }
    url2 = ns_strdup(url);

    /*
     *  Restore the url string
     */

    if (file != NULL) {
	*file = '/';
    }
    
    httpPtr = ns_calloc(1U, sizeof(Ns_HttpTask));
    httpPtr->sock            = sock;
    httpPtr->spoolLimit      = -1;
    httpPtr->url             = url2;
    Ns_MutexInit(&httpPtr->lock);
    /*Ns_MutexSetName(&httpPtr->lock, name, buffer);*/
    Tcl_DStringInit(&httpPtr->ds);

    Ns_DStringAppend(&httpPtr->ds, method);
    Ns_StrToUpper(Ns_DStringValue(&httpPtr->ds));

    Ns_DStringVarAppend(&httpPtr->ds, " ", 
			(file != NULL) ? file : "/",
			" HTTP/1.0\r\n", NULL);

    /*
     * Submit provided headers
     */
    if (hdrPtr != NULL) {
	size_t i;

	/*
	 * Remove the header fields, we are providing
	 */
	Ns_SetIDeleteKey(hdrPtr, "Host");
	Ns_SetIDeleteKey(hdrPtr, "Connection");
	Ns_SetIDeleteKey(hdrPtr, "Content-Length");

	for (i = 0U; i < Ns_SetSize(hdrPtr); i++) {
	    char *key = Ns_SetKey(hdrPtr, i);
	    if (uaFlag != 0) {
		uaFlag = strcasecmp(key, "User-Agent");
	    }
	    Ns_DStringPrintf(&httpPtr->ds, "%s: %s\r\n", key, Ns_SetValue(hdrPtr, i));
	}
    }

    /*
     * No keep-alive even in case of HTTP 1.1
     */
    Ns_DStringAppend(&httpPtr->ds, "Connection: close\r\n");
   
    /*
     * User-Agent header was not supplied, add our own header
     */
    if (uaFlag != 0) {
	Ns_DStringPrintf(&httpPtr->ds, "User-Agent: %s/%s\r\n",
			 Ns_InfoServerName(),
			 Ns_InfoServerVersion());
    }
    
    if (port == NULL) {
	Ns_DStringPrintf(&httpPtr->ds, "Host: %s\r\n", hostBuffer);
    } else {
	Ns_DStringPrintf(&httpPtr->ds, "Host: %s:%d\r\n", hostBuffer, portNr);
    }
    
    body = NULL;
    if (bodyPtr != NULL) {
	body = Tcl_GetStringFromObj(bodyPtr, &len);
	if (len == 0) {
	    body = NULL;
	}
    }
    if (body != NULL) {
	Ns_DStringPrintf(&httpPtr->ds, "Content-Length: %d\r\n", len);
    }
    Tcl_DStringAppend(&httpPtr->ds, "\r\n", 2);
    if (body != NULL) {
	Tcl_DStringAppend(&httpPtr->ds, body, len);
    }
    httpPtr->next = httpPtr->ds.string;
    httpPtr->len = (size_t)httpPtr->ds.length;

    /* Ns_Log(Notice, "final request <%s>", httpPtr->ds.string);*/

    *httpPtrPtr = httpPtr;
    return TCL_OK;
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
HttpAppendRawBuffer(Ns_HttpTask *httpPtr, CONST char *buffer, size_t outSize) 
{
    int status = TCL_OK;

    assert(httpPtr != NULL);
    assert(buffer != NULL);

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
Ns_HttpAppendBuffer(Ns_HttpTask *httpPtr, CONST char *buffer, size_t inSize) 
{
    int status = TCL_OK;

    assert(httpPtr != NULL);
    assert(buffer != NULL);

    Ns_Log(Debug, "Ns_HttpAppendBuffer: got %" PRIdz " bytes flags %.6x", inSize, httpPtr->flags);
    
    if (likely((httpPtr->flags & NS_HTTP_FLAG_GUNZIP) != NS_HTTP_FLAG_GUNZIP)) {
	/*
	 * Output raw content
	 */
	HttpAppendRawBuffer(httpPtr, buffer, inSize);

    } else {
	char out[16384];

	/*
	 * Output decompressed content
	 */
	Ns_InflateBufferInit(httpPtr->compress, buffer, inSize);
	Ns_Log(Debug, "InflateBuffer: got %" PRIdz " compressed bytes", inSize);
	do {
	    int uncompressedLen = 0;
		
	    status = Ns_InflateBuffer(httpPtr->compress, out, sizeof(out), &uncompressedLen);
	    Ns_Log(Debug, "InflateBuffer status %d uncompressed %d bytes", status, uncompressedLen);
	    
	    HttpAppendRawBuffer(httpPtr, out, uncompressedLen);

	} while(status == TCL_CONTINUE);
    }
    return status;
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
    assert(httpPtr != NULL);

    if (httpPtr->task != NULL)          {Ns_TaskFree(httpPtr->task);}
    if (httpPtr->sock > 0)              {ns_sockclose(httpPtr->sock);}
    if (httpPtr->spoolFileName != NULL) {ns_free(httpPtr->spoolFileName);}
    if (httpPtr->spoolFd > 0)           {ns_close(httpPtr->spoolFd);}
    if (httpPtr->compress != NULL)      {
	Ns_InflateEnd(httpPtr->compress);
	ns_free(httpPtr->compress);
    }
    Ns_MutexDestroy(&httpPtr->lock);
    Tcl_DStringFree(&httpPtr->ds);
    ns_free(httpPtr->url);
    ns_free(httpPtr);
}


static void
HttpCancel(const Ns_HttpTask *httpPtr)
{
    assert(httpPtr != NULL);

    Ns_TaskCancel(httpPtr->task);
    Ns_TaskWait(httpPtr->task, NULL);
}


static void
HttpAbort(Ns_HttpTask *httpPtr)
{
    assert(httpPtr != NULL);

    HttpCancel(httpPtr);
    HttpClose(httpPtr);
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
HttpProc(Ns_Task *task, NS_SOCKET sock, void *arg, Ns_SockState why)
{
    Ns_HttpTask *httpPtr = arg;
    char buf[16384];
    ssize_t n;

    assert(task != NULL);
    assert(httpPtr != NULL);

    switch (why) {
    case NS_SOCK_INIT:
	Ns_TaskCallback(task, NS_SOCK_WRITE, &httpPtr->timeout);
	return;

    case NS_SOCK_WRITE:
        n = send(sock, httpPtr->next, (int)httpPtr->len, 0);
    	if (n < 0) {
	    httpPtr->error = "send failed";
	} else {
    	    httpPtr->next += n;
    	    httpPtr->len -= (size_t)n;
    	    if (httpPtr->len == 0u) {
            	Tcl_DStringTrunc(&httpPtr->ds, 0);
	    	Ns_TaskCallback(task, NS_SOCK_READ, &httpPtr->timeout);
	    }
	    return;
	}
	break;

    case NS_SOCK_READ:
    	n = ns_recv(sock, buf, sizeof(buf), 0);
    	if (likely(n > 0)) {

	    /* 
	     * Spooling is only activated after (a) having processed
	     * the headers, and (b) after the wait command has
	     * required to spool. Once we know spoolFd, there is no
	     * need to HttpCheckHeader() again.
	     */
	    if (httpPtr->spoolFd > 0) {
		Ns_HttpAppendBuffer(httpPtr, buf, (size_t)n);
	    } else {
		Ns_Log(Debug, "Task got %d bytes", (int)n);
		Ns_HttpAppendBuffer(httpPtr, buf, (size_t)n);

		if (unlikely(httpPtr->replyHeaderSize == 0)) {
		    Ns_HttpCheckHeader(httpPtr);
		}
		/*
		 * Ns_HttpCheckSpool might set httpPtr->spoolFd
		 */
		Ns_HttpCheckSpool(httpPtr);
		/*Ns_Log(Notice, "Task got %d bytes, header = %d", (int)n, httpPtr->replyHeaderSize);*/
	    }
	    return;
	}
	if (n < 0) {
	    httpPtr->error = "recv failed";
	}
	break;

    case NS_SOCK_DONE:
        return;

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

    /*
     * Get completion time and mark task as done.
     */

    Ns_GetTime(&httpPtr->etime);
    Ns_TaskDone(httpPtr->task);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
