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

static int ConnSend(Ns_Conn *conn, Tcl_WideInt nsend, Tcl_Channel chan,
                    FILE *fp, int fd);
static int ConnCopy(Ns_Conn *conn, size_t ncopy, Tcl_Channel chan,
                    FILE *fp, int fd);

static int ConstructHeaders(Ns_Conn *conn, Tcl_WideInt length, int flags,
                            Ns_DString *dsPtr);
static int CheckKeep(Conn *connPtr);
static int CheckCompress(Conn *connPtr, struct iovec *bufs, int nbufs, int ioflags);
static int HdrEq(Ns_Set *set, char *name, char *value);



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
Ns_ConnWriteChars(Ns_Conn *conn, CONST char *buf, size_t towrite, int flags)
{
    struct iovec sbuf;

    sbuf.iov_base = (void *) buf;
    sbuf.iov_len = towrite;
    return Ns_ConnWriteVChars(conn, &sbuf, 1, flags);
}

int
Ns_ConnWriteVChars(Ns_Conn *conn, struct iovec *bufs, int nbufs, int flags)
{
    Conn              *connPtr   = (Conn *) conn;
    Ns_CompressStream *streamPtr = &connPtr->stream;
    Ns_DString         encDs, gzDs;
    struct iovec       iov;
    CONST char        *utfBytes;
    int                status;

    Ns_DStringInit(&encDs);
    Ns_DStringInit(&gzDs);

    /*
     * Transcode from utf8 if neccessary.
     */

    if (connPtr->outputEncoding != NULL
        && !NsEncodingIsUtf8(connPtr->outputEncoding)
        && nbufs > 0
        && bufs[0].iov_len > 0) {
        int i;

        for (i = 0; i < nbufs; i++) {
	    size_t utfLen;

            utfBytes = bufs[i].iov_base;
            utfLen   = bufs[i].iov_len;

            if (utfLen > 0) {
                (void) Tcl_UtfToExternalDString(connPtr->outputEncoding,
                                                utfBytes, (int)utfLen, &encDs);
            }
        }
        Ns_SetVec(&iov, 0, encDs.string, encDs.length);
        bufs = &iov;
        nbufs = 1;
    }

    /*
     * Compress if possible.
     */

    if (connPtr->compress < 0) {
        connPtr->compress = CheckCompress(connPtr, bufs, nbufs, flags);
    }
    if (connPtr->compress > 0) {
        int flush = (flags & NS_CONN_STREAM) ? 0 : 1;

        if (Ns_CompressBufsGzip(streamPtr, bufs, nbufs, &gzDs,
                                connPtr->compress, flush) == NS_OK) {
            /* NB: Compression will always succeed. */
            Ns_SetVec(&iov, 0, gzDs.string, gzDs.length);
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
 *      0-9. Will return 0 if gzip support compiled out.
 *
 * Side effects:
 *      May set the Content-Encoding and Vary headers.
 *
 *----------------------------------------------------------------------
 */

static int
CheckCompress(Conn *connPtr, struct iovec *bufs, int nbufs, int ioflags)
{
    Ns_Conn  *conn    = (Ns_Conn *) connPtr;
    NsServer *servPtr = connPtr->servPtr;
    int       level, compress = 0;

    /* Check the default setting and explicit overide. */

    if ((level = Ns_ConnGetCompression(conn)) > 0) {

        /*
         * Make sure the length is above the minimum threshold, or
         * we're streaming (assume length is long enough for streams).
         */

        if ((ioflags & NS_CONN_STREAM)
            || Ns_SumVec(bufs, nbufs) >= (size_t)servPtr->compress.minsize
            || connPtr->responseLength >= servPtr->compress.minsize) {

            /* We won't be compressing if there are no headers or body. */

            if (!(connPtr->flags & NS_CONN_SENTHDRS)
		&& !(connPtr->flags & NS_CONN_SKIPBODY)) {
		
                Ns_ConnSetHeaders(conn, "Vary", "Accept-Encoding");

                if (connPtr->flags & NS_CONN_ZIPACCEPTED) {
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

int
Ns_ConnWriteData(Ns_Conn *conn, CONST void *buf, size_t towrite, int flags)
{
    struct iovec vbuf;

    vbuf.iov_base = (void *) buf;
    vbuf.iov_len = towrite;

    return Ns_ConnWriteVData(conn, &vbuf, 1, flags);
}

int
Ns_ConnWriteVData(Ns_Conn *conn, struct iovec *bufs, int nbufs, int flags)
{
    Conn         *connPtr = (Conn *) conn;
    Ns_DString    ds;
    int           nsbufs, sbufIdx;
    size_t        bodyLength, towrite;
    ssize_t       nwrote;
    char          hdr[32];
    struct iovec  sbufs[32], *sbufPtr = sbufs;

    Ns_DStringInit(&ds);

    /*
     * Make sure there's enough send buffers to contain the given
     * buffers, a set of optional HTTP headers, and an optional
     * HTTP chunked header/footer pair. Use the stack if possible.
     */

    if (nbufs + 2 + 1 > (sizeof(sbufs) / sizeof(struct iovec))) {
        sbufPtr = ns_calloc(nbufs + 2 + 1, sizeof(struct iovec));
    }
    nsbufs = 0;
    sbufIdx = 0;

    /*
     * Work out the body length for non-chunking case.
     */

    bodyLength = Ns_SumVec(bufs, nbufs);
    towrite = 0;

    if (flags & NS_CONN_STREAM) {
	connPtr->flags |= NS_CONN_STREAM;
    }

    /*
     * Send headers if not already sent.
     */

    if (!(conn->flags & NS_CONN_SENTHDRS)) {
        conn->flags |= NS_CONN_SENTHDRS;
        if (ConstructHeaders(conn, bodyLength, flags, &ds)) {
            towrite += Ns_SetVec(sbufPtr, sbufIdx++,
                                 Ns_DStringValue(&ds), Ns_DStringLength(&ds));
            nsbufs++;
        }
    }

    /*
     * Send body.
     */

    if (!(conn->flags & NS_CONN_SKIPBODY)) {

        if (!(conn->flags & NS_CONN_CHUNK)) {
            /*
             * Output content without chunking header/trailers.
             */

            if (sbufIdx == 0) {
                sbufPtr = bufs;
                nsbufs = nbufs;
            } else {
                (void) memcpy(sbufPtr + sbufIdx, bufs, nbufs * sizeof(struct iovec));
                nsbufs += nbufs;
            }
            towrite += bodyLength;

        } else {

            if (bodyLength > 0) {
                /*
                 * Output length header followed by content and then trailer.
                 */

                towrite += Ns_SetVec(sbufPtr, sbufIdx++,
                                     hdr, sprintf(hdr, "%lx\r\n", (unsigned long)bodyLength));

                (void) memcpy(sbufPtr + sbufIdx, bufs, nbufs * sizeof(struct iovec));
                sbufIdx += nbufs;
                towrite += bodyLength;

                towrite += Ns_SetVec(sbufPtr, sbufIdx++, "\r\n", 2);

                nsbufs += nbufs + 2;
            }

	    if (flags & NS_CONN_STREAM_CLOSE) {
                /*
                 * Output end-of-content trailer for chunked encoding
                 */

                towrite += Ns_SetVec(sbufPtr, sbufIdx, "0\r\n\r\n", 5);

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

    return (nwrote < (ssize_t)towrite) ? NS_ERROR : NS_OK;
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

int
Ns_ConnSendChannel(Ns_Conn *conn, Tcl_Channel chan, Tcl_WideInt nsend)
{
    return ConnSend(conn, nsend, chan, NULL, -1);
}

int
Ns_ConnSendFp(Ns_Conn *conn, FILE *fp, Tcl_WideInt nsend)
{
    return ConnSend(conn, nsend, NULL, fp, -1);
}

int
Ns_ConnSendFd(Ns_Conn *conn, int fd, Tcl_WideInt nsend)
{
    return ConnSend(conn, nsend, NULL, NULL, fd);
}

static int
ConnSend(Ns_Conn *conn, Tcl_WideInt nsend, Tcl_Channel chan, FILE *fp, int fd)
{
    int          status;
    size_t       toread, nread;
    char         buf[IOBUFSZ];

    /*
     * Even if nsend is 0 ensure HTTP response headers get written.
     */

    if (nsend == 0) {
        return Ns_ConnWriteVData(conn, NULL, 0, 0);
    }

    /*
     * Read from disk and send in IOBUFSZ chunks until done.
     */

    status = NS_OK;
    while (status == NS_OK && nsend > 0) {
	toread = (size_t)nsend;
        if (toread > sizeof(buf)) {
            toread = sizeof(buf);
        }
        if (chan != NULL) {
	    nread = Tcl_Read(chan, buf, (int)toread);
        } else if (fp != NULL) {
            nread = fread(buf, 1, toread, fp);
            if (ferror(fp)) {
                nread = -1;
            }
        } else {
            nread = read(fd, buf, toread);
        }

        if (nread == -1 || nread == 0 /* NB: truncated file */) {
            status = NS_ERROR;
        } else {
	    struct iovec vbuf;
	    vbuf.iov_base = (void *) buf;
	    vbuf.iov_len = nread;
	    if ((status = Ns_ConnWriteVData(conn, &vbuf, 1, 0)) == NS_OK) {
		nsend -= nread;
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
    ssize_t      towrite, nwrote, sent;

    nwrote = 0;
    towrite = 0;

    for (i = 0; i < nbufs; i++) {
        towrite += bufs[i].length;
    }

    while (towrite > 0) {
        sent = NsDriverSendFile(connPtr->sockPtr, bufs, nbufs, 0);
        if (sent < 1) {
            break;
        }
        if (sent < towrite) {
            Ns_ResetFileVec(bufs, nbufs, sent);
        }
        nwrote += sent;
        towrite -= sent;
    }
    if (nwrote > 0) {
        connPtr->nContentSent += nwrote;
    }

    return nwrote < towrite ? NS_ERROR : NS_OK;
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

int
Ns_ConnPuts(Ns_Conn *conn, CONST char *string)
{
    struct iovec vbuf;
    vbuf.iov_base = (void *) string;
    vbuf.iov_len  = strlen(string);
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

int
Ns_ConnSendDString(Ns_Conn *conn, Ns_DString *dsPtr)
{
    struct iovec vbuf;

    vbuf.iov_base = dsPtr->string;
    vbuf.iov_len  = dsPtr->length;
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
    size_t   towrite;
    ssize_t  nwrote, sent;

    if (connPtr->sockPtr == NULL) {
        return -1;
    }

    towrite = nwrote = sent = 0;

    for (i = 0; i < nbufs; i++) {
        towrite += bufs[i].iov_len;
    }
   
    if (towrite == 0) {
	return 0;
    }

    if (NsWriterQueue(conn, towrite, NULL, NULL, -1, bufs, nbufs, 0) == NS_OK) {
	Ns_Log(Debug, "==== writer sent %" PRIdz " bytes\n", towrite);
	return towrite;
    }

    {
	Ns_Time timeout;
	
	timeout.sec = connPtr->sockPtr->drvPtr->sendwait;
	timeout.usec = 0;
      
	sent = Ns_SockSendBufs((Ns_Sock*)connPtr->sockPtr, bufs, nbufs, &timeout, 0);
    }
    towrite -= sent;
    nwrote += sent;

    if (nwrote > 0) {
	connPtr->nContentSent += nwrote;
    }

    return (nwrote ? nwrote : sent);
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
    reqPtr->avail  = 0;

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
    Conn *connPtr = (Conn *) conn;
    
    Ns_Log(Debug, "Ns_ConnClose %p stream %.6x chunk %.6x via writer %.6x sockPtr %p", 
	   connPtr, 
	   connPtr->flags & NS_CONN_STREAM, 
	   connPtr->flags & NS_CONN_CHUNK, 
	   connPtr->flags & NS_CONN_SENT_VIA_WRITER, 
	   connPtr->sockPtr);

    if (connPtr->sockPtr != NULL) {
        int keep;

        if (connPtr->flags & NS_CONN_STREAM
             && (connPtr->flags & NS_CONN_CHUNK
                 || connPtr->compress > 0)) {
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
	if ((connPtr->flags & NS_CONN_SENT_VIA_WRITER) == 0) {
	    keep = connPtr->keep > 0 ? 1 : 0;
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
Ns_ConnWrite(Ns_Conn *conn, CONST void *buf, size_t towrite)
{
    Conn  *connPtr = (Conn *) conn;
    size_t n;
    int    status;
    struct iovec vbuf;

    vbuf.iov_base = (void *) buf;
    vbuf.iov_len  = towrite;

    n = (size_t)connPtr->nContentSent;
    status = Ns_ConnWriteVData(conn, &vbuf, 1, 0);
    if (status == NS_OK) {
      return (int)(connPtr->nContentSent - n);
    }
    return -1;
}

int
Ns_WriteConn(Ns_Conn *conn, CONST char *buf, size_t towrite)
{
    struct iovec vbuf;
    vbuf.iov_base = (void *) buf;
    vbuf.iov_len  = towrite;
    return Ns_ConnWriteVData(conn, &vbuf, 1, NS_CONN_STREAM);
}

int
Ns_WriteCharConn(Ns_Conn *conn, CONST char *buf, size_t towrite)
{
    struct iovec sbuf;

    sbuf.iov_base = (void *)buf;
    sbuf.iov_len = towrite;
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
    while (bufsize > 1) {
        if (Ns_ConnRead(conn, p, 1) != 1) {
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
Ns_ConnRead(Ns_Conn *conn, void *vbuf, size_t toread)
{
    Conn    *connPtr = (Conn *) conn;
    Request *reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL) {
        return -1;
    }
    if (toread > reqPtr->avail) {
        toread = reqPtr->avail;
    }
    memcpy(vbuf, reqPtr->next, toread);
    reqPtr->next  += toread;
    reqPtr->avail -= toread;

    return toread;
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
    if (ncopy > 0 && eol[-1] == '\r') {
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
    NsServer       *servPtr = connPtr->servPtr;
    size_t          nread, nline, maxhdr;
    int             status;

    Ns_DStringInit(&ds);
    nread = 0;
    maxhdr = connPtr->drvPtr->maxheaders;
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
                status = Ns_ParseHeader(set, ds.string, servPtr->opts.hdrcase);
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
Ns_ConnCopyToDString(Ns_Conn *conn, size_t tocopy, Ns_DString *dsPtr)
{
    Conn    *connPtr = (Conn *) conn;
    Request *reqPtr = connPtr->reqPtr;

    if (connPtr->sockPtr == NULL || reqPtr->avail < tocopy) {
        return NS_ERROR;
    }
    Ns_DStringNAppend(dsPtr, reqPtr->next, (int)tocopy);
    reqPtr->next  += tocopy;
    reqPtr->avail -= tocopy;

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
ConnCopy(Ns_Conn *conn, size_t tocopy, Tcl_Channel chan, FILE *fp, int fd)
{
    Conn    *connPtr = (Conn *) conn;
    Request *reqPtr = connPtr->reqPtr;
    long     nwrote, ncopy = (long)tocopy;

    if (connPtr->sockPtr == NULL || reqPtr->avail < tocopy) {
        return NS_ERROR;
    }
    while (ncopy > 0) {
        if (chan != NULL) {
            nwrote = Tcl_Write(chan, reqPtr->next, ncopy);
        } else if (fp != NULL) {
	  nwrote = (long)fwrite(reqPtr->next, 1, (size_t)ncopy, fp);
            if (ferror(fp)) {
                nwrote = -1;
            }
        } else {
            nwrote = write(fd, reqPtr->next, (size_t)ncopy);
        }
        if (nwrote < 0) {
            return NS_ERROR;
        }
        ncopy -= nwrote;
        reqPtr->next  += nwrote;
        reqPtr->avail -= nwrote;
    }
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * ConstructHeaders --
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

static int
ConstructHeaders(Ns_Conn *conn, Tcl_WideInt dataLength, int flags,
                 Ns_DString *dsPtr)
{
    Conn       *connPtr = (Conn *) conn;
    CONST char *keep;

    if (conn->flags & NS_CONN_SKIPHDRS) {
        return 0;
    }

    /*
     * Check for streaming vs. non-streaming.
     */

    if (flags & NS_CONN_STREAM) {

        conn->flags |= NS_CONN_STREAM;

        if (connPtr->responseLength < 0
            && conn->request->version > 1.0
            && connPtr->keep != 0
            && !HdrEq(connPtr->outputheaders, "Content-Type",
                                              "multipart/byteranges")) {
            conn->flags |= NS_CONN_CHUNK;
        }
    } else if (connPtr->responseLength < 0) {
        Ns_ConnSetLengthHeader(conn, dataLength);
    }

    /*
     * Set and construct the headers.
     */

    if ((connPtr->keep = CheckKeep(connPtr))) {
        keep = "keep-alive";
    } else {
        keep = "close";
    }
    Ns_ConnSetHeaders(conn, "Connection", keep);

    if (conn->flags & NS_CONN_CHUNK) {
        Ns_ConnSetHeaders(conn, "Transfer-Encoding", "chunked");
    }
    Ns_ConnConstructHeaders(conn, dsPtr);

    return 1;
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

static int
CheckKeep(Conn *connPtr)
{
    if (connPtr->drvPtr->keepwait > 0) {

        /*
         * Check for manual keep-alive override.
         */

        if (connPtr->keep > 0) {
            return 1;
        }

        /*
         * Apply default rules.
         */

        if (connPtr->keep == -1
            && connPtr->request != NULL) {

            /*
             * HTTP 1.0/1.1 keep-alive header checks.
             */
            if ((connPtr->request->version == 1.0
                 && HdrEq(connPtr->headers, "connection", "keep-alive"))
                || (connPtr->request->version > 1.0
                    && !HdrEq(connPtr->headers, "connection", "close"))) {

                /*
                 * POST, PUT etc. require a content-length header to allow keep-alive
                 */
                if (connPtr->contentLength > 0
		    && !Ns_SetIGet(connPtr->headers, "Content-Length")) {
                    return 0;
                }

		if (connPtr->drvPtr->keepmaxuploadsize
		    && connPtr->contentLength > (size_t)connPtr->drvPtr->keepmaxuploadsize) {
		    Ns_Log(Notice, "Disallow keep-alive, content-Length %" PRIdz " larger keepmaxuploadsize %d: %s",
			   connPtr->contentLength, connPtr->drvPtr->keepmaxuploadsize,
			   connPtr->request->line);
		    return 0;
		} else if (connPtr->drvPtr->keepmaxdownloadsize
			   && connPtr->responseLength > connPtr->drvPtr->keepmaxdownloadsize) {
		    Ns_Log(Notice, "Disallow keep-alive response length %" TCL_LL_MODIFIER
			   "d larger keepmaxdownloadsize %d: %s",
			   connPtr->responseLength, connPtr->drvPtr->keepmaxdownloadsize,
			   connPtr->request->line);
		    return 0;
		}
		
                /*
                 * We allow keep-alive for chunked encoding variants or a valid
                 * content-length header.
                 */
		if ((connPtr->flags & NS_CONN_CHUNK)
                        || Ns_SetIGet(connPtr->outputheaders, "Content-Length")
                        || HdrEq(connPtr->outputheaders, "Content-Type",
                                 "multipart/byteranges")) {
		    return 1;
                }
            }
        }
    }

    /*
     * Don't allow keep-alive.
     */
    return 0;
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

static int
HdrEq(Ns_Set *set, char *name, char *value)
{
    char *hdrvalue;

    if (set != NULL
        && (hdrvalue = Ns_SetIGet(set, name)) != NULL
        && strncasecmp(hdrvalue, value, strlen(value)) == 0) {
        return 1;
    }
    return 0;
}
