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
 *      Parse HTTP range requests.
 */

#include "nsd.h"

typedef struct Range {
    off_t   start;
    off_t   end;
} Range;


/*
 * Local functions defined in this file
 */
static int ParseRangeOffsets(Ns_Conn *conn, size_t objLength,
                             Range *ranges, int maxRanges)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static void SetRangeHeader(const Ns_Conn *conn, off_t start, off_t end, size_t objLength)
    NS_GNUC_NONNULL(1);
    
static void SetMultipartRangeHeader(const Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

static int AppendMultipartRangeHeader(Ns_DString *dsPtr, const char *type,
                                      off_t start, off_t end, size_t objLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(1);

static int AppendMultipartRangeTrailer(Ns_DString *dsPtr)
        NS_GNUC_NONNULL(1);

static bool MatchRange(const Ns_Conn *conn, time_t mtime)
        NS_GNUC_NONNULL(1);


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
 *      Only HTTP date is supportd in the If-Range: header
 *
 *----------------------------------------------------------------------
 */

static bool
MatchRange(const Ns_Conn *conn, time_t mtime)
{
    bool result = NS_TRUE;

    NS_NONNULL_ASSERT(conn != NULL);

    /*
     * From RFC 2068: If the client has no entity tag for an entity,
     * but does have a Last-Modified date, it may use that date in a
     * If-Range header. (The server can distinguish between a valid
     * HTTP-date and any form of entity-tag by examining no more than
     * two characters.)
     */

    if (Ns_SetIGet(conn->headers, "Range") != NULL) {
        char *hdr = Ns_SetIGet(conn->headers, "If-Range");
        if (hdr != NULL && mtime > Ns_ParseHttpTime(hdr)) {
            result = NS_FALSE;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsConnParseRange --
 *
 *      Checks for presence of "Range:" header, parses it and fills-in
 *      bufs with byte-range headers and file/data offsets, as needed.
 *
 * Results:
 *      -1 on error, otherwise number of valid ranges parsed.
 *      If at least 1 range is found, nbufsPtr is updated with number
 *      of FileVec bufs to be sent.
 *
 * Side effects:
 *      The number of possible ranges parsed depends on the number
 *      of Ns_FileVec bufs passed (bufs/2 -1).
 *      May send error response if invalid range-spec..
 *
 *----------------------------------------------------------------------
 */

int
NsConnParseRange(Ns_Conn *conn, const char *type,
                 int fd, const void *data, size_t objLength,
                 Ns_FileVec *bufs, int *nbufsPtr, Ns_DString *dsPtr)
{
    const Conn *connPtr = (const Conn *) conn;
    int         rangeCount = 0;
    off_t       start, end;
    size_t      len, responseLength;
#ifdef NS_HAVE_C99
    Range       ranges[NS_MAX_RANGES];
#else
    Range      *ranges;
    
    ranges = alloca(sizeof(Range) * (size_t)NS_MAX_RANGES);
#endif    

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(type != NULL);
    NS_NONNULL_ASSERT(nbufsPtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    Ns_ConnCondSetHeaders(conn, "Accept-Ranges", "bytes");

    if (MatchRange(conn, connPtr->fileInfo.st_mtime)) {
        int maxranges = NS_MAX_RANGES;
        
        rangeCount = ParseRangeOffsets(conn, objLength, ranges, maxranges);
    }
    
    if (rangeCount < 1) {
        
        /*
         * There are no ranges.
         */
        *nbufsPtr = 0;
        
    } else if (rangeCount == 1) {
        
        /*
         * There is a single range.
         */
        Ns_ConnSetResponseStatus(conn, 206);
        
        start = ranges[0].start;
        end   = ranges[0].end;
        len   = (size_t)((end - start) + 1);

        responseLength = Ns_SetFileVec(bufs, 0, fd, data, start, len);
        *nbufsPtr = 1;

        SetRangeHeader(conn, start, end, objLength);
        Ns_ConnSetLengthHeader(conn, responseLength, NS_FALSE);

    } else {
        off_t    dsbase;
        int      i, v;
        
        /*
         * We have multiple ranges; Construct the MIME headers for a
         * multipart range against a 0 base and rebase after we've
         * finished resizing the string.
         */
        
        Ns_ConnSetResponseStatus(conn, 206);
        dsbase = 0;
        len = 0u;

        for (i = 0, v = 0; i < rangeCount; i++, v += 2) {

            start = ranges[i].start;
            end   = ranges[i].end;

            len += (size_t)AppendMultipartRangeHeader(dsPtr, type, start, end, objLength);
            dsbase += (off_t)Ns_SetFileVec(bufs, v, -1, NULL, dsbase, len);

            /* Combine the footer with the next header. */
            Ns_DStringAppend(dsPtr, "\r\n");
            len = 2u;
        }
        len += (size_t)AppendMultipartRangeTrailer(dsPtr);
        (void) Ns_SetFileVec(bufs, v, -1, NULL, dsbase, len);

        /*
         * Rebase the header, add the data range, and finish off with
         * the rebased trailer.
         */

        responseLength = 0u;

        for (i = 0, v = 0; i < rangeCount; i++, v += 2) {

            /* Rebase the header. */
            responseLength += Ns_SetFileVec(bufs, v, -1, dsPtr->string,
                                            bufs[v].offset, bufs[v].length);

            start = ranges[i].start;
            len   = (size_t)((ranges[i].end - start) + 1);

            responseLength += Ns_SetFileVec(bufs, v + 1, fd, data, start, len);
        }

        /* Rebase the trailer. */
        responseLength += Ns_SetFileVec(bufs, v, -1, dsPtr->string,
                                        bufs[v].offset, bufs[v].length);
        *nbufsPtr = (rangeCount * 2) + 1;

        SetMultipartRangeHeader(conn);
        Ns_ConnSetLengthHeader(conn, responseLength, NS_FALSE);
    }
    return rangeCount;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseRangeOffsets --
 *
 *      Checks for presence of "Range:" header, parses it and fills-in
 *      the parsed range offsets.
 *
 * Results:
 *      -1 on error, otherwise number of valid ranges parsed.
 *
 * Side effects:
 *      May send error response if invalid range-spec.
 *
 *----------------------------------------------------------------------
 */

static int
ParseRangeOffsets(Ns_Conn *conn, size_t objLength,
                  Range *ranges, int maxRanges)
{
    char   *rangestr;
    off_t   start, end;
    Range  *thisPtr = NULL, *prevPtr = NULL;
    int     rangeCount = 0;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(ranges != NULL);
    
    /*
     * Check for valid "Range:" header
     */

    rangestr = Ns_SetIGet(conn->headers, "Range");
    if (rangestr == NULL) {
        return 0;
    }

    /*
     * Parse the header value and fill-in ranges.
     * See RFC 2616 "14.35.1 Byte Ranges" for the syntax.
     */

    rangestr = strstr(rangestr, "bytes=");
    if (rangestr == NULL) {
        return 0;
    }
    rangestr += 6; /* Skip "bytes=" */

    while (*rangestr != '\0' && rangeCount < maxRanges) {

        thisPtr = &ranges[rangeCount];
        if (CHARTYPE(digit, *rangestr) != 0) {

            /*
             * Parse: first-byte-pos "-" last-byte-pos
             */

	    start = (off_t)strtoll(rangestr, &rangestr, 10);
            if (*rangestr != '-') {
                return 0; /* Invalid syntax? */
            }
            rangestr++; /* Skip '-' */

            if (CHARTYPE(digit, *rangestr) != 0) {
	        end = (off_t)strtoll(rangestr, &rangestr, 10);
                if (end >= (off_t)objLength) {
		  end = (off_t)objLength - 1;
                }
            } else {
	      end = (off_t)objLength - 1;
            }

        } else if (*rangestr == '-') {

            /*
             * Parse: "-" suffix-length
             */

            rangestr++; /* Skip '-' */
            if (CHARTYPE(digit, *rangestr) == 0) {
                return 0; /* Invalid syntax? */
            }

            end = (off_t)strtoll(rangestr, &rangestr, 10);
            if (end >= (off_t)objLength) {
	      end = (off_t)objLength;
            }

            /*
             * Size from the end; convert into offset.
             */

            start = ((off_t)objLength - end);
            end = start + end - 1;

        } else {

            /*
             * Not a digit and not a '-': invalid syntax.
             */

            return 0;
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
            return 0; /* Invalid syntax? */
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
         * "if first-byte-pos of all of the byte-range-spec values
         *  were greater than the current length of the selected
         *  resource"
         *
         * This is not clear: "all of the..." means *each-and-every*
         * first-byte-pos MUST be greater than the resource length.
         *
         * We opt to implement "any of the..." rather ...
         */

        if (start >= (off_t)objLength) {
            Ns_ConnPrintfHeaders(conn, "Content-Range",
                                 "bytes */%" PRIuMAX, (uintmax_t) objLength);
            (void)Ns_ConnReturnStatus(conn, 416);
            rangeCount = -1;
            break;
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
            rangeCount = 0;
            break;
        }

        /*
         * Check this range overlapping with the former.
         * The standard does not cleary specify how to
         * check those. Therefore, here is what we do:
         *
         *  a. for non-overlapping ranges: keep both
         *  b. for overlapping ranges: collapse into one
         */

        if ((prevPtr == NULL)
            || (thisPtr->start > (prevPtr->end + 1))
            || (prevPtr->start != 0 && thisPtr->end < (prevPtr->start - 1))) {
            /* a. */
            prevPtr = thisPtr;
            rangeCount++; /* One more valid range */
        } else {
            /* b. */
            prevPtr->start = MIN(prevPtr->start, thisPtr->start);
            prevPtr->end   = MAX(prevPtr->end,   thisPtr->end);
        }
    }

    return rangeCount;
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
SetRangeHeader(const Ns_Conn *conn, off_t start, off_t end, size_t objLength)
{
    NS_NONNULL_ASSERT(conn != NULL);
    
    Ns_ConnPrintfHeaders(conn, "Content-range",
        "bytes %" PRIuMAX "-%" PRIuMAX "/%" PRIuMAX,
        (uintmax_t) start, (uintmax_t) end, (uintmax_t) objLength);
}

static void
SetMultipartRangeHeader(const Ns_Conn *conn)
{
    NS_NONNULL_ASSERT(conn != NULL);
        
    Ns_ConnSetTypeHeader(conn,
        "multipart/byteranges; boundary=NaviServerNaviServerNaviServer");
}


/*
 *----------------------------------------------------------------------
 *
 * AppendMultipartRangerHeader, AppendMultipartRangeTraler --
 *
 *      Append a MIME header/trailer for multipart ranges to the dstring.
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
AppendMultipartRangeHeader(Ns_DString *dsPtr, const char *type,
                           off_t start, off_t end, size_t objLength)
{
    int origlen;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(type != NULL);

    origlen = dsPtr->length;
    
    Ns_DStringPrintf(dsPtr, "--NaviServerNaviServerNaviServer\r\n"
        "Content-type: %s\r\n"
        "Content-range: bytes %" PRIuMAX "-%" PRIuMAX "/%" PRIuMAX "\r\n\r\n",
        type,
        (uintmax_t) start, (uintmax_t) end, (uintmax_t) objLength);

    return dsPtr->length - origlen;
}

static int
AppendMultipartRangeTrailer(Ns_DString *dsPtr)
{
    int origlen;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    origlen = dsPtr->length;
    Ns_DStringAppend(dsPtr, "--NaviServerNaviServerNaviServer--\r\n");

    return dsPtr->length - origlen;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
