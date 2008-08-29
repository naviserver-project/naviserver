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
 * range.c --
 *
 *      Parse and send HTTP range requests.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


/*
 * An arbitrary limit to the number of ranges we're willing to serve
 * in one request.
 */

#define MAX_RANGES 32


/*
 * The following structure records the validated offsets requested by
 * an HTTP user-agent.
 */

typedef struct Range {

    struct offset {
        off_t   start;    /* Offset of first byte, zero based. */
        off_t   end;      /* Offset of last byte, zero based. */
    } ranges[MAX_RANGES];

    int count;            /* Number of ranges to send. */

} Range;


/*
 * Local functions defined in this file
 */

static int ParseRange(Ns_Conn *conn, Range *range, size_t length);
static int MatchRange(Ns_Conn *conn, time_t mtime);
static time_t LastModified(Ns_Conn *conn);

static void SetRangeHeader(Ns_Conn *conn, off_t offset, off_t end, size_t len);
static void SetMultipartRangeHeader(Ns_Conn *conn);
static int AppendMultipartRangeHeader(Ns_DString *ds, CONST char *type,
                                      off_t start, off_t end, size_t len);
static int AppendMultipartRangeTrailer(Ns_DString *ds);

static void SetVec(struct iovec *buf, int i, void *data, size_t len);


/*
 *----------------------------------------------------------------------
 *
 * NsConnWriteFdRanges --
 *
 *    Write the contents of the open file descriptor to the connection.
 *    If an HTTP range header is present, send only the corresponding
 *    portions.
 *
 * Results:
 *    NS_OK if all data written, NS_ERROR otherwise.
 *
 * Side effects:
 *    Will write an appropriate error response to the client.
 *
 *----------------------------------------------------------------------
 */

