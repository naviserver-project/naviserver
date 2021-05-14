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

#define BUFSIZE 2048u

typedef struct Stream {
    NS_SOCKET sock;
    int       error;
    size_t    cnt;
    char     *ptr;
    char      buf[BUFSIZE + 1u];
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
 *      This function is deprecated, one should use the nmuch more general
 *      "ns_http" machinery instead.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      The file contents will be put into the passed-in dstring.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_FetchPage(Ns_DString *dsPtr, const char *url, const char *server)
{
    Ns_DString    ds;
    Tcl_Channel   chan;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(server != NULL);

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
        result = (Tcl_Close(NULL, chan) == TCL_OK ? NS_OK : NS_ERROR);
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
 *      This function is deprecated, one should use the much more general
 *      "ns_http" machinery instead.
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

Ns_ReturnCode
Ns_FetchURL(Ns_DString *dsPtr, const char *url, Ns_Set *headers)
{
    NS_SOCKET       sock;
    const char     *p;
    Ns_DString      ds;
    Stream          s;
    Ns_Request      request;
    Ns_ReturnCode   status;
    size_t          toSend;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    sock = NS_INVALID_SOCKET;
    Ns_DStringInit(&ds);

    /*
     * Parse the URL and open a connection.
     */

    Ns_DStringVarAppend(&ds, "GET ", url, " HTTP/1.0", (char *)0L);
    status = Ns_ParseRequest(&request, ds.string, (size_t)ds.length);
    if (status == NS_ERROR
        || request.protocol == NULL
        || !STREQ(request.protocol, "http")
        || request.host == NULL) {
        Ns_Log(Notice, "urlopen: invalid url '%s'", url);
        goto done;
    }
    if (request.port == 0u) {
        request.port = 80u;
    }
    sock = Ns_SockConnect(request.host, request.port);
    if (sock == NS_INVALID_SOCKET) {
        Ns_Log(Error, "urlopen: failed to connect to '%s': '%s'",
               url, ns_sockstrerror(ns_sockerrno));
        goto done;
    }

    /*
     * Send a simple HTTP GET request.
     */

    Ns_DStringSetLength(&ds, 0);
    Ns_DStringVarAppend(&ds, "GET ", request.url, (char *)0L);
    if (request.query != NULL) {
        Ns_DStringVarAppend(&ds, "?", request.query, (char *)0L);
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

    /*
     * Read response line.
     */
    if (GetLine(&s, &ds) == NS_FALSE) {
        goto done;
    }
    if (headers != NULL && strncmp(ds.string, "HTTP", 4u) == 0) {
        if (headers->name != NULL) {
            ns_free((char *)headers->name);
        }
        headers->name = Ns_DStringExport(&ds);
    }

    /*
     * Parse header lines
     */
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
 *      Implements "ns_geturl".
 *      This function is deprecated, use ns_http instead.
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
NsTclGetUrlObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int             code;

    if ((objc != 3) && (objc != 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "url ?headersSetIdVar?");
        code = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        Ns_Set         *headers;
        Ns_ReturnCode   status;
        const char     *url;
        Ns_DString      ds;

        Ns_LogDeprecated(objv, 2, "ns_http run ...", NULL);

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
            Ns_TclPrintfResult(interp, "could not fetch: %s", Tcl_GetString(objv[1]));
            if (headers != NULL) {
                Ns_SetFree(headers);
            }

        } else if (objc == 3) {
            code = Ns_TclEnterSet(interp, headers, NS_TCL_SET_DYNAMIC);
            if (code == TCL_OK
                && Tcl_ObjSetVar2(interp, objv[2], NULL, Tcl_GetObjResult(interp),
                                  TCL_LEAVE_ERR_MSG) == NULL) {
                code = TCL_ERROR;
            }
        }
        if (code == TCL_OK) {
            Tcl_DStringResult(interp, &ds);
        }

        Ns_DStringFree(&ds);
    }

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
 *      NS_TRUE if fill ok, NS_FALSE otherwise.
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
    bool    result = NS_TRUE;

    NS_NONNULL_ASSERT(sPtr != NULL);

    n = ns_recv(sPtr->sock, sPtr->buf, BUFSIZE, 0);
    if (n <= 0) {
        if (n < 0) {
            Ns_Log(Error, "urlopen: failed to fill socket stream buffer: '%s'",
                   strerror(errno));
            sPtr->error = 1;
        }
        result = NS_FALSE;
    } else {
        assert(n > 0);

        /*
         * The recv() operation was sucessuful, fill values into result fields and
         * return NS_TRUE.
         */

        sPtr->buf[n] = '\0';
        sPtr->ptr = sPtr->buf;
        sPtr->cnt = (size_t)n;
    }

    return result;
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
 *      boolean success
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
    bool    success = NS_FALSE;

    NS_NONNULL_ASSERT(sPtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_DStringSetLength(dsPtr, 0);
    do {
        if (sPtr->cnt > 0u) {
            eol = strchr(sPtr->ptr, INTCHAR('\n'));
            if (eol == NULL) {
                n = sPtr->cnt;
            } else {
                *eol++ = '\0';
                n = (size_t)(eol - sPtr->ptr);
            }
            Ns_DStringNAppend(dsPtr, sPtr->ptr, (int)n - 1);
            sPtr->ptr += n;
            sPtr->cnt -= n;
            if (eol != NULL) {
                n = (size_t)dsPtr->length;
                if (n > 0u && dsPtr->string[n - 1u] == '\r') {
                    Ns_DStringSetLength(dsPtr, (int)n - 1);
                }
                success = NS_TRUE;
                break;
            }
        }
    } while (FillBuf(sPtr));

    return success;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
