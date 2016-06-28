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
 * compress.c --
 *
 *      Support for gzip compression using Zlib.
 */

#include "nsd.h"

#ifdef HAVE_ZLIB_H

# define COMPRESS_SENT_HEADER 0x01u
# define COMPRESS_FLUSHED     0x02u


/*
 * Static functions defined in this file.
 */

static void DeflateOrAbort(z_stream *z, int flushFlags);
static voidpf ZAlloc(voidpf arg, uInt items, uInt size);
static void ZFree(voidpf arg, voidpf address);


/*
 *----------------------------------------------------------------------
 *
 * Ns_CompressInit, Ns_CompressFree --
 *
 *      Initialize a copression stream buffer. Do this once.
 *
 * Results:
 *      Ns_ReturnCode
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_CompressInit(Ns_CompressStream *cStream)
{
    z_stream      *z = &cStream->z;
    Ns_ReturnCode  status;

    cStream->flags = 0u;
    z->zalloc = ZAlloc;
    z->zfree = ZFree;
    z->opaque = Z_NULL;

    /*  
     * Memory requirements (see zconf.h):
     *    (1 << (windowBits+2)) +  (1 << (memLevel+9)) =
     *    (1 << (15+2)) +  (1 << (9+9)) = 393216 = ~400KB
     */

    status = deflateInit2(z,
                          Z_BEST_COMPRESSION, /* to size memory, will be reset later */
                          Z_DEFLATED, /* method. */
                          15 + 16,    /* windowBits: 15 (max), +16 (Gzip header/footer). */
                          9,          /* memlevel: 1-9 (min-max), default: 8.*/
                          Z_DEFAULT_STRATEGY);
    if (status != Z_OK) {
      /*
       * When the stream is already closed from the client side, don't
       * kill the server via Fatal(). The stream might be already
       * closed, when a huge number of requests was queued and the
       * client gives up quickly.
       */
      if (status == Z_STREAM_ERROR) {
        Ns_Log(Notice, "Ns_CompressInit: zlib error: %d (%s): %s",
                 status, zError(status), (z->msg != NULL) ? z->msg : "(none)");
	return NS_ERROR;
      } else {
        Ns_Fatal("Ns_CompressInit: zlib error: %d (%s): %s",
                 status, zError(status), (z->msg != NULL) ? z->msg : "(none)");
      }
    }

    return NS_OK;
}

