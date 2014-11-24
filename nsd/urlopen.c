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
 * urlopen.c --
 *
 *  Make outgoing HTTP requests.
 */

#include "nsd.h"

#define BUFSIZE 2048

typedef struct Stream {
    NS_SOCKET sock;
    int       error;
    size_t    cnt;
    char     *ptr;
    char      buf[BUFSIZE+1];
} Stream;

/*
 * Local functions defined in this file
 */

static bool GetLine(Stream *sPtr, Ns_DString *dsPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static bool FillBuf(Stream *sPtr)
     NS_GNUC_NONNULL(1);



/*
 *----------------------------------------------------------------------
 *
 * Ns_FetchPage --
 *
 *      Fetch a page off of this very server. Url must reference a 
 *      file in the filesystem. 
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      The file contents will be put into the passed-in dstring.
 *
 *----------------------------------------------------------------------
 */

int
Ns_FetchPage(Ns_DString *dsPtr, const char *url, const char *server)
{
    Ns_DString  ds;
    Tcl_Channel chan;
    int result = NS_OK;

    assert(dsPtr != NULL);
    assert(url != NULL);
    assert(server != NULL);

    Ns_DStringInit(&ds);
    (void) Ns_UrlToFile(&ds, server, url);
    chan = Tcl_OpenFileChannel(NULL, ds.string, "r", 0);
    Ns_DStringFree(&ds);
    if (chan != NULL) {
        char buf[1024];
        int  nread;

        while ((nread = Tcl_Read(chan, buf, (int)sizeof(buf))) > 0) {
            Ns_DStringNAppend(dsPtr, buf, nread);
        }
        result = Tcl_Close(NULL, chan);
    } else {
        result = NS_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_FetchURL --
 *
 *      Open up an HTTP connection to an arbitrary URL. 
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Page contents will be appended to the passed-in dstring.
 *      Headers returned to us will be put into the passed-in Ns_Set.
 *      The set name will be changed to a copy of the HTTP status line.
 *
 *----------------------------------------------------------------------
 */

int
Ns_FetchURL(Ns_DString *dsPtr, const char *url, Ns_Set *headers)
{
    NS_SOCKET       sock;
    const char     *p;
    Ns_DString      ds;
    Stream          s;
    Ns_Request      request;
    int             status;
    size_t          toSend;

    assert(dsPtr != NULL);
    assert(url != NULL);

    sock = NS_INVALID_SOCKET;
    Ns_DStringInit(&ds);

    /*
     * Parse the URL and open a connection.
     */

    Ns_DStringVarAppend(&ds, "GET ", url, " HTTP/1.0", NULL);
    status = Ns_ParseRequest(&request, ds.string);
    if (status == NS_ERROR ||
        request.protocol == NULL ||
        !STREQ(request.protocol, "http") ||
        request.host == NULL) {
        Ns_Log(Notice, "urlopen: invalid url '%s'", url);
        goto done;
    }
    if (request.port == 0U) {
        request.port = 80U;
    }
    sock = Ns_SockConnect(request.host, (int)request.port);
    if (sock == NS_INVALID_SOCKET) {
        Ns_Log(Error, "urlopen: failed to connect to '%s': '%s'",
               url, ns_sockstrerror(ns_sockerrno));
        goto done;
    }

    /*
     * Send a simple HTTP GET request.
     */
     
    Ns_DStringSetLength(&ds, 0);
    Ns_DStringVarAppend(&ds, "GET ", request.url, NULL);
    if (request.query != NULL) {
        Ns_DStringVarAppend(&ds, "?", request.query, NULL);
    }
    Ns_DStringAppend(&ds, " HTTP/1.0\r\nAccept: */*\r\n\r\n");
    p = ds.string;
    toSend = (size_t)ds.length;
    while (toSend > 0u) {
        ssize_t sent = ns_send(sock, p, toSend, 0);
        if (sent < 0) {
            Ns_Log(Error, "urlopen: failed to send data to '%s': '%s'",
                   url, ns_sockstrerror(ns_sockerrno));
            goto done;
        }
        toSend -= (size_t)sent;
        p += sent;
    }

    /*
     * Buffer the socket and read the response line and then
     * consume the headers, parsing them into any given header set.
     */

    s.cnt = 0u;
    s.error = 0;
    s.ptr = s.buf;
    s.sock = sock;
    if (GetLine(&s, &ds) == NS_FALSE) {
        goto done;
    }
    if (headers != NULL && strncmp(ds.string, "HTTP", 4u) == 0) {
        if (headers->name != NULL) {
	    ns_free((char *)headers->name);
        }
        headers->name = Ns_DStringExport(&ds);
    }
    do {
        if (GetLine(&s, &ds) == NS_FALSE) {
            goto done;
        }
        if (ds.length > 0
            && headers != NULL
            && Ns_ParseHeader(headers, ds.string, Preserve) != NS_OK) {
            goto done;
        }
    } while (ds.length > 0);
    
    /*
     * Without any check on limit or total size, foolishly read
     * the remaining content into the dstring.
     */
    
    do {
      Ns_DStringNAppend(dsPtr, s.ptr, (int)s.cnt);
    } while (FillBuf(&s));

    if (s.error == 0) {
        status = NS_OK;
    }

 done:
    Ns_ResetRequest(&request);

    if (sock != NS_INVALID_SOCKET) {
        ns_sockclose(sock);
    }
    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetUrlObjCmd --
 *
 *      Implements ns_geturl. 
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
NsTclGetUrlObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp   *itPtr = arg;
    Ns_DString  ds;
    Ns_Set     *headers;
    int         status, code;
    const char *url;

    if ((objc != 3) && (objc != 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "url ?headersSetIdVar?");
        return TCL_ERROR;
    }

    code = TCL_ERROR;
    if (objc == 2) {
        headers = NULL;
    } else {
        headers = Ns_SetCreate(NULL);
    }
    Ns_DStringInit(&ds);
    url = Tcl_GetString(objv[1]);
    if (url[1] == '/') {
        status = Ns_FetchPage(&ds, url, itPtr->servPtr->server);
    } else {
        status = Ns_FetchURL(&ds, url, headers);
    }
    if (status != NS_OK) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "could not fetch: ",
                               Tcl_GetString(objv[1]), NULL);
        if (headers != NULL) {
            Ns_SetFree(headers);
        }
        goto done;
    }
    if (objc == 3) {
        Ns_TclEnterSet(interp, headers, NS_TCL_SET_DYNAMIC);
        if (Tcl_ObjSetVar2(interp, objv[2], NULL, Tcl_GetObjResult(interp),
                           TCL_LEAVE_ERR_MSG) == NULL) {
            goto done;
        }
    }
    Tcl_DStringResult(interp, &ds);
    code = TCL_OK;
done:
    Ns_DStringFree(&ds);

    return code;
}


/*
 *----------------------------------------------------------------------
 *
 * FillBuf --
 *
 *      Fill the socket stream buffer.
 *
 * Results:
 *      1 if fill ok, 0 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
FillBuf(Stream *sPtr)
{
    ssize_t n;
    
    assert(sPtr != NULL);

    n = ns_recv(sPtr->sock, sPtr->buf, BUFSIZE, 0);
    if (n <= 0) {
        if (n < 0) {
            Ns_Log(Error, "urlopen: "
                   "failed to fill socket stream buffer: '%s'", 
                   strerror(errno));
            sPtr->error = 1;
        }
        return NS_FALSE;
    }
    assert(n > 0);

    sPtr->buf[n] = '\0';
    sPtr->ptr = sPtr->buf;
    sPtr->cnt = (size_t)n;

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * GetLine --
 *
 *      Copy the next line from the stream to a dstring, trimming
 *      the \n and \r.
 *
 * Results:
 *      1 or 0.
 *
 * Side effects:
 *      The dstring is truncated on entry.
 *
 *----------------------------------------------------------------------
 */

static bool
GetLine(Stream *sPtr, Ns_DString *dsPtr)
{
    char   *eol;
    size_t  n;

    assert(sPtr != NULL);
    assert(dsPtr != NULL);

    Ns_DStringSetLength(dsPtr, 0);
    do {
        if (sPtr->cnt > 0u) {
            eol = strchr(sPtr->ptr, '\n');
            if (eol == NULL) {
                n = sPtr->cnt;
            } else {
                *eol++ = '\0';
                n = eol - sPtr->ptr;
            }
            Ns_DStringNAppend(dsPtr, sPtr->ptr, (int)n - 1);
            sPtr->ptr += n;
            sPtr->cnt -= n;
            if (eol != NULL) {
                n = (size_t)dsPtr->length;
                if (n > 0u && dsPtr->string[n-1] == '\r') {
		  Ns_DStringSetLength(dsPtr, (int)n - 1);
                }
                return NS_TRUE;
            }
        }
    } while (FillBuf(sPtr) == NS_TRUE);

    return NS_FALSE;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
