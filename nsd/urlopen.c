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
 * urlopen.c --
 *
 *  Make outgoing HTTP requests.
 */


#include "nsd.h"

#ifdef NS_WITH_DEPRECATED

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

static bool GetLine(Stream *sPtr, Tcl_DString *dsPtr)
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
Ns_FetchPage(Tcl_DString *dsPtr, const char *url, const char *server)
{
    Tcl_DString   ds;
    Tcl_Channel   chan;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(server != NULL);

    Tcl_DStringInit(&ds);
    (void) Ns_UrlToFile(&ds, server, url);
    chan = Tcl_OpenFileChannel(NULL, ds.string, "r", 0);
    Tcl_DStringFree(&ds);
    if (chan != NULL) {
        char buf[1024];
        TCL_SIZE_T nread;

        while ((nread = Tcl_Read(chan, buf, (int)sizeof(buf))) > 0) {
            Tcl_DStringAppend(dsPtr, buf, nread);
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
Ns_FetchURL(Tcl_DString *dsPtr, const char *url, Ns_Set *headers)
{
    NS_SOCKET       sock;
    const char     *p;
    Tcl_DString     ds;
    Stream          s;
    Ns_Request      request;
    Ns_ReturnCode   status;
    size_t          toSend;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    sock = NS_INVALID_SOCKET;
    Tcl_DStringInit(&ds);

    /*
     * Parse the URL and open a connection.
     */

    Ns_DStringVarAppend(&ds, "GET ", url, " HTTP/1.0", NS_SENTINEL);
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

    Tcl_DStringSetLength(&ds, 0);
    Ns_DStringVarAppend(&ds, "GET ", request.url, NS_SENTINEL);
    if (request.query != NULL) {
        Ns_DStringVarAppend(&ds, "?", request.query, NS_SENTINEL);
    }
    Tcl_DStringAppend(&ds, " HTTP/1.0\r\nAccept: */*\r\n\r\n", 26);
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
        ns_free((char *)headers->name);
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
            && Ns_ParseHeader(headers, ds.string, NULL, Preserve, NULL) != NS_OK) {
            goto done;
        }
    } while (ds.length > 0);

    /*
     * Without any check on limit or total size, foolishly read
     * the remaining content into the dstring.
     */

    do {
        Tcl_DStringAppend(dsPtr, s.ptr, (TCL_SIZE_T)s.cnt);
    } while (FillBuf(&s));

    if (s.error == 0) {
        status = NS_OK;
    }

 done:
    Ns_ResetRequest(&request);

    if (sock != NS_INVALID_SOCKET) {
        ns_sockclose(sock);
    }
    Tcl_DStringFree(&ds);

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
NsTclGetUrlObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int             code;

    if ((objc != 3) && (objc != 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "/url/ ?/headersSetIdVar/?");
        code = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        Ns_Set         *headers;
        Ns_ReturnCode   status;
        const char     *url;
        Tcl_DString     ds;

        Ns_LogDeprecated(objv, 2, "ns_http run ...", NULL);

        code = TCL_ERROR;
        if (objc == 2) {
            headers = NULL;
        } else {
            headers = Ns_SetCreate(NULL);
        }
        Tcl_DStringInit(&ds);
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

        Tcl_DStringFree(&ds);
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
GetLine(Stream *sPtr, Tcl_DString *dsPtr)
{
    char   *eol;
    size_t  n;
    bool    success = NS_FALSE;

    NS_NONNULL_ASSERT(sPtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    Tcl_DStringSetLength(dsPtr, 0);
    do {
        if (sPtr->cnt > 0u) {
            eol = strchr(sPtr->ptr, INTCHAR('\n'));
            if (eol == NULL) {
                n = sPtr->cnt;
            } else {
                *eol++ = '\0';
                n = (size_t)(eol - sPtr->ptr);
            }
            Tcl_DStringAppend(dsPtr, sPtr->ptr, (TCL_SIZE_T)n - 1);
            sPtr->ptr += n;
            sPtr->cnt -= n;
            if (eol != NULL) {
                n = (size_t)dsPtr->length;
                if (n > 0u && dsPtr->string[n - 1u] == '\r') {
                    Tcl_DStringSetLength(dsPtr, (TCL_SIZE_T)n - 1);
                }
                success = NS_TRUE;
                break;
            }
        }
    } while (FillBuf(sPtr));

    return success;
}
#else
/*
 * Avoid empty translation unit
 */
   typedef void empty;
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