void
Ns_CompressFree(Ns_CompressStream *cStream)
{
    z_stream *z = &cStream->z;

    if (z->zalloc != NULL) {
	int status = deflateEnd(z);
	if (status != Z_OK && status != Z_DATA_ERROR) {
	    Ns_Log(Bug, "Ns_CompressFree: deflateEnd: %d (%s): %s",
		   status, zError(status), (z->msg != NULL) ? z->msg : "(unknown)");
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_InflateInit, Ns_InflateBufferInit, Ns_InflateBuffer, Ns_InflateEnd --
 *
 *      Initialize decompression stream, decompress from the stream
 *      and terminate stream.
 *
 * Results:
 *      Ns_ReturnCode
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_InflateInit(Ns_CompressStream *cStream)
{
    z_stream      *zPtr = &cStream->z;
    int            rc;
    Ns_ReturnCode  status = NS_OK;

    zPtr->zalloc   = ZAlloc;
    zPtr->zfree    = ZFree;
    zPtr->opaque   = Z_NULL;
    zPtr->avail_in = 0;
    zPtr->next_in  = Z_NULL;
    rc = inflateInit2(zPtr, 15 + 16); /* windowBits: 15 (max), +16 (Gzip header/footer). */
    if (rc != Z_OK) {
	Ns_Log(Bug, "Ns_Compress: inflateInit: %d (%s): %s",
	       rc, zError(rc), (zPtr->msg != NULL) ? zPtr->msg : "(unknown)");
	status = NS_ERROR;
    }
    return status;
}


Ns_ReturnCode
Ns_InflateBufferInit(Ns_CompressStream *cStream, const char *buffer, size_t inSize) 
{
    z_stream *zPtr = &cStream->z;

    zPtr->avail_in = inSize;
    zPtr->next_in  = (unsigned char *)buffer;

    return NS_OK;
}

int
Ns_InflateBuffer(Ns_CompressStream *cStream, const char *buffer, size_t outSize, size_t *nrBytes) 
{
    z_stream     *zPtr = &cStream->z;
    int           rc;
    int           tclStatus = TCL_OK;
    
    zPtr->avail_out = outSize;
    zPtr->next_out  = (unsigned char *)buffer;
    rc = inflate(zPtr, Z_NO_FLUSH);

    if (rc != Z_OK && rc != Z_PARTIAL_FLUSH) {
	Ns_Log(Bug, "Ns_Compress: inflateBuffer: %d (%s); %s",
	       rc, zError(rc), (zPtr->msg != NULL) ? zPtr->msg : "(unknown)");
	tclStatus = TCL_ERROR;
    } else if (zPtr->avail_out == 0) {
	tclStatus = TCL_CONTINUE;
    }
    
    *nrBytes = outSize-zPtr->avail_out;

    return tclStatus;
}

Ns_ReturnCode
Ns_InflateEnd(Ns_CompressStream *cStream) 
{
    z_stream     *zPtr = &cStream->z;
    int           rc;
    Ns_ReturnCode status = NS_OK;

    rc = inflateEnd(zPtr);
    if (rc != Z_OK) {
	Ns_Log(Bug, "Ns_Compress: inflateEnd: %d (%s); %s",
	       rc, zError(rc), (zPtr->msg != NULL) ? zPtr->msg : "(unknown)");
	status = NS_ERROR;
    }
    return status;

}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CompressBufsGzip --
 *
 *      Compress a vector of bufs and append to dstring.
 *
 *      Flags must contain NS_COMPRESS_BEGIN on first call and
 *      NS_COMPRESS_END on the last, to add correct gzip
 *      header/footer. Function may be called any number of times inbetween.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      Aborts on error (which should not happen).
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_CompressBufsGzip(Ns_CompressStream *cStream, struct iovec *bufs, int nbufs,
                    Ns_DString *dsPtr, int level, bool flush)
{
    z_stream   *z = &cStream->z;
    size_t      toCompress, nCompressed, compressLen;
    ptrdiff_t   offset;
    int         flushFlags;

    NS_NONNULL_ASSERT(cStream != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (z->zalloc == NULL) {
	(void) Ns_CompressInit(cStream);
    }

    offset = (ptrdiff_t) Ns_DStringLength(dsPtr);
    toCompress = (nbufs > 0) ? Ns_SumVec(bufs, nbufs) : 0u;
    compressLen = compressBound(toCompress) + 12;

    if (!(cStream->flags & COMPRESS_SENT_HEADER)) {
        cStream->flags |= COMPRESS_SENT_HEADER;
        compressLen += 10; /* Gzip header length. */
        (void) deflateParams(z,
                             MIN(MAX(level, 1), 9),
                             Z_DEFAULT_STRATEGY);
    }
    if (flush) {
        compressLen += 4; /* Gzip footer. */
    }
    Ns_DStringSetLength(dsPtr, compressLen);

    z->next_out  = (Bytef *) (dsPtr->string + offset);
    z->avail_out = compressLen;

    /*
     * Compress all buffers.
     */

    nCompressed = 0;

    if (nbufs == 0) {
	flushFlags = flush ? Z_FINISH : Z_SYNC_FLUSH;
        DeflateOrAbort(z, flushFlags);
    } else {
	int i;
	for (i = 0; i < nbufs; i++) {

	    z->next_in  = (void *)bufs[i].iov_base;
	    z->avail_in = bufs[i].iov_len;
	    nCompressed += z->avail_in;;
	    
	    if (z->avail_in == 0 && i < nbufs -1) {
		continue;
	    }
	    if (nCompressed == toCompress) {
		flushFlags = flush ? Z_FINISH : Z_SYNC_FLUSH;
	    } else {
		flushFlags = Z_NO_FLUSH;
	    }
	    
	    DeflateOrAbort(z, flushFlags);
	}
    }
    Ns_DStringSetLength(dsPtr, dsPtr->length - z->avail_out);

    if (flush) {
        (void) deflateReset(z);
        cStream->flags = 0u;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CompressGzip --
 *
 *      Compress a buffer with RFC 1952 gzip header/footer.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      Aborts on error (which should not happen).
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_CompressGzip(const char *buf, int len, Ns_DString *dsPtr, int level)
{
    Ns_CompressStream  cStream;
    struct iovec       iov;
    Ns_ReturnCode      status;

    NS_NONNULL_ASSERT(buf != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    status = Ns_CompressInit(&cStream);
    if (status == NS_OK) {
      Ns_SetVec(&iov, 0, buf, len);
      status = Ns_CompressBufsGzip(&cStream, &iov, 1, dsPtr, level, NS_TRUE);
      Ns_CompressFree(&cStream);
    }

    return status;
}



/*
 *----------------------------------------------------------------------
 *
 * DeflateOrAbort --
 *
 *      Call deflate and abort on error.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DeflateOrAbort(z_stream *z, int flushFlags)
{
    int status;

    status = deflate(z, flushFlags);

    if ((status != Z_OK && status != Z_STREAM_END)
        || z->avail_in != 0
        || z->avail_out == 0) {

        Ns_Fatal("Ns_CompressBufsGzip: zlib error: %d (%s): %s:"
                 " avail_in: %d, avail_out: %d",
                 status, zError(status), (z->msg != NULL) ? z->msg : "(unknown)",
                 z->avail_in, z->avail_out);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ZAlloc, ZFree --
 *
 *      Memory callbacks for the zlib library.
 *
 * Results:
 *      Memory/None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static voidpf
ZAlloc(voidpf arg, uInt items, uInt size)
{
    return ns_calloc(items, size);
}

static void
ZFree(voidpf arg, voidpf address)
{
    ns_free(address);
}

#else /* ! HAVE_ZLIB_H */

Ns_ReturnCode
Ns_CompressInit(Ns_CompressStream *UNUSED(cStream))
{
    return NS_ERROR;
}

void
Ns_CompressFree(Ns_CompressStream *UNUSED(cStream))
{
    return;
}

Ns_ReturnCode
Ns_CompressBufsGzip(Ns_CompressStream *UNUSED(cStream), struct iovec *UNUSED(bufs), int UNUSED(nbufs),
                    Ns_DString *UNUSED(dsPtr), int UNUSED(level), bool UNUSED(flush))
{
    return NS_ERROR;
}

Ns_ReturnCode
Ns_CompressGzip(const char *UNUSED(buf), int UNUSED(len), Tcl_DString *UNUSED(outPtr), int UNUSED(level))
{
    return NS_ERROR;
}

Ns_ReturnCode 
Ns_InflateInit(Ns_CompressStream *UNUSED(cStream)) 
{
    return NS_ERROR;
}

Ns_ReturnCodex
Ns_InflateBufferInit(Ns_CompressStream *UNUSED(cStream), const char *UNUSED(buffer), size_t UNUSED(inSize)) 
{
    return NS_ERROR;
}
int
Ns_InflateBuffer(Ns_CompressStream *UNUSED(cStream), const char *UNUSED(buffer),
		 size_t UNUSED(outSize), size_t *UNUSED(nrBytes)) 
{
    return TCL_ERROR;
}

int
Ns_InflateEnd(Ns_CompressStream *UNUSED(cStream)) 
{
    return NS_ERROR;
}

#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
