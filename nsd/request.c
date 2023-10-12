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
 * request.c --
 *
 *      Functions that implement the Ns_Request type.
 *
 */

#include "nsd.h"

#define HTTP "HTTP/"

/*
 * Local functions defined in this file.
 */

static void SetUrl(Ns_Request *request, char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void FreeUrl(Ns_Request *request)
    NS_GNUC_NONNULL(1);

static const char *GetQvalue(const char *str, int *lenPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static const char *GetEncodingFormat(const char *encodingString,
                                     const char *encodingFormat, double *qValue)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void RequestCleanupMembers(Ns_Request *request)
    NS_GNUC_NONNULL(1);

/*
 *----------------------------------------------------------------------
 *
 * RequestCleanupMembers --
 *
 *    Frees the members of the provided Ns_Request structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Freeing memory.
 *
 *----------------------------------------------------------------------
 */
static void
RequestCleanupMembers(Ns_Request *request)
{
    NS_NONNULL_ASSERT(request != NULL);

    if (request->line != NULL) {
        Ns_Log(Ns_LogRequestDebug, "end %s", request->line);
    }
    ns_free((char *)request->line);
    ns_free((char *)request->method);
    ns_free((char *)request->protocol);
    ns_free((char *)request->host);
    ns_free(request->query);
    ns_free((char *)request->serverRoot);
    FreeUrl(request);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ResetRequest --
 *
 *    Free the Ns_Request members. This function is usually called on
 *    embedded Ns_Request structures, such as these part of the Request
 *    structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ResetRequest(Ns_Request *request)
{
    NS_NONNULL_ASSERT(request != NULL);

    /*
     * There is no need to free the full structure, just clean the members and
     * reset it to NULL.
     */
    RequestCleanupMembers(request);
    memset(request, 0, sizeof(Ns_Request));
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_FreeRequest --
 *
 *    Free an Ns_Request structure and all its members.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_FreeRequest(Ns_Request *request)
{
    if (request != NULL) {
        RequestCleanupMembers(request);
        ns_free(request);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseRequest --
 *
 *    Parse a request from the client into an Ns_Request structure.
 *    On success, it fills the following Ns_Request members:
 *      - line
 *      - method
 *      - version
 *      - protocol
 *      - host
 *      - port
 *
 * Results:
 *    NS_OK on success, NS_ERROR on error
 *
 * Side effects:
 *    The request if not NULL is always zero-ed before filled with values
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ParseRequest(Ns_Request *request, const char *line, size_t len)
{
    char       *url, *l, *p;
    Ns_DString  ds;
    const char *errorMsg = "unknown error";

    NS_NONNULL_ASSERT(line != NULL);

    if (request == NULL) {
        return NS_ERROR;
    }

    /*
     * Check, if the request looks like a TLS handshake. If yes, there is no
     * need to try to parse the received buffer. There is no need to complain
     * about binary content in this case.
     */
    if (line[0] == (char)0x16 && line[1] >= 3 && line[2] == 1) {
        return NS_ERROR;
    }

    /*
     * We could check here the validity UTF-8 of the request line, in case we
     * would know it is supposed to be UTF8. Unfortunately, this is known
     * ownly after the server is determined. We could use the ns/param
     * encoding, but then, the per-server urlEncoding does not make sense.
     *
     * RFC 7230 (Hypertext Transfer Protocol (HTTP/1.1): Message Syntax and
     * Routing) states: Parsing an HTTP message as a stream of Unicode
     * characters, without regard for the specific encoding, creates security
     * vulnerabilities due to the varying ways that string processing
     * libraries handle invalid multibyte character sequences that contain the
     * octet LF (%x0A).
     *
     * W3C recommends only URLs with proper encodings (subset of US ASCII):
     * https://www.w3.org/Addressing/URL/4_URI_Recommentations.html
     */
    if (!Ns_Is7bit(line, len)) {
        Ns_Log(Warning, "Ns_ParseRequest: line <%s> contains 8-bit "
               "character data. Future versions might reject it.", line);
    }

#if !defined(NDEBUG)
    /*
     * The passed-in line must not contain a newline
     */
    assert(strrchr(line, INTCHAR('\n')) == NULL);
#endif

    memset(request, 0, sizeof(Ns_Request));
    Ns_DStringInit(&ds);

    /*
     * Make a copy of the line to chop up. Make sure it isn't blank.
     */

    Ns_DStringNAppend(&ds, line, (TCL_SIZE_T)len);
    l = Ns_StrTrim(ds.string);
    if (*l == '\0') {
        errorMsg = "empty request line";
        goto error;
    }

    /*
     * Save the trimmed line for logging purposes.
     */
    request->line = ns_strdup(l);

    Ns_Log(Ns_LogRequestDebug, "begin %s", request->line);

    /*
     * Look for the minimum of method and URL.
     *
     * Collect non-space characters as first token.
     */

    url = l;
    while (*url != '\0' && CHARTYPE(space, *url) == 0) {
        ++url;
    }
    if (*url == '\0') {
        errorMsg = "no method found";
        goto error;
    }

    /*
     * Mark the end of the first token and remember it as HTTP-method.
     */
    *url++ = '\0';
    request->method = ns_strdup(l);

    /*
     * Skip spaces.
     */
    while (*url != '\0' && CHARTYPE(space, *url) != 0)  {
        ++url;
    }
    if (*url == '\0') {
        errorMsg = "no version information found";
        goto error;
    }


    /*
     * Look for a valid version. Typically, the HTTP-version number is of the
     * form "HTTP/1.0". However, in HTTP 0.9, the HTTP-version number was not
     * specified.
     */
    request->version = 0.0;

    /*
     * Search from the end for the last space.
     */
    p = strrchr(url, INTCHAR(' '));
    if (likely(p != NULL)) {
        /*
         * We have a final token. Let see, if this an HTTP-version string.
         */
        if (likely(strncmp(p + 1, HTTP, sizeof(HTTP) - 1u) == 0)) {
            /*
             * The HTTP-Version string starts really with HTTP/
             *
             * If strtod fails, version will be set to 0 and the server will
             * treat the connection as if it had no HTTP/n.n keyword.
             */
            *p = '\0';
            p += sizeof(HTTP);
            request->version = strtod(p, NULL);
        } else {
            /*
             * The last token does not have the form of an HTTP-version
             * string. Report result as invalid request.
             */
            errorMsg = "version information invalid";
            goto error;
        }
    } else {
        /*
         * Let us assume, the request is HTTP 0.9, when the URL starts with a
         * slash. HTTP 0.9 did not have proxy functionality.
         */
        if (*url != '/') {
            errorMsg = "HTTP 0.9 URL does not start with a slash";
            goto error;
        }
    }

    url = Ns_StrTrimRight(url);
    if (*url == '\0') {
        errorMsg = "URL is empty";
        goto error;
    }

    /*
     * Look for a protocol in the URL.
     */
    request->protocol = NULL;
    request->host = NULL;
    request->port = 0u;

    if (*url != '/') {

        /*
         * Check for the scheme of the URL. The RFC 3986
         * defines the scheme as
         *
         *      ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
         *
         * but since we support just a subset of protocols, where all of these
         * contain just ALPHA, we restrict to these. This has the advantacge
         * that we can deal here with request lines for CONNECT, such as e.g.
         *
         *      CONNECT google.com:443 HTTP/1.1
         *
         * where "google.com" would be syntactically correct scheme. It sounds
         * more locally to provide "google.com" as "host" and the "443" as
         * port.
         *
         *      curl -v -X CONNECT http://localhost:8080 --request-target www.google.com:443
         *      curl -v -x http://localhost:8080  https://someotherhost:8088/index.tcl
         */
        p = url;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            ++p;
        }
        if (*p == ':') {

            /*
             * Found a scheme; this must be a proxy request. Copy the scheme
             * and search for host:port.
             */
            request->requestType = NS_REQUEST_TYPE_PROXY;
            *p++ = '\0';
            request->protocol = ns_strdup(url);

            if (*p == '/' && *(p+1) == '/') {
                p += 2;
            }
        } else {
            request->requestType = NS_REQUEST_TYPE_CONNECT;
            p = url;
        }
        /*
         * Parse host:port.
         */
        if (*p != '\0' && *p != '/') {
            bool  hostParsedOk;
            char *h = p, *end;

            /*
             * Search for the next slash
             */
            p = strchr(p, INTCHAR('/'));
            if (p != NULL) {
                //*p++ = '\0';
                url = p;
            } else {
                url = (char*)"";
            }

            /*
             * Parse actually host and port
             */
            hostParsedOk = Ns_HttpParseHost2(h, NS_FALSE, NULL, &p, &end);
            if (hostParsedOk) {
                //Ns_Log(Notice, "Parse host+port <%s> -> %d p <%s> end <%s>", h, hostParsedOk, p, end);
                if (p != NULL) {
                    /*
                     * We know, the port string is terminated by a slash or NUL.
                     */
                    request->port = (unsigned short)strtol(p, NULL, 10);
                }
                request->host = ns_strdup(h);
            }

            /*
             * Here, the request is either a proxy request, or a CONNECT
             * request (url == "") or something is wrong.
             */
            if (request->requestType == NS_REQUEST_TYPE_PROXY) {
                errorMsg = "invalid proxy request";
                if (url == NULL || *url == '\0') {
                    Ns_Log(Warning, "%s, path must not be empty"
                           " setting host '%s' port %hu protocol '%s' path '%s' from line '%s'",
                           errorMsg, request->host, request->port, request->protocol, url, line);
                    goto error;
                }
                if (request->protocol == NULL) {
                    Ns_Log(Warning, "%s, protocol must be specified"
                           " setting host '%s' port %hu protocol '%s' path '%s' from line '%s'",
                           errorMsg, request->host, request->port, request->protocol, url, line);
                    goto error;
                }

            } else if (request->requestType == NS_REQUEST_TYPE_CONNECT && *url != '\0') {
                errorMsg = "invalid CONNECT request";
                Ns_Log(Warning, "%s, path must be empty"
                       " setting host '%s' port %hu protocol '%s' path '%s' from line '%s'",
                       errorMsg, request->host, request->port, request->protocol, url, line);
                goto error;
            }

            Ns_Log(Ns_LogRequestDebug, "Ns_ParseRequest processes valid %s request"
                   " setting host '%s' port %hu protocol '%s' requestType '%d' path '%s' line '%s'",
                   request->requestType == NS_REQUEST_TYPE_PLAIN ? "plain"
                   : request->requestType == NS_REQUEST_TYPE_PROXY ? "proxy"
                   : "CONNECT",
                   request->host, request->port, request->protocol, request->requestType,
                   url,line);
        }
    }

    SetUrl(request, url);
    Ns_DStringFree(&ds);

    return NS_OK;

 error:
    Ns_Log(Warning, "Ns_ParseRequest <%s> cannot parse request line: %s", line, errorMsg);

    if (request->protocol != NULL) {
        ns_free((char*)request->protocol);
        request->protocol = NULL;
    }
    if (request->host != NULL) {
        ns_free((char*)request->host);
        request->host = NULL;
    }

    Ns_DStringFree(&ds);
    return NS_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SkipUrl --
 *
 *    Return a pointer n elements into the request's URL.
 *
 * Results:
 *    The URL beginning n elements in.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SkipUrl(const Ns_Request *request, int n)
{
    const char **elements, *result = NULL;
    TCL_SIZE_T   length;

    NS_NONNULL_ASSERT(request != NULL);

    Tcl_SplitList(NULL, request->urlv, &length, &elements);

    if (n <= (int)request->urlc) {
        size_t skip = 0u;

        while (--n >= 0) {
            skip += strlen(elements[n]) + 1u;
        }
        result = (request->url + skip);
    }
    Tcl_Free((char *)elements);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetRequestUrl --
 *
 *    Set the URL in a request structure.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Makes a copy of URL.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetRequestUrl(Ns_Request *request, const char *url)
{
    Ns_DString      ds;

    NS_NONNULL_ASSERT(request != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    FreeUrl(request);
    Ns_DStringInit(&ds);
    Ns_DStringAppend(&ds, url);
    SetUrl(request, ds.string);
    Ns_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * FreeUrl --
 *
 *    Free the URL in a request.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

static void
FreeUrl(Ns_Request *request)
{
    NS_NONNULL_ASSERT(request != NULL);

    if (request->url != NULL) {
        ns_free((char *)request->url);
        request->url = NULL;
    }
    if (request->urlv != NULL) {
        ns_free((char *)request->urlv);
        request->urlv = NULL;
        request->urlc = 0;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * SetUrl --
 *
 *    Break up a URL and put it in the request.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    Memory allocated for members.
 *
 *----------------------------------------------------------------------
 */

static void
SetUrl(Ns_Request *request, char *url)
{
    Tcl_DString  ds1, ds2;
    char        *p;
    const char  *encodedPath;
    Tcl_Encoding encoding;

    NS_NONNULL_ASSERT(request != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Tcl_DStringInit(&ds1);
    Tcl_DStringInit(&ds2);

    /*
     * Look for a query string at the end of the URL.
     */

    p = strchr(url, INTCHAR('?'));
    if (p != NULL) {
        *p++ = '\0';
        ns_free(request->query);
        if (*p != '\0') {
            request->query = ns_strdup(p);
        }
    }

    /*
     * Decode and normalize the URL (remove ".", "..").
     */
    encodedPath = url;
    encoding = Ns_GetUrlEncoding(NULL);
    Ns_Log(Debug, "### Request SetUrl calls Ns_UrlPathDecode '%s'", encodedPath);
    p = Ns_UrlPathDecode(&ds1, encodedPath, encoding);
    Ns_Log(Debug, " ### decoded path '%s'", p);

    if (p == NULL) {
        p = url;
    }
    (void)Ns_NormalizeUrl(&ds2, p);
    Tcl_DStringSetLength(&ds1, 0);

    /*
     * Append a trailing slash to the normalized URL if the original URL
     * ended in slash that wasn't also the leading slash.
     */

    while (*url == '/') {
        ++url;
    }
    if (*url != '\0' && url[strlen(url) - 1u] == '/') {
        Tcl_DStringAppend(&ds2, "/", 1);
    }
    request->url = ns_strdup(ds2.string);
    request->url_len = ds2.length;
    Tcl_DStringFree(&ds2);

    /*
     * Build the urlv and set urlc.
     */
    {
        Tcl_Obj *listPtr, *segmentObj;

        listPtr = Tcl_NewListObj(0, NULL);
        Tcl_IncrRefCount(listPtr);
        /*
         * Skip the leading slash.
         */
        encodedPath++;

        while (*encodedPath != '\0') {
            p = strchr(encodedPath, INTCHAR('/'));
            if (p == NULL) {
                break;
            }
            *p = '\0';
            Ns_UrlPathDecode(&ds1, encodedPath, encoding);
            segmentObj = Tcl_NewStringObj(ds1.string, ds1.length);
            Tcl_ListObjAppendElement(NULL, listPtr, segmentObj);
            Tcl_DStringSetLength(&ds1, 0);
            encodedPath = p + 1;
        }
        /*
         * Append last segment if not empty (for compatibility with previous
         * versions).
         */
        if (*encodedPath != '\0') {
            Ns_UrlPathDecode(&ds1, encodedPath, encoding);
            segmentObj = Tcl_NewStringObj(ds1.string, ds1.length);
            Tcl_ListObjAppendElement(NULL, listPtr, segmentObj);
        }

        /*
         * Set request->urlc and request->urlv based on the listPtr.
         */
        Tcl_ListObjLength(NULL, listPtr, &request->urlc);
        request->urlv = ns_strdup(Tcl_GetString(listPtr));
        request->urlv_len = (TCL_SIZE_T)strlen(request->urlv);

        Tcl_DecrRefCount(listPtr);
    }
    Tcl_DStringFree(&ds1);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseHeader --
 *
 *    Consume a header line, handling header continuation, placing
 *    results in given set.
 *
 * Results:
 *    NS_OK/NS_ERROR
 *
 * Side effects:
 *    None
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ParseHeader(Ns_Set *set, const char *line, const char *prefix, Ns_HeaderCaseDisposition disp,
               size_t *fieldNumberPtr)
{
    Ns_ReturnCode status = NS_OK;
    size_t        idx = 0u;

    /*
     * Header lines are first checked if they continue a previous
     * header indicated by any preceding white space.  Otherwise,
     * they must be in well form key: value form.
     */

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(line != NULL);

    if (CHARTYPE(space, *line) != 0) {
        if (Ns_SetSize(set) == 0u) {
            /*
             * Continue before first header.
             */
            status = NS_ERROR;

        } else {
            idx = Ns_SetLast(set);
            /*
             * Append to the last entry.
             */
            while (CHARTYPE(space, *line) != 0) {
                ++line;
            }
            if (*line != '\0') {
                Ns_DString ds;
                char      *value = Ns_SetValue(set, idx);

                Ns_DStringInit(&ds);
                Ns_DStringVarAppend(&ds, value, " ", line, (char *)0L);
                Ns_SetPutValueSz(set, idx, ds.string, ds.length);
                Ns_DStringFree(&ds);
            }
        }
    } else {
        char *sep;
        Tcl_DString ds, *dsPtr = &ds;

        if (prefix != NULL) {
            Tcl_DStringInit(dsPtr);
            Tcl_DStringAppend(dsPtr, prefix, TCL_INDEX_NONE);
            Tcl_DStringAppend(dsPtr, line, TCL_INDEX_NONE);
            line = dsPtr->string;
        }

        sep = strchr(line, INTCHAR(':'));
        if (sep == NULL) {
            /*
             * Malformed header.
             */
            status = NS_ERROR;

        } else {
            const char *value;
            char       *key;

            *sep = '\0';
            for (value = sep + 1; (*value != '\0') && CHARTYPE(space, *value) != 0; value++) {
                ;
            }
            idx = Ns_SetPutSz(set, line, (TCL_SIZE_T)(sep - line), value, TCL_INDEX_NONE);
            key = Ns_SetKey(set, idx);
            if (disp == ToLower) {
                while (*key != '\0') {
                    if (CHARTYPE(upper, *key) != 0) {
                        *key = CHARCONV(lower, *key);
                    }
                    ++key;
                }
            } else if (disp == ToUpper) {
                while (*key != '\0') {
                    if (CHARTYPE(lower, *key) != 0) {
                        *key = CHARCONV(upper, *key);
                    }
                    ++key;
                }
            }
            *sep = ':';
        }

        if (prefix != NULL) {
            Tcl_DStringFree(dsPtr);
        }
    }

    if (fieldNumberPtr != NULL && status == NS_OK) {
        *fieldNumberPtr = idx;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * GetQvalue --
 *
 *      Return the next qvalue string from accept encodings
 *
 * Results:
 *      string, setting lengthPtr; or NULL, if no or invalie
 *      qvalue provided
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static const char *
GetQvalue(const char *str, int *lenPtr) {
    const char *resultString = NULL;

    NS_NONNULL_ASSERT(str != NULL);
    NS_NONNULL_ASSERT(lenPtr != NULL);

    for (;;) {
        for (; *str == ' '; str++) {
            ;
        }
        if (*str != ';') {
            break;
        }
        for (str ++; *str == ' '; str++) {
            ;
        }
        if (*str != 'q') {
            break;
        }
        for (str ++; *str == ' '; str++) {
            ;
        }
        if (*str != '=') {
            break;
        }
        for (str ++; *str == ' '; str++) {
            ;
        }
        if (CHARTYPE(digit,*str) == 0) {
            break;
        }

        resultString = str;
        str++;
        if (*str == '.') {
            /*
             * Looks like a floating point number; RFC2612 allows up to
             * three digits after the comma.
             */
            str ++;
            if (CHARTYPE(digit, *str) != 0) {
                str++;
                if (CHARTYPE(digit, *str) != 0) {
                    str++;
                    if (CHARTYPE(digit, *str) != 0) {
                        str++;
                    }
                }
            }
        }
        /*
         * "str" should point to a valid terminator of the number.
         */
        if (*str == ' ' || *str == ',' || *str == ';' || *str == '\0') {
            *lenPtr = (int)(str - resultString);
        } else {
            resultString = NULL;
        }
        break;
    }
    return resultString;
}



/*
 *----------------------------------------------------------------------
 *
 * GetEncodingFormat --
 *
 *      Search on encodingString (header field accept-encodings) for
 *      encodingFormat (e.g. "gzip", "identy") and return its q value.
 *
 * Results:
 *      On success non-NULL value and the parsed qValue
 *      (when no qvalue is provided then assume qvalue as 1.0);
 *      On failure NULL value qValue set to -1;
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
GetEncodingFormat(const char *encodingString, const char *encodingFormat, double *qValue) {
    const char *encodingStr;

    NS_NONNULL_ASSERT(encodingString != NULL);
    NS_NONNULL_ASSERT(encodingFormat != NULL);
    NS_NONNULL_ASSERT(qValue != NULL);

    encodingStr = strstr(encodingString, encodingFormat);

    if (encodingStr != NULL) {
        int         len = 0;
        const char *qValueString = GetQvalue(encodingStr + strlen(encodingFormat), &len);

        if (qValueString != NULL) {
            *qValue = strtod(qValueString, NULL);
        } else {
            *qValue = 1.0;
        }

    } else {
        *qValue = -1.0;
    }
    return encodingStr;
}


/*
 *----------------------------------------------------------------------
 *
 * CompressAllow --
 *
 *      Handle quality values expressed explicitly (for gzip or brotli) in the
 *      header fields. Respect cases, where compression is forbidden via
 *      identy or default rules.
 *
 * Results:
 *      boolean
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
CompressAllow(double compressQvalue, double identityQvalue, double starQvalue)
{
    bool result;

    if (compressQvalue > 0.999) {
        /*
         * Compress qvalue 1, use it, nothing else can be higher, so it is
         * allowed.
         */
        result = NS_TRUE;
    } else if (compressQvalue < 0.0009) {
        /*
         * Compress qvalue 0, forbid this kind of compressions
         */
        result = NS_FALSE;
    } else {
        /*
         * A middle compress qvalue was specified, compare it with identity
         * and default.
         */
        if (identityQvalue >=- 1.0) {
            /*
             * The compression format is allowed, when the compression qvalue
             * is larger than identity.
             */
            result = (compressQvalue >= identityQvalue);
        } else if (starQvalue >= -1.0) {
            /*
             * gzip is used, when gzip qvalue is larger than default
             */
            result = (compressQvalue >= starQvalue);
        } else {
            /*
             * Accept the low qvalue due to lack of alternatives
             */
            result = NS_TRUE;
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsParseAcceptEncoding --
 *
 *      Parse the accept-encoding line and return whether gzip
 *      encoding is accepted or not.
 *
 * Results:
 *      The result is passed back in the last two arguments.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
NsParseAcceptEncoding(double version, const char *hdr, bool *gzipAcceptPtr, bool *brotliAcceptPtr)
{
    double      gzipQvalue = -1.0, brotliQvalue = -1, starQvalue = -1.0, identityQvalue = -1.0;
    bool        gzipAccept, brotliAccept;
    const char *gzipFormat, *brotliFormat, *starFormat;

    NS_NONNULL_ASSERT(hdr != NULL);
    NS_NONNULL_ASSERT(gzipAcceptPtr != NULL);
    NS_NONNULL_ASSERT(brotliAcceptPtr != NULL);

    gzipFormat    = GetEncodingFormat(hdr, "gzip", &gzipQvalue);
    brotliFormat  = GetEncodingFormat(hdr, "br", &brotliQvalue);
    starFormat    = GetEncodingFormat(hdr, "*", &starQvalue);
    (void)GetEncodingFormat(hdr, "identity", &identityQvalue);

    //fprintf(stderr, "hdr line <%s> gzipFormat <%s> brotliFormat <%s>\n", hdr, gzipFormat, brotliFormat);
    if ((gzipFormat != NULL) || (brotliFormat != NULL)) {
        gzipAccept   = CompressAllow(gzipQvalue, identityQvalue, starQvalue);
        brotliAccept = CompressAllow(brotliQvalue, identityQvalue, starQvalue);
    } else if (starFormat != NULL) {
        /*
         * No compress format was specified, star matches everything, so as
         * well the compression formats.
         */
        if (starQvalue < 0.0009) {
            /*
             * The low "*" qvalue forbids the compression formats.
             */
            gzipAccept = NS_FALSE;
        } else if (identityQvalue >= -1) {
            /*
             * Star qvalue allows gzip in HTTP/1.1, when it is larger
             * than identity.
             */
            gzipAccept = (starQvalue >= identityQvalue) && (version >= 1.1);
        } else {
            /*
             * No identity was specified, assume compression format is matched
             * with "*" in HTTP/1.1
             */
            gzipAccept = (version >= 1.1);
        }
        /*
         * The implicit rules are the same for gzip and brotli.
         */
        brotliAccept = gzipAccept;
    } else {
        gzipAccept   = NS_FALSE;
        brotliAccept = NS_FALSE;
    }
    *gzipAcceptPtr   = gzipAccept;
    *brotliAcceptPtr = brotliAccept;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
