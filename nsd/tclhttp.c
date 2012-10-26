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

extern Tcl_ObjCmdProc NsTclHttpObjCmd;

typedef struct {
    Ns_Task *task;
    NS_SOCKET sock;
    char *url;
    char *error;
    char *next;
    size_t len;
    int status;
    Ns_Time timeout;
    Ns_Time stime;
    Ns_Time etime;
    Tcl_DString ds;
} Http;

/*
 * Local functions defined in this file
 */

static int HttpWaitCmd(NsInterp *itPtr, int objc, Tcl_Obj * CONST objv[]);
static int HttpQueueCmd(NsInterp *itPtr, int objc, Tcl_Obj * CONST objv[], int run);
static int SetWaitVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr);
static int HttpConnect(Tcl_Interp *interp, char *method, char *url,
			Ns_Set *hdrs, Tcl_Obj *bodyPtr, Http **httpPtrPtr);
static Tcl_Obj *HttpResult(Tcl_DString *ds, int *statusPtr, Ns_Set *hdrs);
static void HttpClose(Http *httpPtr);
static void HttpCancel(Http *httpPtr);
static void HttpAbort(Http *httpPtr);
static int GetHttp(NsInterp *itPtr, char *id, Http **httpPtrPtr, int remove);
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
NsTclHttpObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsInterp *itPtr = arg;
    Http *httpPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    int opt, run = 0;
    static CONST char *opts[] = {
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
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            (int *) &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case HRunIdx:
	run = 1;
	/* FALLTHROUGH */
    case HQueueIdx:
	return HttpQueueCmd(itPtr, objc, objv, run);
	break;

    case HWaitIdx:
	return HttpWaitCmd(itPtr, objc, objv);
	break;

    case HCancelIdx:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, "id");
            return TCL_ERROR;
        }
	if (!GetHttp(itPtr, Tcl_GetString(objv[2]), &httpPtr, 1)) {
	    return TCL_ERROR;
	}
        HttpAbort(httpPtr);
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
        break;
    }
    return TCL_OK;
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
HttpQueueCmd(NsInterp *itPtr, int objc, Tcl_Obj * CONST objv[], int run)
{
    Tcl_Interp *interp = itPtr->interp;
    int isNew, i;
    Tcl_HashEntry *hPtr;
    Http *httpPtr;
    char buf[TCL_INTEGER_SPACE + 4], *url = NULL;
    char *method = "GET";
    Ns_Set *hdrPtr = NULL;
    Tcl_Obj *bodyPtr = NULL;
    Ns_Time *incrPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-timeout",  Ns_ObjvTime,   &incrPtr,  NULL},
        {"-method",   Ns_ObjvString, &method,   NULL},
        {"-body",     Ns_ObjvObj,    &bodyPtr,  NULL},
        {"-headers",  Ns_ObjvSet,    &hdrPtr,     NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",       Ns_ObjvString, &url, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (!HttpConnect(interp, method, url, hdrPtr, bodyPtr, &httpPtr)) {
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
    if (run) {
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
    } while (!isNew);
    Tcl_SetHashValue(hPtr, httpPtr);
    Tcl_SetResult(interp, buf, TCL_VOLATILE);
    return TCL_OK;
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
HttpWaitCmd(NsInterp *itPtr, int objc, Tcl_Obj * CONST objv[])
{
    Tcl_Interp *interp = itPtr->interp;
    Tcl_Obj *valPtr;
    Tcl_Obj *elapsedPtr = NULL;
    Tcl_Obj *resultPtr = NULL;
    Tcl_Obj *statusPtr = NULL;
    Ns_Time *timeoutPtr = NULL;
    char *id = NULL;
    Ns_Set *hdrPtr = NULL;
    Http *httpPtr;
    Ns_Time diff;
    int result = TCL_ERROR;
    int status;

    Ns_ObjvSpec opts[] = {
        {"-timeout",  Ns_ObjvTime, &timeoutPtr,  NULL},
        {"-elapsed",  Ns_ObjvObj,  &elapsedPtr,  NULL},
        {"-result",   Ns_ObjvObj,  &resultPtr,   NULL},
        {"-status",   Ns_ObjvObj,  &statusPtr,   NULL},
        {"-headers",  Ns_ObjvSet,  &hdrPtr,      NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"id",       Ns_ObjvString, &id, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (!GetHttp(itPtr, id, &httpPtr, 1)) {
	return TCL_ERROR;
    }
    if (Ns_TaskWait(httpPtr->task, timeoutPtr) != NS_OK) {
	HttpCancel(httpPtr);
	Tcl_AppendResult(interp, "timeout waiting for task", NULL);
	return TCL_ERROR;
    }
    result = TCL_ERROR;
    if (elapsedPtr != NULL) {
    	Ns_DiffTime(&httpPtr->etime, &httpPtr->stime, &diff);
	valPtr = Tcl_NewObj();
    	Ns_TclSetTimeObj(valPtr, &diff);
    	if (!SetWaitVar(interp, elapsedPtr, valPtr)) {
	    goto err;
	}
    }
    if (httpPtr->error) {
	Tcl_AppendResult(interp, "http failed: ", httpPtr->error, NULL);
	goto err;
    }

    valPtr = HttpResult(&httpPtr->ds, &status, hdrPtr);
    if (statusPtr != NULL &&
		!SetWaitVar(interp, statusPtr, Tcl_NewIntObj(status))) {
	goto err;
    }
    if (resultPtr == NULL) {
	Tcl_SetObjResult(interp, valPtr);
    } else {
	if (!SetWaitVar(interp, resultPtr, valPtr)) {
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
 * GetHttp --
 *
 *	Locate and remove the Http struct for a given id.
 *
 * Results:
 *	1 on success, 0 otherwise.
 *
 * Side effects:
 *	Will update given httpPtrPtr with pointer to Http struct.
 *
 *----------------------------------------------------------------------
 */

static int
GetHttp(NsInterp *itPtr, char *id, Http **httpPtrPtr, int remove)
{
    Tcl_HashEntry *hPtr;

    hPtr = Tcl_FindHashEntry(&itPtr->https, id);
    if (hPtr == NULL) {
        Tcl_AppendResult(itPtr->interp, "no such request: ", id, NULL);
        return 0;
    }
    *httpPtrPtr = Tcl_GetHashValue(hPtr);
    if (remove) {
        Tcl_DeleteHashEntry(hPtr);
    }
    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * SetWaitVar --
 *
 *	Set a variable by name.  Convience routine for for HttpWaitCmd.
 *
 * Results:
 *	1 on success, 0 otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
SetWaitVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr)
{
    Tcl_Obj *errPtr;

    Tcl_IncrRefCount(valPtr);
    errPtr = Tcl_ObjSetVar2(interp, varPtr, NULL, valPtr,
			       TCL_PARSE_PART1|TCL_LEAVE_ERR_MSG);
    Tcl_DecrRefCount(valPtr);
    return (errPtr ? 1 : 0);
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
 *        1 if successful, 0 otherwise.
 *
 * Side effects:
 *        Updates httpPtrPtr with newly allocated Http struct.
 *
 *----------------------------------------------------------------------
 */

int
HttpConnect(Tcl_Interp *interp, char *method, char *url, Ns_Set *hdrs,
	    Tcl_Obj *bodyPtr, Http **httpPtrPtr)
{
    NS_SOCKET sock;
    Http *httpPtr = NULL;
    int i, len;
    char *key, *body, *host, *file, *port;

    if (strncmp(url, "http://", 7) != 0 || url[7] == '\0') {
	Tcl_AppendResult(interp, "invalid url: ", url, NULL);
        return 0;
    }
    host = url + 7;
    file = strchr(host, '/');
    if (file != NULL) {
        *file = '\0';
    }
    port = strchr(host, ':');
    if (port == NULL) {
        i = 80;
    } else {
        *port = '\0';
        i = (int) strtol(port+1, NULL, 10);
    }
    sock = Ns_SockAsyncConnect(host, i);
    if (port != NULL) {
        *port = ':';
    }
    if (sock != INVALID_SOCKET) {
        int uaFlag = -1;

        httpPtr = ns_malloc(sizeof(Http));
        httpPtr->sock = sock;
	httpPtr->error = NULL;
        httpPtr->url = ns_strdup(url);
        Tcl_DStringInit(&httpPtr->ds);
        if (file != NULL) {
            *file = '/';
        }
        Ns_DStringAppend(&httpPtr->ds, method);
        Ns_StrToUpper(Ns_DStringValue(&httpPtr->ds));
        Ns_DStringVarAppend(&httpPtr->ds, " ", file ? file : "/",
			    " HTTP/1.0\r\n", NULL);
        if (file != NULL) {
            *file = '\0';
        }
        if (hdrs != NULL) {
            for (i = 0; i < Ns_SetSize(hdrs); i++) {
                key = Ns_SetKey(hdrs, i);
                if (uaFlag) {
                    uaFlag = strcasecmp(key, "User-Agent");
                }
                Ns_DStringVarAppend(&httpPtr->ds, key, ": ",
                    Ns_SetValue(hdrs, i), "\r\n", NULL);
            }
        }

        /*
         * User-Agent header was not supplied, add our own header
         */

        if (uaFlag) {
            Ns_DStringVarAppend(&httpPtr->ds, "User-Agent: ",
                Ns_InfoServerName(), "/", Ns_InfoServerVersion(), "\r\n", NULL);
        }

        /*
         * No keep-alive even in case of HTTP 1.1
         */

        Ns_DStringVarAppend(&httpPtr->ds,
            "Connection: close\r\n"
            "Host: ", host, "\r\n", NULL);

        if (file != NULL) {
            *file = '/';
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
        httpPtr->len = httpPtr->ds.length;
    }
    if (file != NULL) {
        *file = '/';
    }
    if (httpPtr == NULL) {
	Tcl_AppendResult(interp, "connect to \"", url, "\" failed: ",
	 		 ns_sockstrerror(ns_sockerrno), NULL);
	return 0;
    }
    *httpPtrPtr = httpPtr;
    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * HttpResult --
 *
 *        Parse an Http response for the result body and headers.
 *
 * Results:
 *        Pointer to body within Http buffer.
 *
 * Side effects:
 *        Will append parsed response headers to given hdrs if
 *        not NULL and set HTTP status code in given statusPtr.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
HttpResult(Tcl_DString *ds, int *statusPtr, Ns_Set *hdrs)
{
    char *eoh, *body, *p, *response;
    int major, minor;
    Tcl_Obj *result;

    body = response = ds->string;
    eoh = strstr(response, "\r\n\r\n");
    if (eoh != NULL) {
        body = eoh + 4;
	eoh += 2;
    } else {
        eoh = strstr(response, "\n\n");
        if (eoh != NULL) {
            body = eoh + 2;
	    eoh += 1;
        }
    }

    result = Tcl_NewByteArrayObj((unsigned char*)body, 
				 (int)(ds->length-(body-response)));

    if (eoh == NULL) {
	*statusPtr = 0;
    } else {
	*eoh = '\0';
	sscanf(response, "HTTP/%d.%d %d", &major, &minor, statusPtr);
    	if (hdrs != NULL) {
            int firsthdr = 1;
	    char save = *body;

	    *body = '\0';
            p = response;
            while ((eoh = strchr(p, '\n')) != NULL) {
	        size_t len;

            	*eoh++ = '\0';
            	len = strlen(p);
            	if (len > 0 && p[len-1] == '\r') {
                    p[len-1] = '\0';
            	}
            	if (firsthdr) {
                    if (hdrs->name != NULL) {
                    	ns_free(hdrs->name);
                    }
                    hdrs->name = ns_strdup(p);
                    firsthdr = 0;
            	} else if (Ns_ParseHeader(hdrs, p, ToLower) != NS_OK) {
                    break;
            	}
            	p = eoh;
	    }
	    *body = save;
        }
    }
    return result;
}

static void
HttpClose(Http *httpPtr)
{
    Ns_TaskFree(httpPtr->task);
    Tcl_DStringFree(&httpPtr->ds);
    ns_sockclose(httpPtr->sock);
    ns_free(httpPtr->url);
    ns_free(httpPtr);
}


static void
HttpCancel(Http *httpPtr)
{
    Ns_TaskCancel(httpPtr->task);
    Ns_TaskWait(httpPtr->task, NULL);
}


static void
HttpAbort(Http *httpPtr)
{
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
HttpProc(Ns_Task *task, NS_SOCKET sock, void *arg, int why)
{
    Http *httpPtr = arg;
    char buf[4096];
    int n;

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
    	    httpPtr->len -= n;
    	    if (httpPtr->len == 0) {
            	Tcl_DStringTrunc(&httpPtr->ds, 0);
	    	Ns_TaskCallback(task, NS_SOCK_READ, &httpPtr->timeout);
	    }
	    return;
	}
	break;

    case NS_SOCK_READ:
    	n = recv(sock, buf, sizeof(buf), 0);
    	if (n > 0) {
            Tcl_DStringAppend(&httpPtr->ds, buf, n);
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

    }

    /*
     * Get completion time and mark task as done.
     */

    Ns_GetTime(&httpPtr->etime);
    Ns_TaskDone(httpPtr->task);
}