int
NsConnWriteFdRanges(Ns_Conn *conn, CONST char *type, int fd, size_t length)
{
    Conn        *connPtr = (Conn *) conn;
    Range        range;
    off_t        start, end;
    size_t       count;
    Ns_DString   ds;
    int          i, result;

    if (ParseRange(conn, &range, length) != NS_OK) {
        return NS_ERROR;
    }

    if (range.count == 0) {
        Ns_ConnSetLengthHeader(conn, length);
        return Ns_ConnSendFd(conn, fd, length);
    }

    if (range.count == 1) {

        start = range.ranges[0].start;
        end   = range.ranges[0].end;
        count = end - start + 1;

        /* NB: silently ignore seek errors and return whole file. */
        if (lseek(fd, start, SEEK_SET) == -1) {
            Ns_ConnSetLengthHeader(conn, length);
            return Ns_ConnSendFd(conn, fd, length);
        }

        SetRangeHeader(conn, start, end, length);
        Ns_ConnSetLengthHeader(conn, count);
        Ns_ConnSetResponseStatus(conn, 206);

        return Ns_ConnSendFd(conn, fd, count);
    }

    SetMultipartRangeHeader(conn);
    Ns_ConnSetResponseStatus(conn, 206);

    Ns_DStringInit(&ds);

    for (i = 0; i < range.count; i++) {

        start = range.ranges[i].start;
        end   = range.ranges[i].end;
        count = end - start + 1;

        AppendMultipartRangeHeader(&ds, type, start, end, length);
        result = Ns_ConnWriteData(conn, ds.string, ds.length, NS_CONN_STREAM);
        if (result != NS_OK) {
            goto done;
        }

        Ns_DStringSetLength(&ds, 0);

        /* NB: seek errors are fatal. no keep-alive. */
        if (lseek(fd, start, SEEK_SET) == -1) {
            result = NS_ERROR;
            goto done;
        }

        result = Ns_ConnSendFd(conn, fd, count);
        if (result != NS_OK) {
            goto done;
        }

        Ns_DStringAppend(&ds, "\r\n");
    }

    AppendMultipartRangeTrailer(&ds);
    result = Ns_ConnWriteData(conn, ds.string, ds.length, 0);

 done:
    Ns_DStringFree(&ds);

    if (result != NS_OK) {
        connPtr->keep = 0;
        Ns_ConnClose(conn);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsConnWriteDataRanges --
 *
 *    Write the given data to the connection. If an HTTP range header
 *    is present, send only the corresponding portions.
 *
 * Results:
 *    NS_OK if all data written, NS_ERROR otherwise.
 *
 * Side effects:
 *    Will write an appropriate error response to the client.
 *
 *----------------------------------------------------------------------
 */

int
NsConnWriteDataRanges(Ns_Conn *conn, CONST char *type,
                      CONST void *data, size_t length)
{
    Range         range;
    struct iovec  iov[MAX_RANGES * 2 + 1];
    off_t         start, end;
    size_t        count;
    Ns_DString    ds;
    int           i, v, dsbase, hdrlen;

    if (ParseRange(conn, &range, length) != NS_OK) {
        return NS_ERROR;
    }

    if (range.count == 0) {
        return Ns_ConnWriteData(conn, data, length, 0);
    }

    Ns_ConnSetResponseStatus(conn, 206);

    if (range.count == 1) {

        start = range.ranges[0].start;
        end   = range.ranges[0].end;
        count = end - start + 1;

        SetRangeHeader(conn, start, end, length);
        Ns_ConnSetLengthHeader(conn, count);

        return Ns_ConnWriteData(conn, data + start, count, 0);
    }

    Ns_DStringInit(&ds);

    /*
     * Construct the MIME headers against a 0 base and rebase after we've
     * finnished resizing the string.
     */

    dsbase = hdrlen = 0;

    for (i = 0, v = 0; i < range.count; i++, v += 2) {

        start = range.ranges[i].start;
        end   = range.ranges[i].end;

        hdrlen += AppendMultipartRangeHeader(&ds, type, start, end, length);
        SetVec(iov, v, (void *) dsbase, hdrlen);
        dsbase += hdrlen;

        Ns_DStringAppend(&ds, "\r\n");
        hdrlen = 2;
    }
    hdrlen += AppendMultipartRangeTrailer(&ds);
    SetVec(iov, v, (void *) dsbase, hdrlen);

    /*
     * Rebase the header, add the data range, and finnish off with
     * a trailer.
     */

    for (i = 0, v = 0; i < range.count; i++, v += 2) {

        start = range.ranges[i].start;
        end   = range.ranges[i].end;
        count = end - start + 1;

        SetVec(iov, v, ds.string + (ptrdiff_t) iov[v].iov_base, iov[v].iov_len);
        SetVec(iov, v + 1, (void *) data + start, count);
    }
    SetVec(iov, v, ds.string + (ptrdiff_t) iov[v].iov_base, iov[v].iov_len);

    SetMultipartRangeHeader(conn);
    return Ns_ConnWriteVData(conn, iov, range.count * 2 + 1, 0);
}

static void
SetVec(struct iovec *iov, int i, void *data, size_t len)
{
    iov[i].iov_base = data;
    iov[i].iov_len = len;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseRange --
 *
 *      Checks for presence of "Range:" header, parses it and fills-in
 *      the parsed range offsets.
 *
 * Results:
 *      NS_ERROR: byte-range is syntactically correct but unsatisfiable
 *      NS_OK: parsed ok; rnPtr->count has the number of ranges parsed
 *
 * Side effects:
 *      All byte-range-sets beyond MAX_RANGES will be ignored
 *
 *----------------------------------------------------------------------
 */

static int
ParseRange(Ns_Conn *conn, Range *range, size_t length)
{
    char          *rangestr;
    off_t          start, end;
    time_t         mtime;
    struct offset *thisPtr = NULL, *prevPtr = NULL;

    range->count = 0;
    Ns_ConnCondSetHeaders(conn, "Accept-Ranges", "bytes");

    /*
     * Check for valid "Range:" header
     */

    rangestr = Ns_SetIGet(conn->headers, "Range");
    if (rangestr == NULL) {
        return NS_OK;
    }

    /*
     * Parse the header value and fill-in ranges.
     * See RFC 2616 "14.35.1 Byte Ranges" for the syntax.
     */

    rangestr = strstr(rangestr, "bytes=");
    if (rangestr == NULL) {
        return NS_OK;
    }
    rangestr += 6; /* Skip "bytes=" */

    range->count = 0;

    while (*rangestr && range->count < sizeof(range->ranges) - 1) {

        thisPtr = &range->ranges[range->count];
        start = end = 0;

        if (isdigit(UCHAR(*rangestr))) {

            /*
             * Parse: first-byte-pos "-" last-byte-pos
             */

            start = atoll(rangestr);
            while (isdigit(UCHAR(*rangestr))) {
                rangestr++;
            }

            if (*rangestr != '-') {
                return NS_OK; /* Invalid syntax? */
            }
            rangestr++; /* Skip '-' */

            if (isdigit(UCHAR(*rangestr))) {
                end = atoll(rangestr);
                while (isdigit(UCHAR(*rangestr))) {
                    rangestr++;
                }
                if (end >= length) {
                    end = length - 1;
                }
            } else {
                end = length - 1;
            }

        } else if (*rangestr == '-') {

            /*
             * Parse: "-" suffix-length
             */

            rangestr++; /* Skip '-' */
            if (!isdigit(UCHAR(*rangestr))) {
                return NS_OK; /* Invalid syntax? */
            }

            end = atoll(rangestr);
            while (isdigit(UCHAR(*rangestr))) {
                rangestr++;
            }

            if (end >= length) {
                end = length;
            }

            /*
             * Size from the end; convert into count
             */

            start = length - end;
            end = start + end - 1;

        } else {

            /*
             * Invalid syntax?
             */

            return NS_OK;
        }

        /*
         * Check end of range_spec
         */

        switch (*rangestr) {
        case ',':
            rangestr++;
            break;
        case '\0':
            break;
        default:
            return NS_OK; /* Invalid syntax? */
        }

        /*
         * We are now done with the syntax of the range so go check
         * the semantics of the values...
         */

        thisPtr->start = start;
        thisPtr->end = end;

        /*
         * RFC 2616: 416 "Requested Range Not Satisfiable"
         *
         * "if first-byte-pos of all of the byte-range-spec values were
         *  greater than the current length of the selected resource"
         *
         * This is not clear: "all of the..." means *each-and-every*
         * first-byte-pos MUST be greater than the resource length.
         *
         * We opt to implement "any of the..." rather ...
         */

        if (start >= length) {
            Ns_ConnPrintfHeaders(conn, "Content-Range",
                                 "bytes */%" PRIuMAX, (intmax_t) length);
            Ns_ConnReturnStatus(conn, 416);
            return NS_ERROR;
        }

        /*
         * RFC 2616: 14.35.1 Byte Ranges
         *
         *  "If the last-byte-pos value is present, it MUST be greater
         *   than or equal to the first-byte-pos in that byte-range-spec,
         *   or the byte-range-spec is syntactically invalid."
         *
         */

        if (end < start) {
            return NS_OK;
        }

        /*
         * Check this range overlapping with the former.
         * The standard does not cleary specify how to
         * check those. Therefore, here is what we do:
         *
         *  a. for non-overlapping ranges: keep both
         *  b. for overlapping ranges: collapse into one
         */

        if (prevPtr == NULL
            || (thisPtr->start > (prevPtr->end + 1))
            || (prevPtr->start && thisPtr->end < (prevPtr->start - 1))) {
            /* a. */
            prevPtr = thisPtr;
            range->count++; /* One more valid range */
        } else {
            /* b. */
            prevPtr->start = MIN(prevPtr->start, thisPtr->start);
            prevPtr->end   = MAX(prevPtr->end,   thisPtr->end);
        }
    }

    /*
     * If the data has changed, send the whole content (no ranges).
     */

    mtime = LastModified(conn);
    if (mtime && !MatchRange(conn, mtime)) {
        range->count = 0;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * MatchRange --
 *
 *      Check an If-Range header against the data's mtime.
 *
 * Results:
 *      NS_TRUE if partial content may be returned, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
MatchRange(Ns_Conn *conn, time_t mtime)
{
    char *hdr;

    if ((hdr = Ns_SetIGet(conn->headers, "If-Range")) != NULL
            && Ns_ParseHttpTime(hdr) != mtime) {
        return NS_FALSE;
    }
    return NS_TRUE;
}

static time_t
LastModified(Ns_Conn *conn)
{
    char *hdr;

    if ((hdr = Ns_SetIGet(conn->outputheaders, "Last-Modified")) != NULL) {
        return Ns_ParseHttpTime(hdr);
    }
    return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * SetRangeHeader, SetMultipartRangeHeader --
 *
 *      Set the HTTP header for single or multipart range requests.
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
SetRangeHeader(Ns_Conn *conn, off_t start, off_t end, size_t len)
{
    Ns_ConnPrintfHeaders(conn, "Content-range",
        "bytes %" PRIdMAX "-%" PRIdMAX "/%" PRIdMAX,
        (intmax_t) start, (intmax_t) end, (intmax_t) len);
}

static void
SetMultipartRangeHeader(Ns_Conn *conn)
{
    Ns_ConnSetTypeHeader(conn,
        "multipart/byteranges; boundary=NaviServerNaviServerNaviServer");
}


/*
 *----------------------------------------------------------------------
 *
 * AppendMultipartRangerHeader, AppendMultipartRangeTraler --
 *
 *      Append a MIME header for multipart ranges to the dstring.
 *
 * Results:
 *      Number of bytes appended.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
AppendMultipartRangeHeader(Ns_DString *ds, CONST char *type,
                           off_t start, off_t end, size_t total)
{
    int origlen = ds->length;

    Ns_DStringPrintf(ds, "--NaviServerNaviServerNaviServer\r\n"
        "Content-type: %s\r\n"
        "Content-range: bytes %" PRIuMAX "-%" PRIuMAX "/%" PRIuMAX "\r\n\r\n",
        type,
        (uintmax_t) start, (uintmax_t) end, (uintmax_t) total);

    return ds->length - origlen;
}

static int
AppendMultipartRangeTrailer(Ns_DString *ds)
{
    int origlen = ds->length;

    Ns_DStringAppend(ds, "--NaviServerNaviServerNaviServer--\r\n");
    return ds->length - origlen;
}
