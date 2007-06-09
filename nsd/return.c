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
 * return.c --
 *
 *      Functions that construct a response and return it to the client.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


#define HTTP11_HDR_TE     "Transfer-Encoding"
#define HTTP11_TE_CHUNKED "chunked"


/*
 * Local functions defined in this file
 */

static int ReturnOpen(Ns_Conn *conn, int status, CONST char *type, Tcl_Channel chan,
                      FILE *fp, int fd, Tcl_WideInt len);
static int ReturnCharData(Ns_Conn *conn, int status, CONST char *data, int len,
                          CONST char *type, int sendRaw);

/*
 * This structure connections HTTP response codes to their descriptions.
 */

static struct {
    int         status;
    CONST char *reason;
} reasons[] = {
    {100, "Continue"},
    {101, "Switching Protocols"},
    {102, "Processing"},
    {200, "OK"},
    {201, "Created"},
    {202, "Accepted"},
    {203, "Non-Authoritative Information"},
    {204, "No Content"},
    {205, "Reset Content"},
    {206, "Partial Content"},
    {207, "Multi-Status"},
    {300, "Multiple Choices"},
    {301, "Moved"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {305, "Use Proxy"},
    {307, "Temporary Redirect"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {402, "Payment Required"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {406, "Not Acceptable"},
    {407, "Proxy Authentication Required"},
    {408, "Request Timeout"},
    {409, "Conflict"},
    {410, "Gone"},
    {411, "Length Required"},
    {412, "Precondition Failed"},
    {413, "Request Entity Too Large"},
    {414, "Request-URI Too Long"},
    {415, "Unsupported Media Type"},
    {416, "Requested Range Not Satisfiable"},
    {417, "Expectation Failed"},
    {422, "Unprocessable Entity"},
    {423, "Locked"},
    {424, "Method Failure"},
    {425, "Insufficient Space On Resource"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
    {507, "Insufficient Storage"}
};

/*
 * Static variables defined in this file.
 */

static int nreasons = (sizeof(reasons) / sizeof(reasons[0]));


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnConstructHeaders --
 *
 *      Put the header of an HTTP response into the dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Content length and connection-keepalive headers will be added
 *      if possible.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnConstructHeaders(Ns_Conn *conn, Ns_DString *dsPtr)
{
    Conn       *connPtr = (Conn *) conn;
    Driver     *drvPtr  = (Driver *) connPtr->drvPtr;
    int         i, length;
    CONST char *reason, *value, *keep, *key, *lengthHdr;

    /*
     * Construct the HTTP response status line.
     */

    reason = "Unknown Reason";
    for (i = 0; i < nreasons; i++) {
        if (reasons[i].status == connPtr->responseStatus) {
            reason = reasons[i].reason;
            break;
        }
    }

    /*
     * Perform some checks to ensure proper use of chunked
     * encoding
     */

    if ((connPtr->responseStatus == 204
         || connPtr->responseStatus == 206
         || connPtr->responseStatus == 304)) {
        conn->flags &= ~NS_CONN_WRITE_CHUNKED;
        Ns_SetIDeleteKey(conn->outputheaders, HTTP11_HDR_TE);
    }

    /*
     * This connection has been marked to return in chunked encoding
     */

    if (conn->flags & NS_CONN_WRITE_CHUNKED) {
        Ns_ConnCondSetHeaders(conn, HTTP11_HDR_TE, HTTP11_TE_CHUNKED);
        Ns_SetIDeleteKey(conn->outputheaders, "Content-length");
        connPtr->responseLength = 0;
    }

    Ns_DStringPrintf(dsPtr, "%s %d %s\r\n",
                     (connPtr->responseVersion != NULL) ? connPtr->responseVersion :
                     (conn->flags & NS_CONN_WRITE_CHUNKED) ? "HTTP/1.1" : "HTTP/1.0",
                     connPtr->responseStatus,
                     reason);

    /*
     * Output any headers.
     */

    if (conn->outputheaders != NULL) {

        /*
         * Update the response length value directly from the
         * header to be sent, i.e., don't trust programmers
         * correctly called Ns_ConnSetLengthHeader().
         */

        length = connPtr->responseLength;
        lengthHdr = Ns_SetIGet(conn->outputheaders, "content-length");
        if (lengthHdr != NULL) {
            connPtr->responseLength = atoi(lengthHdr);
        }

        /*
         * Output a connection keep-alive header only on
         * any HTTP status 200 response which included
         * a valid and correctly set content-length header.
         */

        if (connPtr->keep > 0 ||
            (connPtr->keep < 0
             && drvPtr->keepwait > 0
             && connPtr->headers != NULL
             && connPtr->request != NULL
             && (((connPtr->responseStatus >= 200 && connPtr->responseStatus < 300)
                 && ((lengthHdr != NULL && connPtr->responseLength == length)
                     || (conn->flags & NS_CONN_WRITE_CHUNKED)) )
                || (connPtr->responseStatus == 304
                    || connPtr->responseStatus == 201
                    || connPtr->responseStatus == 207) )
             && (drvPtr->keepallmethods == NS_TRUE
                 || STREQ(connPtr->request->method, "GET"))
             && (key = Ns_SetIGet(conn->headers, "connection")) != NULL
                 && STRIEQ(key, "keep-alive"))) {

            connPtr->keep = 1;
            keep = "keep-alive";
        } else {
            keep = "close";
        }

        Ns_ConnCondSetHeaders(conn, "Connection", keep);

        for (i = 0; i < Ns_SetSize(conn->outputheaders); i++) {
            key = Ns_SetKey(conn->outputheaders, i);
            value = Ns_SetValue(conn->outputheaders, i);
            if (key != NULL && value != NULL) {
                Ns_DStringAppend(dsPtr, key);
                Ns_DStringNAppend(dsPtr, ": ", 2);
                Ns_DStringAppend(dsPtr, value);
                Ns_DStringNAppend(dsPtr, "\r\n", 2);
            }
        }
    }
    Ns_DStringNAppend(dsPtr, "\r\n", 2);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnQueueHeaders --
 *
 *      Format basic headers to be sent on the connection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See Ns_ConnConstructHeaders.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnQueueHeaders(Ns_Conn *conn, int status)
{
    Conn *connPtr = (Conn *) conn;

    if (!(conn->flags & NS_CONN_SENTHDRS)) {
        /* 200 is default, don't stomp custom redirects. */
        if (status != 200) {
            connPtr->responseStatus = status;
        }
        if (!(conn->flags & NS_CONN_SKIPHDRS)) {
            Ns_ConnConstructHeaders(conn, &connPtr->queued);
            connPtr->nContentSent -= connPtr->queued.length;
        }
        conn->flags |= NS_CONN_SENTHDRS;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnFlushHeaders --
 *
 *      Send out a well-formed set of HTTP headers with the given
 *      status.
 *
 * Results:
 *      Number of bytes written.
 *
 * Side effects:
 *      See Ns_ConnQueueHeaders.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnFlushHeaders(Ns_Conn *conn, int status)
{
    Ns_ConnQueueHeaders(conn, status);
    return Ns_ConnSend(conn, NULL, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetHeaders --
 *
 *      Add an output header.
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
Ns_ConnSetHeaders(Ns_Conn *conn, CONST char *field, CONST char *value)
{
    Ns_SetPut(conn->outputheaders, field, value);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnUpdateHeaders --
 *
 *      Update an output header.
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
Ns_ConnUpdateHeaders(Ns_Conn *conn, CONST char *field, CONST char *value)
{
    Ns_SetUpdate(conn->outputheaders, field, value);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnPrintfHeaders --
 *
 *      Add a printf-style string as an output header.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnPrintfHeaders(Ns_Conn *conn, CONST char *field, CONST char *fmt,...)
{
    Ns_DString ds;
    va_list ap;

    Ns_DStringInit(&ds);
    va_start(ap, fmt);
    Ns_DStringVPrintf(&ds, fmt, ap);
    va_end(ap);
    Ns_SetPut(conn->outputheaders, field, ds.string);
    Ns_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnCondSetHeaders --
 *
 *      Add an output header, only if it doesn't already exist.
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
Ns_ConnCondSetHeaders(Ns_Conn *conn, CONST char *field, CONST char *value)
{
    if (Ns_SetIGet(conn->outputheaders, field) == NULL) {
        Ns_SetPut(conn->outputheaders, field, value);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReplaceHeaders --
 *
 *      Free the existing outpheaders and set them to a copy of
 *      newheaders.
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
Ns_ConnReplaceHeaders(Ns_Conn *conn, Ns_Set *newheaders)
{
    Ns_SetFree(conn->outputheaders);
    conn->outputheaders = Ns_SetCopy(newheaders);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetRequiredHeaders --
 *
 *      Set a sane set of minimal headers for any response:
 *      MIME-Version, Date, Server, Content-Type, Content-Length
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
Ns_ConnSetRequiredHeaders(Ns_Conn *conn, CONST char *type, Tcl_WideInt length)
{
    Ns_DString ds;

    /*
     * Set the standard mime and date headers.
     */

    Ns_DStringInit(&ds);
    Ns_ConnCondSetHeaders(conn, "MIME-Version", "1.0");
    Ns_ConnCondSetHeaders(conn, "Accept-Ranges", "bytes");
    Ns_ConnCondSetHeaders(conn, "Date", Ns_HttpTime(&ds, NULL));
    Ns_DStringSetLength(&ds, 0);

    Ns_DStringVarAppend(&ds, Ns_InfoServerName(), "/", Ns_InfoServerVersion(), NULL);
    Ns_ConnCondSetHeaders(conn, "Server", ds.string);

    /*
     * Set the type and/or length headers if provided.  Note
     * that a valid length is required for connection keep-alive.
     */

    if (type != NULL) {
        Ns_ConnSetTypeHeader(conn, type);
    }
    if (length >= 0) {
        Ns_ConnSetLengthHeader(conn, length);
    }

    Ns_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetTypeHeader --
 *
 *      Sets the Content-Type HTTP output header
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
Ns_ConnSetTypeHeader(Ns_Conn *conn, CONST char *type)
{
    Ns_ConnUpdateHeaders(conn, "Content-Type", type);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetLengthHeader --
 *
 *      Set the Content-Length output header.
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
Ns_ConnSetLengthHeader(Ns_Conn *conn, Tcl_WideInt length)
{
    Conn *connPtr = (Conn *) conn;
    char strlength[TCL_INTEGER_SPACE];

    snprintf(strlength, sizeof(strlength), "%" TCL_LL_MODIFIER "d", length);
    connPtr->responseLength = length;
    Ns_ConnUpdateHeaders(conn, "Content-Length", strlength);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetLastModifiedHeader --
 *
 *      Set the Last-Modified output header if it isn't already set.
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
Ns_ConnSetLastModifiedHeader(Ns_Conn *conn, time_t *mtime)
{
    Ns_DString ds;

    Ns_DStringInit(&ds);
    Ns_ConnCondSetHeaders(conn, "Last-Modified", Ns_HttpTime(&ds, mtime));
    Ns_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetExpiresHeader --
 *
 *      Set the Expires output header.
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
Ns_ConnSetExpiresHeader(Ns_Conn *conn, CONST char *expires)
{
    Ns_ConnSetHeaders(conn, "Expires", expires);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnResetReturn --
 *
 *      Deprecated
 *
 * Results:
 *      NS_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnResetReturn(Ns_Conn *conn)
{
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnAdminNotice --
 *
 *      Return a short notice to a client to contact system
 *      administrator.
 *
 * Results:
 *      See Ns_ConnReturnNotice
 *
 * Side effects:
 *      See Ns_ConnReturnNotice
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnAdminNotice(Ns_Conn *conn, int status,
                         CONST char *title, CONST char *notice)
{
    return Ns_ConnReturnNotice(conn, status, title, notice);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotice --
 *
 *      Return a short notice to a client.
 *
 * Results:
 *      See Ns_ReturnHtml.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnNotice(Ns_Conn *conn, int status,
                    CONST char *title, CONST char *notice)
{
    Conn       *connPtr = (Conn *) conn;
    NsServer   *servPtr = connPtr->servPtr;
    Ns_DString  ds;
    int         result;

    Ns_DStringInit(&ds);
    if (title == NULL) {
        title = "Server Message";
    }
    Ns_DStringVarAppend(&ds,
            "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
            "<HTML>\n<HEAD>\n"
            "<TITLE>", title, "</TITLE>\n"
            "</HEAD>\n<BODY>\n"
            "<H2>", title, "</H2>\n", NULL);
    if (notice != NULL) {
        Ns_DStringVarAppend(&ds, notice, "\n", NULL);
    }

    /*
     * Detailed server information at the bottom of the page.
     */

    if (servPtr->opts.noticedetail) {
        Ns_DStringVarAppend(&ds, "<P ALIGN=RIGHT><SMALL><I>",
                            Ns_InfoServerName(), "/",
                            Ns_InfoServerVersion(), " on ",
                            NULL);
        Ns_ConnLocationAppend(conn, &ds);
        Ns_DStringAppend(&ds, "</I></SMALL></P>\n");
    }

    /*
     * Padding that suppresses those horrible MSIE friendly errors.
     * NB: Because we pad inside the body we may pad more than needed.
     */

    if (status >= 400) {
        while (ds.length < servPtr->opts.errorminsize) {
            Ns_DStringAppend(&ds, "                    ");
        }
    }

    Ns_DStringVarAppend(&ds, "\n</BODY></HTML>\n", NULL);

    result = Ns_ConnReturnHtml(conn, status, ds.string, ds.length);
    Ns_DStringFree(&ds);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnData --
 *
 *      Sets required headers, dumps them, and then writes your data.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      May set numerous headers, will close connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnData(Ns_Conn *conn, int status, CONST char *data, int len,
                  CONST char *type)
{
    return ReturnCharData(conn, status, data, len, type, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnCharData --
 *
 *      Sets required headers, dumps them, and then writes your
 *      data, translating from utf-8 to the correct character encoding.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      May set numerous headers, will close connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnCharData(Ns_Conn *conn, int status, CONST char *data, int len,
                      CONST char *type)
{
    return ReturnCharData(conn, status, data, len, type, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnCharData --
 *
 *      Sets required headers, dumps them, and then writes your data.
 *      If sendRaw is false, then translate the data from utf-8 to the
 *      correct character encoding if appropriate.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      May set numerous headers, will close connection.
 *
 *----------------------------------------------------------------------
 */

static int
ReturnCharData(Ns_Conn *conn, int status, CONST char *data, int len,
               CONST char *type, int sendRaw)
{
    Conn        *connPtr = (Conn *) conn;
    int          hlen, result;
    Tcl_Encoding enc;
    Tcl_DString  type_ds;
    int          new_type = NS_FALSE;

    if (conn->flags & NS_CONN_SKIPBODY) {
        data = NULL;
        len = 0;
    }
    if (len < 0) {
        len = data ? strlen(data) : 0;
    }

    hlen = len;
    if (len > 0 && !sendRaw) {
        /*
         * Make sure we know what output encoding (if any) to use.
         */
        NsComputeEncodingFromType(type, &enc, &new_type, &type_ds);
        if (new_type) {
            type = Tcl_DStringValue(&type_ds);
        }
        if (enc != NULL) {
            connPtr->encoding = enc;
            if (connPtr->request->version > 1.0) {
                connPtr->flags |= NS_CONN_WRITE_CHUNKED;
            } else {
                hlen = -1;
            }
        } else if (connPtr->encoding == NULL) {
            sendRaw = NS_TRUE;
        }
    }
    Ns_ConnSetRequiredHeaders(conn, type, hlen);
    Ns_ConnQueueHeaders(conn, status);
    if (sendRaw) {
        result = Ns_WriteConn(conn, data, len);
    } else {
        result = Ns_WriteCharConn(conn, data, len);
    }
    if (result == NS_OK) {
        result = Ns_ConnClose(conn);
    }
    if (new_type) {
        Tcl_DStringFree(&type_ds);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnHtml --
 *
 *      Return data of type text/html to client.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      See Ns_ConnReturnData
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnHtml(Ns_Conn *conn, int status, CONST char *html, int len)
{
    return Ns_ConnReturnData(conn, status, html, len, "text/html");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnOpenChannel --
 *
 *      Send an open channel out the conn.
 *
 * Results:
 *      See ReturnOpen.
 *
 * Side effects:
 *      See ReturnOpen.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnOpenChannel(Ns_Conn *conn, int status, CONST char *type,
                         Tcl_Channel chan, Tcl_WideInt len)
{
    return ReturnOpen(conn, status, type, chan, NULL, -1, len);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnOpenFile --
 *
 *      Send an open file out the conn.
 *
 * Results:
 *      See ReturnOpen.
 *
 * Side effects:
 *      See ReturnOpen.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnOpenFile(Ns_Conn *conn, int status, CONST char *type,
                      FILE *fp, Tcl_WideInt len)
{
    return ReturnOpen(conn, status, type, NULL, fp, -1, len);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnOpenFd --
 *
 *      Send an open fd out the conn.
 *
 * Results:
 *      See ReturnOpen.
 *
 * Side effects:
 *      See ReturnOpen.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnOpenFd(Ns_Conn *conn, int status, CONST char *type,
                    int fd, Tcl_WideInt len)
{
    return ReturnOpen(conn, status, type, NULL, NULL, fd, len);
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnOpen --
 *
 *      Dump an open 'something' to the conn.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      Will close the connection on success.
 *
 *----------------------------------------------------------------------
 */

static int
ReturnOpen(Ns_Conn *conn, int status, CONST char *type, Tcl_Channel chan,
           FILE *fp, int fd, Tcl_WideInt len)
{
    int result;

    Ns_ConnSetRequiredHeaders(conn, type, len);
    Ns_ConnQueueHeaders(conn, status);
    if (chan != NULL) {
        result = Ns_ConnSendChannel(conn, chan, len);
    } else if (fp != NULL) {
        result = Ns_ConnSendFp(conn, fp, len);
    } else {
        result = Ns_ConnSendFd(conn, fd, len);
    }
    if (result == NS_OK) {
        result = Ns_ConnClose(conn);
    }
    return result;
}
