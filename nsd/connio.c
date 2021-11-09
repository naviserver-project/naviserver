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
 * encoding character data and for transferring data from disk to the
 * network, and so defines the chunk size of writes to the network.
 */

#define IOBUFSZ 8192

/*
 * The chunked encoding header consists of a hex number followed by
 * CRLF (see e.g. RFC 2616 section 3.6.1). It has to fit the maximum
 * number of digits of a 64 byte number is 8, plus CRLF + NULL.
 */
#define MAX_CHARS_CHUNK_HEADER 12


/*
 * Local functions defined in this file
 */

static Ns_ReturnCode ConnSend(Ns_Conn *conn, ssize_t nsend, Tcl_Channel chan,
                              FILE *fp, int fd)
    NS_GNUC_NONNULL(1);

static Ns_ReturnCode ConnCopy(const Ns_Conn *conn, size_t toCopy, Tcl_Channel chan,
                              FILE *fp, int fd)
    NS_GNUC_NONNULL(1);

static bool CheckKeep(const Conn *connPtr)
    NS_GNUC_NONNULL(1);

static int CheckCompress(const Conn *connPtr, const struct iovec *bufs, int nbufs, unsigned int ioflags)
    NS_GNUC_NONNULL(1);

static bool HdrEq(const Ns_Set *set, const char *name, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnWriteChars, Ns_ConnWriteVChars --
 *
 *      This will write a string buffer to the conn.  The distinction
 *      being that the given data is explicitly a UTF8 character
 *      string, and will be put out in an 'encoding-aware' manner.  It
 *      promises to write all of it.
 *
 * Results:
 *      NS_OK if all data written, NS_ERROR otherwise.
 *
 * Side effects:
 *      See Ns_ConnWriteVData().
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnWriteChars(Ns_Conn *conn, const char *buf, size_t toWrite, unsigned int flags)
{
    struct iovec sbuf;

    sbuf.iov_base = (void *) buf;
    sbuf.iov_len  = toWrite;
    return Ns_ConnWriteVChars(conn, &sbuf, 1, flags);
}

Ns_ReturnCode
Ns_ConnWriteVChars(Ns_Conn *conn, struct iovec *bufs, int nbufs, unsigned int flags)
{
    Conn              *connPtr   = (Conn *) conn;
    Ns_DString         encDs, gzDs;
    struct iovec       iov;
    Ns_ReturnCode      status;

    Ns_DStringInit(&encDs);
    Ns_DStringInit(&gzDs);

    /*
     * Transcode to charset if necessary. In earlier versions, the
     * transcoding was guarded by "!NsEncodingIsUtf8()", which was an
     * optimization. However, we cannot assume that the internal Tcl
     * UTF-8 is the same as an external, especially for emoji and
     * other multibyte characters.
     */

    if (connPtr->outputEncoding != NULL
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
        bool flush = ((flags & NS_CONN_STREAM) == 0u);

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
CheckCompress(const Conn *connPtr, const struct iovec *bufs, int nbufs, unsigned int ioflags)
{
    const Ns_Conn  *conn = (Ns_Conn *) connPtr;
    const NsServer *servPtr;
    int             configuredCompressionLevel, compressionLevel = 0;

    NS_NONNULL_ASSERT(connPtr != NULL);

    servPtr = connPtr->poolPtr->servPtr;

    /*
     * Check the default setting and explicit override.
     */
    configuredCompressionLevel = Ns_ConnGetCompression(conn);

    if (configuredCompressionLevel > 0) {
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
                    compressionLevel = configuredCompressionLevel;
                }
            }
        }
    }
    return compressionLevel;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnWriteData, Ns_ConnWriteVData --
 *
 *      Send zero or more buffers of raw bytes to the client, possibly
 *      using the HTTP chunked encoding if flags includes
 *      NS_CONN_STREAM.
 *
 *      Ns_ConnWriteVData() is called with (nbufs == 0) to flush
 *      headers.
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
    Ns_DString    ds;
    int           nsbufs, sbufIdx;
    size_t        bodyLength, toWrite, neededBufs;
    ssize_t       nwrote;
    struct iovec  sbufs[32], *sbufPtr;
    char          hdr[MAX_CHARS_CHUNK_HEADER]; /* Address of this
                                                  variable might be
                                                  used in
                                                  Ns_ConnSend(),
                                                  therefore, we cannot
                                                  reduce scope. */

    NS_NONNULL_ASSERT(conn != NULL);
    //NS_NONNULL_ASSERT(bufs != NULL);

    Ns_DStringInit(&ds);

    /*
     * Make sure there's enough send buffers to contain the given
     * buffers, a set of optional HTTP headers, and an optional HTTP
     * chunked header/footer pair. Use the stack if possible.
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
        conn->flags |= NS_CONN_STREAM;
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

            /*
             * Output content with chunking header/trailers.
             */

            if (bodyLength > 0u) {
                size_t len;

                assert(nbufs > 0);
                assert(bufs != NULL);

                /*
                 * Output length header followed by content and then
                 * trailer.
                 */
                len = (size_t)snprintf(hdr, sizeof(hdr), "%lx\r\n", (unsigned long)bodyLength);
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
                conn->flags &= ~NS_CONN_STREAM;
                conn->flags |= NS_CONN_SENT_LAST_CHUNK;
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
 *      Send some number of bytes to an open channel, FILE or fd.
 *      If the number is negative, send until EOF condition on source.
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
Ns_ConnSendChannel(Ns_Conn *conn, Tcl_Channel chan, ssize_t nsend)
{
    return ConnSend(conn, nsend, chan, NULL, -1);
}

Ns_ReturnCode
Ns_ConnSendFp(Ns_Conn *conn, FILE *fp, ssize_t nsend)
{
    return ConnSend(conn, nsend, NULL, fp, -1);
}

Ns_ReturnCode
Ns_ConnSendFd(Ns_Conn *conn, int fd, ssize_t nsend)
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
 *      via Ns_ConnWriteVData(). Stop transmission on error, when all
 *      requested data was sent or EOF condition on channel/FILE/fd.
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
ConnSend(Ns_Conn *conn, ssize_t nsend, Tcl_Channel chan, FILE *fp, int fd)
{
    Ns_ReturnCode status;
    unsigned int flags = 0;

    NS_NONNULL_ASSERT(conn != NULL);
    assert(chan != NULL || fp != NULL || fd > -1);

    if (nsend == 0) {
        /*
         * Even if no data to send, ensure HTTP response headers get written.
         */
        status = Ns_ConnWriteVData(conn, NULL, 0, flags);

    } else {
        bool stream = NS_FALSE, eod = NS_FALSE;
        char buf[IOBUFSZ];
        struct iovec vbuf;

        vbuf.iov_base = (void *)buf;

        /*
         * Turn-on http-streaming for unknown content/data length
         */
        if (nsend == -1) {
            stream = NS_TRUE;
            flags |= NS_CONN_STREAM;
        }

        /*
         * Read from disk and send in (max) IOBUFSZ chunks until
         * all requested data was sent or until EOF condition on source.
         */

        status = NS_OK;

        while (status == NS_OK && (nsend > 0 || (stream && !eod))) {
            ssize_t nread = 0;
            size_t  toRead = 0;

            if (stream) {
                toRead = sizeof(buf);
            } else {
                toRead = ((size_t)nsend > sizeof(buf)) ? sizeof(buf) : (size_t)nsend;
            }
            if (chan != NULL) {
                nread = Tcl_Read(chan, buf, (int)toRead);
                if (stream && Tcl_Eof(chan)) {
                    eod = NS_TRUE;
                }
            } else if (fp != NULL) {
                nread = (int)fread(buf, 1u, toRead, fp);
                if (ferror(fp)) {
                    nread = -1;
                } else if (stream && feof(fp)) {
                    eod = NS_TRUE;
                }
            } else if (fd > -1) {
                nread = ns_read(fd, buf, toRead);
                if (stream && nread == 0) {
                    eod = NS_TRUE;
                }
            } else {
                status = NS_ERROR; /* Should never be reached */
            }
            if (nread == -1 || (!stream && nread == 0) /* NB: truncated file */) {
                status = NS_ERROR;
            } else if (nread > 0) {
                vbuf.iov_len = (size_t)nread;
                status = Ns_ConnWriteVData(conn, &vbuf, 1, flags);
                if (status == NS_OK && !stream) {
                    nsend -= nread;
                }
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
 *      Send a vector of file buffers directly to the connection socket.
 *      Promises to send all of the data.
 *
 * Results:
 *      NS_OK if all data sent, NS_ERROR otherwise.
 *
 * Side effects:
 *      Will update connPtr->nContentSent.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnSendFileVec(Ns_Conn *conn, Ns_FileVec *bufs, int nbufs)
{
    Conn        *connPtr;
    Sock        *sockPtr;
    int          i;
    size_t       nwrote = 0u, towrite = 0u;
    Ns_Time      waitTimeout;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);

    connPtr = (Conn *)conn;
    sockPtr = (Sock *)connPtr->sockPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(sockPtr->drvPtr != NULL);

    waitTimeout.sec  = sockPtr->drvPtr->sendwait.sec;
    waitTimeout.usec = sockPtr->drvPtr->sendwait.usec;

    for (i = 0; i < nbufs; i++) {
        towrite += bufs[i].length;
    }

    while (nwrote < towrite) {
        ssize_t sent;

        sent = NsDriverSendFile(sockPtr, bufs, nbufs, 0u);
        if (sent == -1) {
            break;
        }
        nwrote += (size_t)sent;
        if (nwrote < towrite) {
            Ns_Sock *sock = (Ns_Sock *)sockPtr;

            if (sent > 0) {
                (void)Ns_ResetFileVec(bufs, nbufs, (size_t)sent);
            }
            if (Ns_SockTimedWait(sock->sock, NS_SOCK_WRITE,
                                 &waitTimeout) != NS_OK) {
                break;
            }
        }
    }

    if (likely(nwrote > 0u)) {
        connPtr->nContentSent += nwrote;
    }

    return (nwrote != towrite) ? NS_ERROR : NS_OK;
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
 *      Send buffers to the connection socket efficiently.
 *      It promises to send all data.
 *
 * Results:
 *      Number of bytes sent, -1 on error.
 *
 * Side effects:
 *      Will update connPtr->nContentSent.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_ConnSend(Ns_Conn *conn, struct iovec *bufs, int nbufs)
{
    ssize_t  sent;
    int      i;
    size_t   towrite = 0u;

    for (i = 0; i < nbufs; i++) {
        towrite += bufs[i].iov_len;
    }

    if (towrite == 0u) {
        sent = 0;

    } else if (NsWriterQueue(conn, towrite, NULL, NULL, NS_INVALID_FD,
                             bufs, nbufs, NULL, 0, NS_FALSE) == NS_OK) {
        Ns_Log(Debug, "==== writer sent %" PRIuz " bytes\n", towrite);
        sent = (ssize_t)towrite;

    } else {
        Ns_Time  waitTimeout;
        Conn    *connPtr;
        Sock    *sockPtr;

        NS_NONNULL_ASSERT(conn != NULL);

        connPtr = (Conn *)conn;
        sockPtr = connPtr->sockPtr;

        NS_NONNULL_ASSERT(sockPtr != NULL);
        NS_NONNULL_ASSERT(sockPtr->drvPtr != NULL);

        waitTimeout.sec  = sockPtr->drvPtr->sendwait.sec;
        waitTimeout.usec = sockPtr->drvPtr->sendwait.usec;

        sent = Ns_SockSendBufs((Ns_Sock*)sockPtr, bufs, nbufs, &waitTimeout, 0u);

        if (likely(sent > 0)) {
            connPtr->nContentSent += (size_t)sent;
        }
        NsPoolAddBytesSent(((Conn *)conn)->poolPtr,  (Tcl_WideInt)connPtr->nContentSent);
    }

    return sent;
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

Ns_ReturnCode
Ns_ConnFlushContent(const Ns_Conn *conn)
{
    const Conn   *connPtr = (const Conn *) conn;
    Request      *reqPtr = connPtr->reqPtr;
    Ns_ReturnCode status = NS_OK;

    if (connPtr->sockPtr == NULL) {
        status = NS_ERROR;
    } else {
        reqPtr->next  += reqPtr->avail;
        reqPtr->avail  = 0u;
    }
    return status;
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

Ns_ReturnCode
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
            NsSockClose(connPtr->sockPtr, connPtr->keep);
        }


        connPtr->sockPtr = NULL;
        connPtr->flags |= NS_CONN_CLOSED;
        Ns_Log(Ns_LogRequestDebug, "connection closed");

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
    const Conn   *connPtr = (const Conn *) conn;
    size_t        n;
    Ns_ReturnCode status;
    int           result;
    struct iovec  vbuf;

    vbuf.iov_base = (void *) buf;
    vbuf.iov_len  = toWrite;

    n = connPtr->nContentSent;
    status = Ns_ConnWriteVData(conn, &vbuf, 1, 0u);
    if (status == NS_OK) {
        result = (int)connPtr->nContentSent - (int)n;
    } else {
        result = -1;
    }
    return result;
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

Ns_ReturnCode
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
 *      Read in a string from a connection, stopping when either we've
 *      run out of data, hit a newline, or had an error.
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
Ns_ConnGets(char *buf, size_t bufsize, const Ns_Conn *conn)
{
    char *p, *result = buf;

    NS_NONNULL_ASSERT(buf != NULL);
    NS_NONNULL_ASSERT(conn != NULL);

    p = buf;
    while (bufsize > 1u) {
        if (Ns_ConnRead(conn, p, 1u) != 0u) {
            result = NULL;
            break;
        }
        if (*p++ == '\n') {
            break;
        }
        --bufsize;
    }
    if (likely(result != NULL)) {
        *p = '\0';
    }

    return result;
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
Ns_ConnRead(const Ns_Conn *conn, void *vbuf, size_t toRead)
{
    const Conn *connPtr = (const Conn *) conn;
    Request    *reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL) {
        toRead = 0u;
    } else {
        if (toRead > reqPtr->avail) {
            toRead = reqPtr->avail;
        }
        memcpy(vbuf, reqPtr->next, toRead);
        reqPtr->next  += toRead;
        reqPtr->avail -= toRead;
    }

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

Ns_ReturnCode
Ns_ConnReadLine(const Ns_Conn *conn, Ns_DString *dsPtr, size_t *nreadPtr)
{
    const Conn   *connPtr;
    Request      *reqPtr;
    const Driver *drvPtr;
    const char   *eol;
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    connPtr = (const Conn *) conn;
    reqPtr = connPtr->reqPtr;
    assert(reqPtr != NULL);

    drvPtr = connPtr->drvPtr;
    eol = strchr(reqPtr->next, INTCHAR('\n'));

    if ((connPtr->sockPtr == NULL) || (eol == NULL)) {
        status = NS_ERROR;
    } else {
        ptrdiff_t nread = eol - reqPtr->next;

        if (nread > drvPtr->maxline) {
            status = NS_ERROR;
        } else {
            ptrdiff_t ncopy = nread;

            ++nread;
            if (nreadPtr != NULL) {
                *nreadPtr = (size_t)nread;
            }

            /*
             * Read from the end of the buffer until we either reach
             * ncopy == 0 (this means the start of the buffer), or
             * until we find a '\r'.
             */
            if (ncopy > 0u && *(eol-1) == '\r') {
                --ncopy;
            }
            Ns_DStringNAppend(dsPtr, reqPtr->next, (int)ncopy);
            reqPtr->next  += nread;
            reqPtr->avail -= (size_t)nread;

            status = NS_OK;
        }
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReadHeaders --
 *
 *      Read the headers and insert them into the passed-in set.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Stuff will be read from the conn.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReadHeaders(const Ns_Conn *conn, Ns_Set *set, size_t *nreadPtr)
{
    Ns_DString      ds;
    const Conn     *connPtr = (const Conn *) conn;
    size_t          nread, maxhdr;
    Ns_ReturnCode   status = NS_OK;

    Ns_DStringInit(&ds);
    nread = 0u;
    maxhdr = (size_t)connPtr->drvPtr->maxheaders;
    while (nread < maxhdr && status == NS_OK) {
        size_t nline;

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

Ns_ReturnCode
Ns_ConnCopyToDString(const Ns_Conn *conn, size_t toCopy, Ns_DString *dsPtr)
{
    const Conn    *connPtr;
    Request       *reqPtr;
    Ns_ReturnCode  status = NS_OK;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    connPtr = (const Conn *)conn;
    reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL || reqPtr->avail < toCopy) {
        status = NS_ERROR;
    } else {
        Ns_DStringNAppend(dsPtr, reqPtr->next, (int)toCopy);
        reqPtr->next  += toCopy;
        reqPtr->avail -= toCopy;
    }

    return status;
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

Ns_ReturnCode
Ns_ConnCopyToChannel(const Ns_Conn *conn, size_t ncopy, Tcl_Channel chan)
{
    return ConnCopy(conn, ncopy, chan, NULL, -1);
}

Ns_ReturnCode
Ns_ConnCopyToFile(const Ns_Conn *conn, size_t ncopy, FILE *fp)
{
    return ConnCopy(conn, ncopy, NULL, fp, -1);
}

Ns_ReturnCode
Ns_ConnCopyToFd(const Ns_Conn *conn, size_t ncopy, int fd)
{
    return ConnCopy(conn, ncopy, NULL, NULL, fd);
}

static Ns_ReturnCode
ConnCopy(const Ns_Conn *conn, size_t toCopy, Tcl_Channel chan, FILE *fp, int fd)
{
    const Conn   *connPtr;
    Request      *reqPtr;
    size_t        ncopy = toCopy;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(conn != NULL);

    connPtr = (const Conn *) conn;
    reqPtr = connPtr->reqPtr;
    assert(reqPtr != NULL);

    if (connPtr->sockPtr == NULL || reqPtr->avail < toCopy) {
        status = NS_ERROR;
    } else {
        /*
         * There is data to copy.
         */
        while (ncopy > 0u) {
            ssize_t nwrote;

            /*
             * Write it to the chan, or fp, or fd, depending on what
             * was provided.
             */
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
                status = NS_ERROR;
                break;
            } else {
                ncopy -= (size_t)nwrote;
                reqPtr->next  += nwrote;
                reqPtr->avail -= (size_t)nwrote;
            }
        }
    }
    return status;
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
    bool        success;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if ((conn->flags & NS_CONN_SKIPHDRS) != 0u) {
        /*
         * Pre-HTTP/1.0 has no headers, and no keep-alive
         */
        if (conn->request.version < 1.0) {
            connPtr->keep = 0;
        }
        success = NS_FALSE;
    } else {
        const char *keepString;

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
            Ns_ConnSetLengthHeader(conn, dataLength, NS_FALSE);
        }

        /*
         * Set and construct the headers.
         */

        connPtr->keep = (CheckKeep(connPtr) ? 1 : 0);
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
        success = NS_TRUE;
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * CheckKeep --
 *
 *      Should the Connection header be set to keep-alive or close.
 *
 * Results:
 *      NS_TRUE if keep-alive is allowed, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
CheckKeep(const Conn *connPtr)
{
    bool result = NS_FALSE;

    NS_NONNULL_ASSERT(connPtr != NULL);

    do {
        if (connPtr->drvPtr->keepwait.sec > 0 || connPtr->drvPtr->keepwait.usec > 0 ) {
            /*
             * Check for manual keep-alive override.
             */

            if (connPtr->keep > 0) {
                result = NS_TRUE;
                break;
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
                    ||  (   (connPtr->request.version > 1.0)
                            && (HdrEq(connPtr->headers, "connection", "close") == NS_FALSE) )
                    ) {

                    /*
                     * POST, PUT etc. require a content-length header
                     * to allow keep-alive.
                     */
                    if ((connPtr->contentLength > 0u)
                        && (Ns_SetIGet(connPtr->headers, "Content-Length") == NULL)) {
                        /*
                         * No content length -> disallow.
                         */
                        break;
                    }

                    if ( (connPtr->drvPtr->keepmaxuploadsize > 0u)
                         && (connPtr->contentLength > connPtr->drvPtr->keepmaxuploadsize) ) {
                        Ns_Log(Notice,
                               "Disallow keep-alive: content-Length %" PRIdz
                               " larger keepmaxuploadsize %" PRIdz ": %s",
                               connPtr->contentLength, connPtr->drvPtr->keepmaxuploadsize,
                               connPtr->request.line);
                        break;
                    } else if ( (connPtr->drvPtr->keepmaxdownloadsize > 0u)
                                && (connPtr->responseLength > 0)
                                && ((size_t)connPtr->responseLength > connPtr->drvPtr->keepmaxdownloadsize) ) {
                        Ns_Log(Notice,
                               "Disallow keep-alive: response length %" PRIdz " "
                               "larger keepmaxdownloadsize %" PRIdz ": %s",
                               connPtr->responseLength, connPtr->drvPtr->keepmaxdownloadsize,
                               connPtr->request.line);
                        break;
                    }

                    /*
                     * We allow keep-alive for chunked encoding
                     * variants or a valid content-length header.
                     */
                    if (((connPtr->flags & NS_CONN_CHUNK) != 0u)
                        || (Ns_SetIGet(connPtr->outputheaders, "Content-Length") != NULL)
                        || (HdrEq(connPtr->outputheaders, "Content-Type", "multipart/byteranges") == NS_TRUE)) {

                        result = NS_TRUE;
                        break;
                    }
                }
            }
        }
    } while (NS_FALSE); /* loop construct just for breaks */

    /*
     * Don't allow keep-alive.
     */
    return result;
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
 *      NS_TRUE if there is a match, NS_FALSE otherwise.
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

    return ((hdrvalue != NULL) && (strncasecmp(hdrvalue, value, strlen(value)) == 0));
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
