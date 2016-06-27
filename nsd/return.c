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
                                 int fd, const void *data, size_t len)
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
    {426, "Ugrade Required"},
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
  (void) Ns_SetPut(conn->outputheaders, field, value);
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
    (void) Ns_SetPut(conn->outputheaders, field, ds.string);
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
        (void) Ns_SetPut(conn->outputheaders, field, value);
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
        Ns_DStringVarAppend(&ds, mimeType, "; charset=", charset, NULL);
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
Ns_ConnSetLengthHeader(Ns_Conn *conn, size_t length, int doStream)
{
    Conn *connPtr = (Conn *) conn;

    if (doStream == 0) {
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
Ns_ConnConstructHeaders(Ns_Conn *conn, Ns_DString *dsPtr)
{
    Conn       *connPtr = (Conn *) conn;
    Ns_Sock    *sockPtr;
    size_t      i;
    const char *reason, *value;

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
     * However, MIME_Version is a MIME header, not a HTTP header (allthough
     * allowed in HTTP/1.1); it's only used when HTTP messages are moved over
     * MIME-based protocols (e.g., SMTP), which is uncommon. The HTTP mime
     * message parsing semantics are defined by this RFC 2616 and not any MIME
     * specification.  
     *
     * For full backwards compatibility, a MIME-Version header could be added
     * for a site via nssocket/nsssl driver parameter "extraheaders".
     */

    Ns_DStringVarAppend(dsPtr,
			"Server: ", Ns_InfoServerName(), "/", Ns_InfoServerVersion(), "\r\n",
			"Date: ",
			NULL);
    (void)Ns_HttpTime(dsPtr, NULL);
    Ns_DStringNAppend(dsPtr, "\r\n", 2);

    /*
     * Add extra headers from config file, if available.
     */
    sockPtr = Ns_ConnSockPtr(conn);
    if (sockPtr != NULL) {
        value = sockPtr->driver->extraHeaders;
        if (value != NULL) {
            Ns_DStringNAppend(dsPtr, value, -1);
        }
    }

    /*
     * Output any extra headers.
     */

    if (conn->outputheaders != NULL) {
        for (i = 0u; i < Ns_SetSize(conn->outputheaders); i++) {
	    const char *key;

            key = Ns_SetKey(conn->outputheaders, i);
            value = Ns_SetValue(conn->outputheaders, i);
            if (key != NULL && value != NULL) {
		char *lineBreak = strchr(value, (int)UCHAR('\n'));

		if (lineBreak == NULL) {
		    Ns_DStringVarAppend(dsPtr, key, ": ", value, "\r\n", NULL);
		} else {
		    Ns_DString sanitize, *sanitizePtr = &sanitize;
		    /*
		     * We have to sanititize the header field to avoid
		     * a HTTP response splitting attack. After each
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
			lineBreak = strchr(value, (int)UCHAR('\n'));

		    } while (lineBreak != NULL);

		    Tcl_DStringAppend(sanitizePtr, value, -1);

		    Ns_DStringVarAppend(dsPtr, key, ": ", Tcl_DStringValue(sanitizePtr), "\r\n", NULL);
		    Ns_DStringFree(sanitizePtr);
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
    Conn *connPtr = (Conn *) conn;
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
    Ns_ConnSetLengthHeader(conn, length, 0);
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
    Conn         *connPtr = (Conn *) conn;
    NsServer     *servPtr;
    Ns_DString    ds;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(title != NULL);
    NS_NONNULL_ASSERT(notice != NULL);

    servPtr = connPtr->poolPtr->servPtr;
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
    Ns_DStringVarAppend(&ds, "</h2>\n", notice, "\n", NULL);

    /*
     * Detailed server information at the bottom of the page.
     */

    if (servPtr->opts.noticedetail != 0) {
        Ns_DStringVarAppend(&ds, "<p align='right'><small><i>",
                            Ns_InfoServerName(), "/",
                            Ns_InfoServerVersion(), " on ",
                            NULL);
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

    Ns_DStringVarAppend(&ds, "\n</body></html>\n", NULL);

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
 *      length of the entitiy.
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
    
    return ReturnOpen(conn, status, mimeType, chan, NULL, -1, len);
}

Ns_ReturnCode
Ns_ConnReturnOpenFile(Ns_Conn *conn, int status, const char *mimeType,
                      FILE *fp, size_t len)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);
    
    return ReturnOpen(conn, status, mimeType, NULL, fp, -1, len);
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
	&& (NsWriterQueue(conn, len, chan, fp, fd, NULL, 0, 0) == NS_OK)) {
	return NS_OK;
    }

    if (chan != NULL) {
        Ns_ConnSetLengthHeader(conn, len, 0);
        result = Ns_ConnSendChannel(conn, chan, len);
    } else if (fp != NULL) {
        Ns_ConnSetLengthHeader(conn, len, 0);
        result = Ns_ConnSendFp(conn, fp, len);
    } else {
        result = ReturnRange(conn, mimeType, fd, NULL, len);
    }

    (void) Ns_ConnClose(conn);

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
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
ReturnRange(Ns_Conn *conn, const char *mimeType,
            int fd, const void *data, size_t len)
{
    Ns_DString    ds;
    Ns_FileVec    bufs[32];
    int           nbufs = (int)(sizeof(bufs) / sizeof(bufs[0]));
    int           rangeCount;
    Ns_ReturnCode result = NS_ERROR;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(mimeType != NULL);

    Ns_DStringInit(&ds);
    rangeCount = NsConnParseRange(conn, mimeType, fd, data, len,
                                  bufs, &nbufs, &ds);

    /*
     * Don't use writer when only headers are returned
     */
    if ((conn->flags & NS_CONN_SKIPBODY) == 0u) {

	/*
	 * We are able to handle the following cases via writer:
	 * - iovec based requests: all range request up to 32 ranges.
	 * - fd based requests: 0 or 1 range requests
	 */
	if (fd == NS_INVALID_FD) {
	    int nvbufs;
	    struct iovec vbuf[32];

	    if (rangeCount == 0) {
		nvbufs = 1;
		vbuf[0].iov_base = (void *)data;
		vbuf[0].iov_len  = len;
	    } else {
		int i;

		nvbufs = rangeCount;
		len = 0u;
		for (i = 0; i < rangeCount; i++) {
		    vbuf[i].iov_base = INT2PTR(bufs[i].offset);
		    vbuf[i].iov_len  = bufs[i].length;
		    len += bufs[i].length;
		}
	    }

	    if (NsWriterQueue(conn, len, NULL, NULL, fd, &vbuf[0], nvbufs, 0) == NS_OK) {
		Ns_DStringFree(&ds);
		return NS_OK;
	    }
	} else if (rangeCount < 2) {
	    if (rangeCount == 1) {
		if (ns_lseek(fd, bufs[0].offset, SEEK_SET) == -1) {
                    Ns_Log(Warning, "seek operation with offset %" PROTd " failed: %s",
                           bufs[0].offset, strerror(errno));
                }
		len = bufs[0].length;
	    }
	    if (NsWriterQueue(conn, len, NULL, NULL, fd, NULL, 0, 0) == NS_OK) {
		Ns_DStringFree(&ds);
		return NS_OK;
	    }
	}
    }
    
    if (rangeCount >= 0) {
	if (rangeCount == 0) {
            Ns_ConnSetLengthHeader(conn, len, 0);

	    if ((conn->flags & NS_CONN_SKIPBODY) != 0u) {
	      len = 0u;
	    }

            (void) Ns_SetFileVec(bufs, 0, fd, data, 0, len);
            nbufs = 1;
        }

	result = Ns_ConnWriteVData(conn, NULL, 0, NS_CONN_STREAM);
        if (result == NS_OK) {
            result = Ns_ConnSendFileVec(conn, bufs, nbufs);
        }
    }
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
