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
 * connio.c --
 *
 *      Handle connection I/O.
 */

#include "nsd.h"

/*
 * The following is used to allocate a buffer on the stack for
 * encoding character data and stransfering data from disk to
 * the network, and so defines the chunk size of writes to the
 * network.
 */

#define IOBUFSZ 8192


/*
 * Local functions defined in this file
 */

static Ns_ReturnCode ConnSend(Ns_Conn *conn, size_t nsend, Tcl_Channel chan,
                              FILE *fp, int fd)
    NS_GNUC_NONNULL(1);

static int ConnCopy(Ns_Conn *conn, size_t toCopy, Tcl_Channel chan,
                    FILE *fp, int fd)
    NS_GNUC_NONNULL(1);

static bool CheckKeep(const Conn *connPtr)
    NS_GNUC_NONNULL(1);

static int CheckCompress(Conn *connPtr, const struct iovec *bufs, int nbufs, unsigned int ioflags)
    NS_GNUC_NONNULL(1);

static bool HdrEq(const Ns_Set *set, const char *name, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnWriteChars, Ns_ConnWriteVChars --
 *
 *      This will write a string buffer to the conn.  The distinction
 *      being that the given data is explicitly a UTF8 character string,
 *      and will be put out in an 'encoding-aware' manner.
 *      It promises to write all of it.
 *
 * Results:
 *      NS_OK if all data written, NS_ERROR otherwise.
 *
 * Side effects:
 *      See Ns_ConnWriteVData().
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnWriteChars(Ns_Conn *conn, const char *buf, size_t toWrite, unsigned int flags)
{
    struct iovec sbuf;

    sbuf.iov_base = (void *) buf;
    sbuf.iov_len  = toWrite;
    return Ns_ConnWriteVChars(conn, &sbuf, 1, flags);
}

int
Ns_ConnWriteVChars(Ns_Conn *conn, struct iovec *bufs, int nbufs, unsigned int flags)
{
    Conn              *connPtr   = (Conn *) conn;
    Ns_DString         encDs, gzDs;
    struct iovec       iov;
    Ns_ReturnCode      status;

    Ns_DStringInit(&encDs);
    Ns_DStringInit(&gzDs);

    /*
     * Transcode from utf8 if neccessary.
     */

    if (connPtr->outputEncoding != NULL
        && NsEncodingIsUtf8(connPtr->outputEncoding) == 0
        && nbufs > 0
        && bufs[0].iov_len > 0u) {
        int i;

        for (i = 0; i < nbufs; i++) {
	    const char *utfBytes;
	    size_t      utfLen;

            utfBytes = bufs[i].iov_base;
            utfLen   = bufs[i].iov_len;

            if (utfLen > 0u) {
                (void) Tcl_UtfToExternalDString(connPtr->outputEncoding,
                                                utfBytes, (int)utfLen, &encDs);
            }
        }
        (void)Ns_SetVec(&iov, 0, encDs.string, (size_t)encDs.length);
        bufs = &iov;
        nbufs = 1;
    }

    /*
     * Compress if possible.
     */

    if (connPtr->compress < 0) {
        connPtr->compress = CheckCompress(connPtr, bufs, nbufs, flags);
    }
    if (connPtr->compress > 0
        && (nbufs > 0 || (flags & NS_CONN_STREAM_CLOSE) != 0u)
        ) {
        bool flush = ((flags & NS_CONN_STREAM) != 0u) ? NS_FALSE : NS_TRUE;

        if (Ns_CompressBufsGzip(&connPtr->cStream, bufs, nbufs, &gzDs,
                                connPtr->compress, flush) == NS_OK) {
            /* NB: Compression will always succeed. */
            (void)Ns_SetVec(&iov, 0, gzDs.string, (size_t)gzDs.length);
            bufs = &iov;
            nbufs = 1;
        }
    }

    status = Ns_ConnWriteVData(conn, bufs, nbufs, flags);

    Ns_DStringFree(&encDs);
    Ns_DStringFree(&gzDs);

    return status;
}



/*
 *----------------------------------------------------------------------
 *
 * CheckCompress --
 *
 *      Is compression enabled, and at what level.
 *
 * Results:
 *      compress level 0-9
 *
 * Side effects:
 *      May set the Content-Encoding and Vary headers.
 *
 *----------------------------------------------------------------------
 */

static int
CheckCompress(Conn *connPtr, const struct iovec *bufs, int nbufs, unsigned int ioflags)
{
    Ns_Conn  *conn    = (Ns_Conn *) connPtr;
    NsServer *servPtr;
    int       level, compress = 0;

    NS_NONNULL_ASSERT(connPtr != NULL);

    servPtr = connPtr->poolPtr->servPtr;

    /* 
     * Check the default setting and explicit overide. 
     */
    level = Ns_ConnGetCompression(conn);

    if (level > 0) {
        /*
         * Make sure the length is above the minimum threshold, or
         * we're streaming (assume length is long enough for streams).
         */
        if (((ioflags & NS_CONN_STREAM) != 0u)
            || (bufs != NULL && Ns_SumVec(bufs, nbufs) >= (size_t)servPtr->compress.minsize)
            || connPtr->responseLength >= servPtr->compress.minsize) {
            /* 
	     * We won't be compressing if there are no headers or body. 
	     */
            if (((connPtr->flags & NS_CONN_SENTHDRS) == 0u)
		&& ((connPtr->flags & NS_CONN_SKIPBODY) == 0u)) {
	        Ns_ConnSetHeaders(conn, "Vary", "Accept-Encoding");

                if ((connPtr->flags & NS_CONN_ZIPACCEPTED) != 0u) {
                    Ns_ConnSetHeaders(conn, "Content-Encoding", "gzip");
                    compress = level;
                }
            }
        }
    }
    return compress;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnWriteData, Ns_ConnWriteVData --
 *
 *      Send one or more buffers of raw bytes to the client, possibly
 *      using the HTTP chunked encoding if flags includes NS_CONN_STREAM.
 *
 * Results:
 *      NS_OK if all data written, NS_ERROR otherwise.
 *
 * Side effects:
 *      HTTP headers are constructed and sent on first call.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnWriteData(Ns_Conn *conn, const void *buf, size_t toWrite, unsigned int flags)
{
    struct iovec vbuf;

    vbuf.iov_base = (void *) buf;
    vbuf.iov_len  = toWrite;

    return Ns_ConnWriteVData(conn, &vbuf, 1, flags);
}

Ns_ReturnCode
Ns_ConnWriteVData(Ns_Conn *conn, struct iovec *bufs, int nbufs, unsigned int flags)
{
    Conn         *connPtr = (Conn *) conn;
    Ns_DString    ds;
    int           nsbufs, sbufIdx;
    size_t        bodyLength, toWrite, neededBufs;
    ssize_t       nwrote;
    struct iovec  sbufs[32], *sbufPtr;

    NS_NONNULL_ASSERT(connPtr != NULL);
    assert(nbufs < 1 || bufs != NULL);

    Ns_DStringInit(&ds);

    /*
     * Make sure there's enough send buffers to contain the given
     * buffers, a set of optional HTTP headers, and an optional
     * HTTP chunked header/footer pair. Use the stack if possible.
     */

    neededBufs = (size_t)nbufs + 2u + 1u;
    if (neededBufs > (sizeof(sbufs) / sizeof(struct iovec))) {
        sbufPtr = ns_calloc(neededBufs, sizeof(struct iovec));
    } else {
        sbufPtr = sbufs;
    }
    nsbufs = 0;
    sbufIdx = 0;

    /*
     * Work out the body length for non-chunking case.
     */

    bodyLength = (bufs != NULL) ? Ns_SumVec(bufs, nbufs) : 0u;
    toWrite = 0u;

    if ((flags & NS_CONN_STREAM) != 0u) {
	connPtr->flags |= NS_CONN_STREAM;
    }

    /*
     * Send headers if not already sent.
     */

    if (((conn->flags & NS_CONN_SENTHDRS) == 0u)) {
        conn->flags |= NS_CONN_SENTHDRS;
        if (Ns_CompleteHeaders(conn, bodyLength, flags, &ds) == NS_TRUE) {
            toWrite += Ns_SetVec(sbufPtr, sbufIdx++,
                                 Ns_DStringValue(&ds), 
				 (size_t)Ns_DStringLength(&ds));
            nsbufs++;
        }
    }

    /*
     * Send body.
     */

    if ((conn->flags & NS_CONN_SKIPBODY) == 0u) {

        if ((conn->flags & NS_CONN_CHUNK) == 0u) {
            /*
             * Output content without chunking header/trailers.
             */

            if (sbufIdx == 0) {
                sbufPtr = bufs;
                nsbufs = nbufs;
            } else if (nbufs > 0) {
		NS_NONNULL_ASSERT(bufs != NULL);
                (void) memcpy(sbufPtr + sbufIdx, bufs, (size_t)nbufs * sizeof(struct iovec));
                nsbufs += nbufs;
            }
            toWrite += bodyLength;

        } else {

            if (bodyLength > 0u) {
		char hdr[32];
		size_t len;

		assert(nbufs > 0);
		assert(bufs != NULL);
                /*
                 * Output length header followed by content and then trailer.
                 */

		len = (size_t)sprintf(hdr, "%lx\r\n", (unsigned long)bodyLength);
                toWrite += Ns_SetVec(sbufPtr, sbufIdx++, hdr, len);

                (void) memcpy(sbufPtr + sbufIdx, bufs, (size_t)nbufs * sizeof(struct iovec));
                sbufIdx += nbufs;
                toWrite += bodyLength;

                toWrite += Ns_SetVec(sbufPtr, sbufIdx++, "\r\n", (size_t)2);

                nsbufs += nbufs + 2;
            }

	    if ((flags & NS_CONN_STREAM_CLOSE) != 0u) {
                /*
                 * Output end-of-content trailer for chunked encoding
                 */

                toWrite += Ns_SetVec(sbufPtr, sbufIdx, "0\r\n\r\n", (size_t)5);

                nsbufs += 1;
                connPtr->flags &= ~NS_CONN_STREAM;
                connPtr->flags |= NS_CONN_SENT_LAST_CHUNK;
            }
        }
    }

    /*
     * Write the output buffer.
     */

    nwrote = Ns_ConnSend(conn, sbufPtr, nsbufs);

    Ns_DStringFree(&ds);
    if (sbufPtr != sbufs && sbufPtr != bufs) {
        ns_free(sbufPtr);
    }

    return (nwrote < (ssize_t)toWrite) ? NS_ERROR : NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSendChannel, Fp, Fd --
 *
 *      Send an open channel, FILE or fd.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      See ConnSend().
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnSendChannel(Ns_Conn *conn, Tcl_Channel chan, size_t nsend)
{
    return ConnSend(conn, nsend, chan, NULL, -1);
}

Ns_ReturnCode
Ns_ConnSendFp(Ns_Conn *conn, FILE *fp, size_t nsend)
{
    return ConnSend(conn, nsend, NULL, fp, -1);
}

Ns_ReturnCode
Ns_ConnSendFd(Ns_Conn *conn, int fd, size_t nsend)
{
    return ConnSend(conn, nsend, NULL, NULL, fd);
}


/*
 *----------------------------------------------------------------------
 *
 * ConnSend --
 *
 *      Send an open channel, FILE or fd. Read the content from the
 *      various sources into a buffer and send the data to the client
 *      via Ns_ConnWriteVData(). Stop transmission when errors or
 *      0-byte reads occur.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      Send data to client
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ConnSend(Ns_Conn *conn, size_t nsend, Tcl_Channel chan, FILE *fp, int fd)
{
    Ns_ReturnCode status;
    int           nread;
    char          buf[IOBUFSZ];

    NS_NONNULL_ASSERT(conn != NULL);

    /*
     * Even if nsend is 0 ensure HTTP response headers get written.
     */

    if (nsend == 0u) {
        return Ns_ConnWriteVData(conn, NULL, 0, 0u);
    }

    /*
     * Read from disk and send in IOBUFSZ chunks until done.
     */

    status = NS_OK;
    while (status == NS_OK && nsend > 0u) {
        size_t toRead = nsend;

        if (toRead > sizeof(buf)) {
            toRead = sizeof(buf);
        }
        if (chan != NULL) {
	    nread = Tcl_Read(chan, buf, (int)toRead);
        } else if (fp != NULL) {
	    nread = (int)fread(buf, 1u, toRead, fp);
            if (ferror(fp) != 0) {
                nread = -1;
            }
        } else {
            nread = ns_read(fd, buf, toRead);
        }

        if (nread == -1 || nread == 0 /* NB: truncated file */) {
            status = NS_ERROR;
        } else {
	    struct iovec vbuf;

	    vbuf.iov_base = (void *)buf;
	    vbuf.iov_len  = (size_t)nread;
	    status = Ns_ConnWriteVData(conn, &vbuf, 1, 0u);
	    if (status == NS_OK) {
		nsend -= (size_t)nread;
	    }
	}
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSendFileVec --
 *
 *      Send a vector of file ranges/buffers. It promises to send
 *      all of it.
 *
 * Results:
 *      NS_OK if all data sent, NS_ERROR otherwise.
 *
 * Side effects:
 *      Will update connPtr->nContentSent.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnSendFileVec(Ns_Conn *conn, Ns_FileVec *bufs, int nbufs)
{
    Conn        *connPtr = (Conn *) conn;
    int          i;
    size_t       toWrite, nwrote;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);
    
    nwrote = 0u;
    toWrite = 0u;

    for (i = 0; i < nbufs; i++) {
        toWrite += bufs[i].length;
    }

    while (toWrite > 0u) {
        ssize_t sent = NsDriverSendFile(connPtr->sockPtr, bufs, nbufs, 0u);

        if (sent < 1) {
            break;
        }
        if ((size_t)sent < toWrite) {
            (void)Ns_ResetFileVec(bufs, nbufs, (size_t)sent);
        }
        nwrote += (size_t)sent;
        toWrite -= (size_t)sent;
    }
    if (nwrote > 0u) {
        connPtr->nContentSent += nwrote;
    }

    return nwrote < toWrite ? NS_ERROR : NS_OK;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnPuts --
 *
 *      Write a null-terminated string directly to the conn; no
 *      trailing newline will be appended despite the name.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      See Ns_ConnWriteVData().
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnPuts(Ns_Conn *conn, const char *s)
{
    struct iovec vbuf;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(s != NULL);

    vbuf.iov_base = (void *) s;
    vbuf.iov_len  = strlen(s);
    
    return Ns_ConnWriteVData(conn, &vbuf, 1, NS_CONN_STREAM);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSendDString --
 *
 *      Write contents of a DString directly to the conn.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      See Ns_ConnWriteVData().
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnSendDString(Ns_Conn *conn, const Ns_DString *dsPtr)
{
    struct iovec vbuf;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    vbuf.iov_base = dsPtr->string;
    vbuf.iov_len  = (size_t)dsPtr->length;
    
    return Ns_ConnWriteVData(conn, &vbuf, 1, NS_CONN_STREAM);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSend --
 *
 *      Send buffers to client efficiently.
 *      It promises to send all of it.
 *
 * Results:
 *      Number of bytes of given buffers written, otherwise
 *      -1 on error from first send.
 *
 * Side effects:
 *      - Will update connPtr->nContentSent.
 *      - Also depends on configured comm driver, i.e. nssock, nsssl.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_ConnSend(Ns_Conn *conn, struct iovec *bufs, int nbufs)
{
    Conn    *connPtr = (Conn *) conn;
    int      i;
    size_t   toWrite;
    ssize_t  nwrote, sent;

    if (connPtr->sockPtr == NULL) {
        return -1;
    }

    toWrite = 0u;
    nwrote = 0;

    assert(nbufs <= 0 || bufs != NULL);

    for (i = 0; i < nbufs; i++) {
        toWrite += bufs[i].iov_len;
    }
   
    if (toWrite == 0u) {
	return 0;
    }

    if (NsWriterQueue(conn, toWrite, NULL, NULL, -1, bufs, nbufs, 0) == NS_OK) {
	Ns_Log(Debug, "==== writer sent %" PRIuz " bytes\n", toWrite);
	return (ssize_t)toWrite;
    }
    
    /*
     * Perform the actual send operation.
     */
    {
	Ns_Time timeout;
	
	timeout.sec = connPtr->sockPtr->drvPtr->sendwait;
	timeout.usec = 0;
      
	sent = Ns_SockSendBufs((Ns_Sock*)connPtr->sockPtr, bufs, nbufs, &timeout, 0u);
    }

    /*
     * Update counters;
     */
    nwrote += sent;
    if (nwrote > 0) {
	connPtr->nContentSent += (size_t)nwrote;
    }

    return (nwrote > 0) ? nwrote : sent;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnFlushContent --
 *
 *      Finish reading waiting content.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnFlushContent(Ns_Conn *conn)
{
    Conn    *connPtr = (Conn *) conn;
    Request *reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL) {
        return -1;
    }
    reqPtr->next  += reqPtr->avail;
    reqPtr->avail  = 0u;

    return NS_OK;
}


/*
 *-----------------------------------------------------------------
 *
 * Ns_ConnClose --
 *
 *      Return a connection to the driver thread for close or
 *      keep-alive.
 *
 * Results:
 *      Always NS_OK.
 *
 * Side effects:
 *      May trigger writing http-chunked trailer.
 *      Tcl at-close callbacks may run.
 *
 *-----------------------------------------------------------------
 */

int
Ns_ConnClose(Ns_Conn *conn)
{
    Conn *connPtr;

    NS_NONNULL_ASSERT(conn != NULL);

    connPtr = (Conn *) conn;
    Ns_Log(Debug, "Ns_ConnClose %p stream %.6x chunk %.6x via writer %.6x sockPtr %p", 
	   (void *)connPtr, 
	   connPtr->flags & NS_CONN_STREAM, 
	   connPtr->flags & NS_CONN_CHUNK, 
	   connPtr->flags & NS_CONN_SENT_VIA_WRITER, 
	   (void *)connPtr->sockPtr);

    if (connPtr->sockPtr != NULL) {

        if ((connPtr->flags & NS_CONN_STREAM) != 0u
	    && ((connPtr->flags & NS_CONN_CHUNK) != 0u
		|| (connPtr->compress > 0)
		)) {
            /*
             * Streaming:
             *   In chunked mode, write the end-of-content trailer.
             *   If compressing, write the gzip footer.
             */
            (void) Ns_ConnWriteVChars(conn, NULL, 0, NS_CONN_STREAM_CLOSE);
        }

	/*
	 * Close the connection to the client either here or in the
	 * writer thread.
	 */
	if ((connPtr->flags & NS_CONN_SENT_VIA_WRITER) == 0u) {
	    bool keep = (connPtr->keep > 0) ? NS_TRUE : NS_FALSE;
	    NsSockClose(connPtr->sockPtr, keep);
	}

        
        connPtr->sockPtr = NULL;
        connPtr->flags |= NS_CONN_CLOSED;

        if (connPtr->itPtr != NULL) {
            NsTclRunAtClose(connPtr->itPtr);
        }
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnWrite, Ns_WriteConn, Ns_WriteCharConn --
 *
 *      Deprecated.
 *
 * Results:
 *      #bytes / NS_OK / NS_ERROR
 *
 * Side effects:
 *      See Ns_ConnWrite*
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnWrite(Ns_Conn *conn, const void *buf, size_t toWrite)
{
    Conn  *connPtr = (Conn *) conn;
    size_t n;
    int    status;
    struct iovec vbuf;

    vbuf.iov_base = (void *) buf;
    vbuf.iov_len  = toWrite;

    n = connPtr->nContentSent;
    status = Ns_ConnWriteVData(conn, &vbuf, 1, 0u);
    if (status == NS_OK) {
        return (int)connPtr->nContentSent - (int)n;
    }
    return -1;
}

Ns_ReturnCode
Ns_WriteConn(Ns_Conn *conn, const char *buf, size_t toWrite)
{
    struct iovec vbuf;

    /* Deprecated for Ns_ConnWriteVData */
    
    NS_NONNULL_ASSERT(conn != NULL);
    
    vbuf.iov_base = (void *) buf;
    vbuf.iov_len  = toWrite;
    
    return Ns_ConnWriteVData(conn, &vbuf, 1, NS_CONN_STREAM);
}

int
Ns_WriteCharConn(Ns_Conn *conn, const char *buf, size_t toWrite)
{
    struct iovec sbuf;

    sbuf.iov_base = (void *)buf;
    sbuf.iov_len = toWrite;
    return Ns_ConnWriteVChars(conn, &sbuf, 1, NS_CONN_STREAM);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnGets --
 *
 *      Read in a string from a connection, stopping when either
 *      we've run out of data, hit a newline, or had an error
 *
 * Results:
 *      Pointer to given buffer or NULL on error.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ConnGets(char *buf, size_t bufsize, Ns_Conn *conn)
{
    char *p;

    p = buf;
    while (bufsize > 1u) {
        if (Ns_ConnRead(conn, p, 1u) != 0u) {
            return NULL;
        }
        if (*p++ == '\n') {
            break;
        }
        --bufsize;
    }
    *p = '\0';

    return buf;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnRead --
 *
 *      Copy data from read-ahead buffers.
 *
 * Results:
 *      Number of bytes copied.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_ConnRead(Ns_Conn *conn, void *vbuf, size_t toRead)
{
    Conn    *connPtr = (Conn *) conn;
    Request *reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL) {
        return 0u;
    }
    if (toRead > reqPtr->avail) {
        toRead = reqPtr->avail;
    }
    memcpy(vbuf, reqPtr->next, toRead);
    reqPtr->next  += toRead;
    reqPtr->avail -= toRead;

    return toRead;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReadLine --
 *
 *      Read a line (\r\n or \n terminated) from the conn.
 *
 * Results:
 *      NS_OK if a line was read.  NS_ERROR if no line ending
 *      was found or the line would be too long.
 *
 * Side effects:
 *      Stuff may be read
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReadLine(Ns_Conn *conn, Ns_DString *dsPtr, size_t *nreadPtr)
{
    Conn       *connPtr = (Conn *) conn;
    Request    *reqPtr = connPtr->reqPtr;
    Driver     *drvPtr = connPtr->drvPtr;
    char       *eol;
    size_t     nread, ncopy;

    if (connPtr->sockPtr == NULL
        || (eol = strchr(reqPtr->next, '\n')) == NULL
        || (nread = (eol - reqPtr->next)) > (size_t)drvPtr->maxline) {
        return NS_ERROR;
    }
    ncopy = nread;
    ++nread;
    if (nreadPtr != NULL) {
        *nreadPtr = nread;
    }
    if (ncopy > 0u && *(eol-1) == '\r') {
        --ncopy;
    }
    Ns_DStringNAppend(dsPtr, reqPtr->next, (int)ncopy);
    reqPtr->next  += nread;
    reqPtr->avail -= nread;

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReadHeaders --
 *
 *      Read the headers and insert them into the passed-in set
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Stuff will be read from the conn
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReadHeaders(Ns_Conn *conn, Ns_Set *set, size_t *nreadPtr)
{
    Ns_DString      ds;
    Conn           *connPtr = (Conn *) conn;
    size_t          nread, nline, maxhdr;
    int             status;

    Ns_DStringInit(&ds);
    nread = 0u;
    maxhdr = (size_t)connPtr->drvPtr->maxheaders;
    status = NS_OK;
    while (nread < maxhdr && status == NS_OK) {
        Ns_DStringSetLength(&ds, 0);
        status = Ns_ConnReadLine(conn, &ds, &nline);
        if (status == NS_OK) {
            nread += nline;
            if (nread > maxhdr) {
                status = NS_ERROR;
            } else {
                if (ds.string[0] == '\0') {
                    break;
                }
                status = Ns_ParseHeader(set, ds.string, connPtr->poolPtr->servPtr->opts.hdrcase);
            }
        }
    }
    if (nreadPtr != NULL) {
        *nreadPtr = nread;
    }
    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnCopyToDString --
 *
 *      Copy data from a connection to a dstring.
 *
 * Results:
 *      NS_OK or NS_ERROR
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnCopyToDString(Ns_Conn *conn, size_t toCopy, Ns_DString *dsPtr)
{
    Conn    *connPtr = (Conn *) conn;
    Request *reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL || reqPtr->avail < toCopy) {
        return NS_ERROR;
    }
    Ns_DStringNAppend(dsPtr, reqPtr->next, (int)toCopy);
    reqPtr->next  += toCopy;
    reqPtr->avail -= toCopy;

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnCopyToFile, Fd, Channel --
 *
 *      Copy data from a connection to a channel, FILE, or fd.
 *
 * Results:
 *      NS_OK or NS_ERROR
 *
 * Side effects:
 *      See ConnCopy().
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnCopyToChannel(Ns_Conn *conn, size_t ncopy, Tcl_Channel chan)
{
    return ConnCopy(conn, ncopy, chan, NULL, -1);
}

int
Ns_ConnCopyToFile(Ns_Conn *conn, size_t ncopy, FILE *fp)
{
    return ConnCopy(conn, ncopy, NULL, fp, -1);
}

int
Ns_ConnCopyToFd(Ns_Conn *conn, size_t ncopy, int fd)
{
    return ConnCopy(conn, ncopy, NULL, NULL, fd);
}

static int
ConnCopy(Ns_Conn *conn, size_t toCopy, Tcl_Channel chan, FILE *fp, int fd)
{
    Conn    *connPtr;
    Request *reqPtr;
    size_t   ncopy = toCopy;
    ssize_t  nwrote;

    NS_NONNULL_ASSERT(conn != NULL);

    connPtr = (Conn *) conn;
    reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL || reqPtr->avail < toCopy) {
        return NS_ERROR;
    }
    while (ncopy > 0u) {
        if (chan != NULL) {
            nwrote = Tcl_Write(chan, reqPtr->next, (int)ncopy);
        } else if (fp != NULL) {
	    nwrote = (ssize_t)fwrite(reqPtr->next, 1u, ncopy, fp);
            if (ferror(fp) != 0) {
                nwrote = -1;
            }
        } else {
            nwrote = ns_write(fd, reqPtr->next, ncopy);
        }
        if (nwrote < 0) {
            return NS_ERROR;
        }
        ncopy -= (size_t)nwrote;
        reqPtr->next  += nwrote;
        reqPtr->avail -= (size_t)nwrote;
    }
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CompleteHeaders --
 *
 *      Construct a set of headers including length, connection and
 *      transfer-encoding and then dump them to the dstring.
 *
 * Results:
 *      1 if headers were dumped to dstring, 0 otherwise.
 *
 * Side effects:
 *      The connections STREAM and/or CHUNK flags may be set.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_CompleteHeaders(Ns_Conn *conn, size_t dataLength, 
		   unsigned int flags, Ns_DString *dsPtr)
{
    Conn       *connPtr = (Conn *) conn;
    const char *keepString;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if ((conn->flags & NS_CONN_SKIPHDRS) != 0u) {
        return NS_FALSE;
    }

    /*
     * Check for streaming vs. non-streaming.
     */

    if ((flags & NS_CONN_STREAM) != 0u) {

        conn->flags |= NS_CONN_STREAM;

        if ((connPtr->responseLength < 0)
            && (conn->request.version > 1.0)
            && (connPtr->keep != 0)
            && (HdrEq(connPtr->outputheaders, "Content-Type",
                      "multipart/byteranges") == NS_FALSE)) {
            conn->flags |= NS_CONN_CHUNK;
        }

    } else if (connPtr->responseLength < 0) {
      Ns_ConnSetLengthHeader(conn, dataLength, 0);
    }

    /*
     * Set and construct the headers.
     */

    connPtr->keep = CheckKeep(connPtr);
    if (connPtr->keep != 0) {
        keepString = "keep-alive";
    } else {
        keepString = "close";
    }
    Ns_ConnSetHeaders(conn, "Connection", keepString);

    if ((conn->flags & NS_CONN_CHUNK) != 0u) {
        Ns_ConnSetHeaders(conn, "Transfer-Encoding", "chunked");
    }
    Ns_ConnConstructHeaders(conn, dsPtr);

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * CheckKeep --
 *
 *      Should the Connection header be set to keep-alive or close.
 *
 * Results:
 *      1 if keep-alive enabled, 0 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
CheckKeep(const Conn *connPtr)
{
    NS_NONNULL_ASSERT(connPtr != NULL);

    if (connPtr->drvPtr->keepwait > 0) {

        /*
         * Check for manual keep-alive override.
         */

        if (connPtr->keep > 0) {
            return NS_TRUE;
        }

        /*
         * Apply default rules.
         */

        if ((connPtr->keep == -1)
            && (connPtr->request.line != NULL)) {

            /*
             * HTTP 1.0/1.1 keep-alive header checks.
             */
            if ((   (connPtr->request.version == 1.0)
                 && (HdrEq(connPtr->headers, "connection", "keep-alive") == NS_TRUE) )
                ||
                (   (connPtr->request.version > 1.0)
                 && (HdrEq(connPtr->headers, "connection", "close") == NS_FALSE) )
                ) {

                /*
                 * POST, PUT etc. require a content-length header to allow keep-alive
                 */
                if ((connPtr->contentLength > 0u)
		    && (Ns_SetIGet(connPtr->headers, "Content-Length") == NULL)) {
                    return NS_FALSE;
                }

		if (   (connPtr->drvPtr->keepmaxuploadsize > 0u)
		    && (connPtr->contentLength > connPtr->drvPtr->keepmaxuploadsize) ) {
		    Ns_Log(Notice, 
			   "Disallow keep-alive, content-Length %" PRIdz 
			   " larger keepmaxuploadsize %" PRIdz ": %s",
			   connPtr->contentLength, connPtr->drvPtr->keepmaxuploadsize,
			   connPtr->request.line);
		    return NS_FALSE;
		} else if (   (connPtr->drvPtr->keepmaxdownloadsize > 0u)
			   && (connPtr->responseLength > 0)
			   && ((size_t)connPtr->responseLength > connPtr->drvPtr->keepmaxdownloadsize) ) {
		    Ns_Log(Notice, 
			   "Disallow keep-alive response length %" PRIdz " "
			   "larger keepmaxdownloadsize %" PRIdz ": %s",
			   connPtr->responseLength, connPtr->drvPtr->keepmaxdownloadsize,
			   connPtr->request.line);
		    return NS_FALSE;
		}
		
                /*
                 * We allow keep-alive for chunked encoding variants or a valid
                 * content-length header.
                 */
		if (((connPtr->flags & NS_CONN_CHUNK) != 0u)
                    || (Ns_SetIGet(connPtr->outputheaders, "Content-Length") != NULL)
                    || (HdrEq(connPtr->outputheaders, "Content-Type", "multipart/byteranges") == NS_TRUE)) {
		    return NS_TRUE;
                }
            }
        }
    }

    /*
     * Don't allow keep-alive.
     */
    return NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * HdrEq --
 *
 *      Test if given set contains a key which matches given value.
 *      Value is matched at the beginning of the header value only.
 *
 * Results:
 *      1 if there is a match, 0 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
HdrEq(const Ns_Set *set, const char *name, const char *value)
{
    const char *hdrvalue;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    hdrvalue = Ns_SetIGet(set, name);

    if ((hdrvalue != NULL) && (strncasecmp(hdrvalue, value, strlen(value)) == 0)) {
        return NS_TRUE;
    }
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
