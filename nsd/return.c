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

/*
 * Local functions defined in this file
 */

static Ns_ReturnCode ReturnOpen(Ns_Conn *conn, int status, const char *mimeType, Tcl_Channel chan,
                                FILE *fp, int fd, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static Ns_ReturnCode ReturnRange(Ns_Conn *conn, const char *mimeType,
                                 int fd, const void *data, size_t dataLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * This structure connections HTTP response codes to their descriptions.
 */

static const struct {
    int         status;
    const char *reason;
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
    {208, "Already Reported"},
    {226, "IM Used"},
    {300, "Multiple Choices"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {303, "See Other"},
    {304, "Not Modified"},
    {305, "Use Proxy"},
    {306, "SwitchProxy"},
    {307, "Temporary Redirect"},
    {308, "Permanent Redirect"},
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
    {413, "Payload Too Large"},
    {414, "URI Too Long"},
    {415, "Unsupported Media Type"},
    {416, "Range Not Satisfiable"},
    {417, "Expectation Failed"},
    {418, "I'm a teapot"},
    {419, "Authentication Timeout"},
    {421, "Misdirected Request"},
    {422, "Unprocessable Entity"},
    {423, "Locked"},
    {424, "Failed Dependency"},
    {425, "Insufficient Space On Resource"},
    {426, "Upgrade Required"},
    {426, "Precondition Required"},
    {429, "Too Many Requests"},
    {431, "Request Header Fields Too Large"},
    {451, "Unavailable For Legal Reasons"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
    {506, "Variant Also Negotiates"},
    {507, "Insufficient Storage"},
    {508, "Loop Detected"},
    {510, "Not Extended"},
    {511, "Network Authentication Required"}
};

/*
 * Static variables defined in this file.
 */

static const size_t nreasons = (sizeof(reasons) / sizeof(reasons[0]));



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
Ns_ConnSetHeaders(const Ns_Conn *conn, const char *field, const char *value)
{
    (void) Ns_SetPutSz(conn->outputheaders, field, -1, value, -1);
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
Ns_ConnUpdateHeaders(const Ns_Conn *conn, const char *field, const char *value)
{
    Ns_SetIUpdate(conn->outputheaders, field, value);
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
Ns_ConnPrintfHeaders(const Ns_Conn *conn, const char *field, const char *fmt,...)
{
    Ns_DString ds;
    va_list ap;

    Ns_DStringInit(&ds);
    va_start(ap, fmt);
    Ns_DStringVPrintf(&ds, fmt, ap);
    va_end(ap);
    (void) Ns_SetPutSz(conn->outputheaders, field, -1, ds.string, ds.length);
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
Ns_ConnCondSetHeaders(const Ns_Conn *conn, const char *field, const char *value)
{
    if (Ns_SetIGet(conn->outputheaders, field) == NULL) {
        (void) Ns_SetPutSz(conn->outputheaders, field, -1, value, -1);
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
Ns_ConnReplaceHeaders(Ns_Conn *conn, const Ns_Set *newheaders)
{
    Ns_SetFree(conn->outputheaders);
    conn->outputheaders = Ns_SetCopy(newheaders);
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
Ns_ConnSetTypeHeader(const Ns_Conn *conn, const char *mimeType)
{
    Ns_ConnUpdateHeaders(conn, "Content-Type", mimeType);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetEncodedTypeHeader --
 *
 *      Sets the Content-Type HTTP output header and charset for
 *      text and other types which may need to be transcoded.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      My change the output encoding if charset specified or add a
 *      charset to the mime-type otherwise.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnSetEncodedTypeHeader(Ns_Conn *conn, const char *mimeType)
{
    Tcl_Encoding  encoding;
    const char   *charset;
    Ns_DString    ds;
    size_t        len;

    Ns_DStringInit(&ds);
    charset = NsFindCharset(mimeType, &len);

    if (charset != NULL) {
        encoding = Ns_GetCharsetEncodingEx(charset, (int)len);
        Ns_ConnSetEncoding(conn, encoding);
    } else {
        encoding = Ns_ConnGetEncoding(conn);
        charset = Ns_GetEncodingCharset(encoding);
        Ns_DStringVarAppend(&ds, mimeType, "; charset=", charset, (char *)0L);
        mimeType = ds.string;
    }

    Ns_ConnSetTypeHeader(conn, mimeType);
    conn->flags |= NS_CONN_WRITE_ENCODED;

    Ns_DStringFree(&ds);
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
Ns_ConnSetLengthHeader(Ns_Conn *conn, size_t length, bool doStream)
{
    Conn *connPtr = (Conn *) conn;

    if (!doStream) {
        char buffer[TCL_INTEGER_SPACE];

        snprintf(buffer, sizeof(buffer), "%" PRIuz, length);
        Ns_ConnUpdateHeaders(conn, "Content-Length", buffer);
        connPtr->responseLength = (ssize_t)length;
    } else {
        /*
         * In the streaming case, make sure no Content-Length is set.
         */
        Ns_SetIDeleteKey(conn->outputheaders, "Content-Length");
        connPtr->responseLength = -1;
    }
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
Ns_ConnSetLastModifiedHeader(const Ns_Conn *conn, const time_t *mtime)
{
    Ns_DString ds;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mtime != NULL);

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
Ns_ConnSetExpiresHeader(const Ns_Conn *conn, const char *expires)
{
    Ns_ConnSetHeaders(conn, "Expires", expires);
}


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
Ns_ConnConstructHeaders(const Ns_Conn *conn, Ns_DString *dsPtr)
{
    const Conn    *connPtr = (const Conn *) conn;
    size_t         i;
    const char    *reason;

    /*
     * Construct the HTTP response status line.
     */

    reason = "Unknown Reason";
    for (i = 0u; i < nreasons; i++) {
        if (reasons[i].status == connPtr->responseStatus) {
            reason = reasons[i].reason;
            break;
        }
    }

    Ns_DStringPrintf(dsPtr, "HTTP/%.1f %d %s\r\n",
                     MIN(connPtr->request.version, 1.1),
                     connPtr->responseStatus,
                     reason);

    /*
     * Add the basic required headers if they.
     *
     * Earlier versions included
     *
     *       "MIME-Version: 1.0\r\n"
     *
     * However, MIME_Version is a MIME header, not an HTTP header (although
     * allowed in HTTP/1.1); it is only used when HTTP messages are moved over
     * MIME-based protocols (e.g., SMTP), which is uncommon. The HTTP mime
     * message parsing semantics are defined by this RFC 2616 and not any MIME
     * specification.
     *
     * For full backwards compatibility, a MIME-Version header could be added
     * via configuration parameter "extraheaders" (from network driver or
     * server config).
     */

    Ns_DStringVarAppend(dsPtr,
                        "Server: ", Ns_InfoServerName(), "/", Ns_InfoServerVersion(), "\r\n",
                        "Date: ", (char *)0L);
    (void)Ns_HttpTime(dsPtr, NULL);
    Ns_DStringNAppend(dsPtr, "\r\n", 2);

    /*
     * Header processing. Merge possibly the output headers as provided by the
     * application with the extra headers (per-server and per-drivet) from the
     * configuration file.
     */
    {
        const Ns_Sock *sockPtr;
        const Ns_Set  *outputHeaders;
        Ns_Set        *headers = conn->outputheaders;

        /*
         * We have always output headers, this is assured by ConnRun().
         */
        assert(conn->outputheaders != NULL);

        sockPtr = Ns_ConnSockPtr(conn);
        if (sockPtr != NULL) {
            NsServer *servPtr = ((Sock *)sockPtr)->servPtr;

            if (servPtr->opts.extraHeaders != NULL) {
                /*
                 * We have server-specific extra headers. Merge these into the
                 * output headers. Output headers have the higher priority: if
                 * there is already shuch a header field, it is kept.
                 */
                Ns_SetIMerge(headers, servPtr->opts.extraHeaders);
            }

            if (sockPtr->driver->extraHeaders != NULL) {
                /*
                 * We have driver-specific output headers. Fields already in
                 * the output headers have the higher priority.
                 */
                Ns_SetIMerge(headers, sockPtr->driver->extraHeaders);
            }
        }
        outputHeaders = headers;

        /*
         * Add the (potentially merged) header set in a sanitized form into
         * the resulting DString (dsPtr).
         */
        if (outputHeaders != NULL) {
            for (i = 0u; i < Ns_SetSize(outputHeaders); i++) {
                const char *key, *value;

                key = Ns_SetKey(outputHeaders, i);
                value = Ns_SetValue(outputHeaders, i);
                if (key != NULL && value != NULL) {
                    const char *lineBreak = strchr(value, INTCHAR('\n'));

                    if (lineBreak == NULL) {
                        Ns_DStringVarAppend(dsPtr, key, ": ", value, "\r\n", (char *)0L);
                    } else {
                        Ns_DString sanitize, *sanitizePtr = &sanitize;
                        /*
                         * We have to sanititize the header field to avoid
                         * an HTTP response splitting attack. After each
                         * newline in the value, we insert a TAB character
                         * (see Section 4.2 in RFC 2616)
                         */

                        Ns_DStringInit(&sanitize);

                        do {
                            size_t offset = (size_t)(lineBreak - value);

                            if (offset > 0u) {
                                Tcl_DStringAppend(sanitizePtr, value, (int)offset);
                            }
                            Tcl_DStringAppend(sanitizePtr, "\n\t", 2);

                            offset ++;
                            value += offset;
                            lineBreak = strchr(value, INTCHAR('\n'));

                        } while (lineBreak != NULL);

                        Tcl_DStringAppend(sanitizePtr, value, -1);

                        Ns_DStringVarAppend(dsPtr, key, ": ", Tcl_DStringValue(sanitizePtr), "\r\n", (char *)0L);
                        Ns_DStringFree(sanitizePtr);
                    }
                }
            }
        }
    }

    /*
     * End of headers.
     */
    Ns_Log(Ns_LogRequestDebug, "headers:\n%s", dsPtr->string);

    Ns_DStringNAppend(dsPtr, "\r\n", 2);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnQueueHeaders, Ns_ConnFlushHeaders, Ns_ConnSetRequiredHeaders --
 *
 *      Deprecated.
 *
 * Results:
 *      None / Number of bytes written.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnQueueHeaders(Ns_Conn *conn, int status)
{
    /*
     * Deprecated
     */
    Ns_ConnSetResponseStatus(conn, status);
}

size_t
Ns_ConnFlushHeaders(Ns_Conn *conn, int status)
{
    const Conn *connPtr = (const Conn *) conn;
    /*
     * Deprecated
     */
    Ns_ConnSetResponseStatus(conn, status);
    (void) Ns_ConnWriteVData(conn, NULL, 0, 0u);

    return connPtr->nContentSent;
}

void
Ns_ConnSetRequiredHeaders(Ns_Conn *conn, const char *mimeType, size_t length)
{
    /*
     * Deprecated
     */
    Ns_ConnSetTypeHeader(conn, mimeType);
    Ns_ConnSetLengthHeader(conn, length, NS_FALSE);
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

Ns_ReturnCode
Ns_ConnResetReturn(Ns_Conn *UNUSED(conn))
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

Ns_ReturnCode
Ns_ConnReturnAdminNotice(Ns_Conn *conn, int status,
                         const char *title, const char *notice)
{
    return Ns_ConnReturnNotice(conn, status, title, notice);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotice --
 *
 *      Return a short notice to a client. The content of argument "title" is
 *      plain text and HTML-quoted by this function, the content of argument
 *      "notice" might be rich text that is assumed to be already properly
 *      quoted.
 *
 * Results:
 *      See Ns_ReturnHtml.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnNotice(Ns_Conn *conn, int status,
                    const char *title, const char *notice)
{
    const NsServer  *servPtr;
    Ns_DString       ds;
    Ns_ReturnCode    result;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(title != NULL);
    NS_NONNULL_ASSERT(notice != NULL);

    servPtr = ((Conn *) conn)->poolPtr->servPtr;
    Ns_DStringInit(&ds);
    Ns_DStringAppend(&ds,
                     "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 4.01//EN\">\n"
                     "<html>\n<head>\n"
                     "<title>");
    Ns_QuoteHtml(&ds, title);
    Ns_DStringAppend(&ds,
                     "</title>\n"
                     "</head>\n<body>\n"
                     "<h2>");
    Ns_QuoteHtml(&ds, title);
    Ns_DStringVarAppend(&ds, "</h2>\n", notice, "\n", (char *)0L);

    /*
     * Detailed server information at the bottom of the page.
     */

    if (servPtr->opts.noticedetail) {
        Ns_DStringVarAppend(&ds, "<p align='right'><small><i>",
                            Ns_InfoServerName(), "/",
                            Ns_InfoServerVersion(), " on ",
                            (char *)0L);
        (void) Ns_ConnLocationAppend(conn, &ds);
        Ns_DStringAppend(&ds, "</i></small></p>\n");
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

    Ns_DStringVarAppend(&ds, "\n</body></html>\n", (char *)0L);

    result = Ns_ConnReturnCharData(conn, status, ds.string, ds.length, "text/html");
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

Ns_ReturnCode
Ns_ConnReturnData(Ns_Conn *conn, int status, const char *data,
                  ssize_t len, const char *mimeType)
{
    Ns_ReturnCode result;
    size_t        length;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(data != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);

    Ns_ConnSetTypeHeader(conn, mimeType);
    length = (len < 0) ? strlen(data) : (size_t)len;
    Ns_ConnSetResponseStatus(conn, status);

    result = ReturnRange(conn, mimeType, -1, data, length);
    (void) Ns_ConnClose(conn);

    return result;
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

Ns_ReturnCode
Ns_ConnReturnCharData(Ns_Conn *conn, int status, const char *data,
                      ssize_t len, const char *mimeType)
{
    struct iovec  sbuf;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(data != NULL);

    if (mimeType != NULL) {
        Ns_ConnSetEncodedTypeHeader(conn, mimeType);
    }

    sbuf.iov_base = (void *)data;
    sbuf.iov_len = len < 0 ? strlen(data) : (size_t)len;

    Ns_ConnSetResponseStatus(conn, status);
    result = Ns_ConnWriteVChars(conn, &sbuf, 1, 0u);
    (void) Ns_ConnClose(conn);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnHtml --
 *
 *      Return utf-8 character data as mime-type text/html to client.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      See Ns_ConnReturnCharData
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnHtml(Ns_Conn *conn, int status, const char *html, ssize_t len)
{
    return Ns_ConnReturnCharData(conn, status, html, len, "text/html");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnOpenChannel, FILE, fd --
 *
 *      Return an open channel, FILE, or fd out the conn.
 *
 * Results:
 *      NS_OK / NS_ERROR.
 *
 * Side effects:
 *      Will set a length header, so 'len' must describe the complete
 *      length of the entity.
 *
 *      May send various HTTP error responses.
 *
 *      May return before the content has been sent if the writer-queue
 *      is enabled.
 *
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnOpenChannel(Ns_Conn *conn, int status, const char *mimeType,
                         Tcl_Channel chan, size_t len)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);

    return ReturnOpen(conn, status, mimeType, chan, NULL, NS_INVALID_FD, len);
}

Ns_ReturnCode
Ns_ConnReturnOpenFile(Ns_Conn *conn, int status, const char *mimeType,
                      FILE *fp, size_t len)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);

    return ReturnOpen(conn, status, mimeType, NULL, fp, NS_INVALID_FD, len);
}

Ns_ReturnCode
Ns_ConnReturnOpenFd(Ns_Conn *conn, int status, const char *mimeType,
                    int fd, size_t len)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);

    return ReturnOpen(conn, status, mimeType, NULL, NULL, fd, len);
}

static Ns_ReturnCode
ReturnOpen(Ns_Conn *conn, int status, const char *mimeType, Tcl_Channel chan,
           FILE *fp, int fd, size_t len)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);

    Ns_ConnSetTypeHeader(conn, mimeType);
    Ns_ConnSetResponseStatus(conn, status);

    if ((chan != NULL || fp != NULL)
        && (NsWriterQueue(conn, len, chan, fp, fd, NULL, 0,  NULL, 0, NS_FALSE) == NS_OK)) {
        result = NS_OK;
    } else {

        if (chan != NULL) {
            Ns_ConnSetLengthHeader(conn, len, NS_FALSE);
            result = Ns_ConnSendChannel(conn, chan, (ssize_t)len);
        } else if (fp != NULL) {
            Ns_ConnSetLengthHeader(conn, len, NS_FALSE);
            result = Ns_ConnSendFp(conn, fp, (ssize_t)len);
        } else {
            result = ReturnRange(conn, mimeType, fd, NULL, len);
        }

        (void) Ns_ConnClose(conn);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnRange --
 *
 *      Return ranges from an open fd or buffer if specified by
 *      client, otherwise return the entire range.
 *
 * Results:
 *      NS_OK if all data sent, NS_ERROR otherwise
 *
 * Side effects:
 *      May send various HTTP error responses.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
ReturnRange(Ns_Conn *conn, const char *mimeType,
            int fd, const void *data, size_t dataLength)
{
    Ns_DString    ds;
    Ns_FileVec    bufs[NS_MAX_RANGES * 2 + 1];
    int           nbufs = NS_MAX_RANGES * 2, rangeCount;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);

    Ns_DStringInit(&ds);

    /*
     * NsConnParseRange() returns in the provided bufs the content plus the
     * separating (chunked) multipart headers and the multipart trailer. For
     * this, it needs (NS_MAX_RANGES * 2 + 1) bufs.
     */
    rangeCount = NsConnParseRange(conn, mimeType, fd, data, dataLength,
                                  bufs, &nbufs, &ds);

    if (rangeCount == -1) {
        Ns_DStringFree(&ds);
        return NS_ERROR;
    }

    /*
     * Don't use writer thread when only headers are returned.
     */

    if ((conn->flags & NS_CONN_SKIPBODY) == 0u) {

        /*
         * Return range based content.
         *
         * We are able to handle the following cases via writer:
         *
         * - iovec based requests: up to NS_MAX_RANGES ranges
         * - fd based requests: 0 (= whole file) or 1 range(s)
         *
         * All other cases: default to the Ns_ConnSendFileVec().
         */

        if (fd == NS_INVALID_FD && rangeCount < NS_MAX_RANGES) {
            struct iovec vbuf[NS_MAX_RANGES *2 + 1];

            if (rangeCount == 0) {
                nbufs = 1;
                vbuf[0].iov_base = (void *)data;
                vbuf[0].iov_len  = dataLength;
            } else {
                int i;

                dataLength = 0u;
                for (i = 0; i < nbufs; i++) {
                    vbuf[i].iov_base = (char *)bufs[i].buffer + bufs[i].offset;
                    vbuf[i].iov_len  = bufs[i].length;
                    dataLength += bufs[i].length;
                }
            }
            if (NsWriterQueue(conn, dataLength, NULL, NULL, NS_INVALID_FD,
                              vbuf, nbufs,  NULL, 0, NS_FALSE) == NS_OK) {
                Ns_DStringFree(&ds);
                return NS_OK;

            }
        } else if (fd != NS_INVALID_FD && rangeCount < 2) {
            if (rangeCount == 1) {
                if (ns_lseek(fd, bufs[0].offset, SEEK_SET) == -1) {

                    /*
                     * TODO:
                     * What is with error string on Windows?
                     */

                    Ns_Log(Warning, "seek operation with offset %" PROTd
                           " failed: %s", bufs[0].offset, strerror(errno));
                    Ns_DStringFree(&ds);
                    return NS_ERROR;
                }
                dataLength = bufs[0].length;
            }
            if (NsWriterQueue(conn, dataLength, NULL, NULL, fd, NULL, 0, NULL, 0,
                              NS_FALSE) == NS_OK) {
                Ns_DStringFree(&ds);
                return NS_OK;
            }
        }
    }

    if (rangeCount == 0) {
        Ns_ConnSetLengthHeader(conn, dataLength, NS_FALSE);
        if ((conn->flags & NS_CONN_SKIPBODY) != 0u) {
            dataLength = 0u;
        }
        (void) Ns_SetFileVec(bufs, 0, fd, data, 0, dataLength);
        nbufs = 1;
    }

    /*
     * Flush Headers and send file contents.
     */

    result = Ns_ConnWriteVData(conn, NULL, 0, NS_CONN_STREAM);

    if (result == NS_OK) {
        result = Ns_ConnSendFileVec(conn, bufs, nbufs);
    }

    NsPoolAddBytesSent(((Conn *)conn)->poolPtr,  (Tcl_WideInt)Ns_ConnContentSent(conn));

    Ns_DStringFree(&ds);

    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
