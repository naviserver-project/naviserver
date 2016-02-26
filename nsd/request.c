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


/*
 *----------------------------------------------------------------------
 *
 * Ns_ResetRequest --
 *
 *	Free an Ns_Request members.
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_ResetRequest(Ns_Request *request)
{
    if (request != NULL) {
        ns_free((char *)request->line);
        ns_free((char *)request->method);
        ns_free((char *)request->protocol);
        ns_free((char *)request->host);
        ns_free(request->query);
        FreeUrl(request);
        memset(request, 0, sizeof(Ns_Request));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_FreeRequest --
 *
 *	Free an Ns_Request structure and all its members. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_FreeRequest(Ns_Request *request)
{
    if (request != NULL) {
        ns_free((char *)request->line);
        ns_free((char *)request->method);
        ns_free((char *)request->protocol);
        ns_free((char *)request->host);
        ns_free(request->query);
        FreeUrl(request);
        ns_free(request);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseRequest --
 *
 *	Parse a request from a browser into an Ns_Request structure. 
 *
 * Results:
 *	NS_OK on success, NS_ERROR on error
 *
 * Side effects:
 *	The request if not NULL is always zero-ed before filled with values
 *
 *----------------------------------------------------------------------
 */

int
Ns_ParseRequest(Ns_Request *request, const char *line)
{
    char       *url, *l, *p;
    Ns_DString  ds;

    NS_NONNULL_ASSERT(line != NULL);

    if (request == NULL) {
        return NS_ERROR;
    }

    memset(request, 0, sizeof(Ns_Request));
    Ns_DStringInit(&ds);

    /*
     * Make a copy of the line to chop up. Make sure it isn't blank.
     */
    
    Ns_DStringAppend(&ds, line);
    l = Ns_StrTrim(ds.string);
    if (*l == '\0') {
        goto done;
    }

    /*
     * Save the trimmed line for logging purposes.
     */
    
    request->line = ns_strdup(l);
    /*Ns_Log(Notice, "Ns_ParseRequest %p %s", request, request->line);*/

    /*
     * Look for the minimum of method and url.
     */
    
    url = l;
    while (*url != '\0' && CHARTYPE(space, *url) == 0) {
        ++url;
    }
    if (*url == '\0') {
        goto done;
    }
    *url++ = '\0';
    while (*url != '\0' && CHARTYPE(space, *url) != 0)  {
        ++url;
    }
    if (*url == '\0') {
        goto done;
    }
    request->method = ns_strdup(l);

    /*
     * Look for a valid version.
     */

    request->version = 0.0;
    p = url + strlen(url);
    while (p-- > url) {
        if (CHARTYPE(digit, *p) == 0 && *p != '.') {
            break;
        }
    }
    p -= (sizeof(HTTP) - 2u);
    if (p >= url) {
        if (strncmp(p, HTTP, sizeof(HTTP) - 1u) == 0) {

            /*
             * If atof fails, version will be set to 0 and the server
             * will treat the connection as if it had no HTTP/n.n keyword.
             */

            *p = '\0';
            p += sizeof(HTTP) - 1u;
            request->version = strtod(p, NULL);
        }
    }

    url = Ns_StrTrim(url);
    if (*url == '\0') {
        goto done;
    }

    /*
     * Look for a protocol in the URL.
     */

    request->protocol = NULL;
    request->host = NULL;
    request->port = 0u;

    if (*url != '/') {
        p = url;
        while (*p != '\0' && *p != '/' && *p != ':') {
            ++p;
        }
        if (*p == ':') {

            /*
             * Found a protocol - copy it and search for host:port.
             */

            *p++ = '\0';
            request->protocol = ns_strdup(url);
            url = p;
            if ((strlen(url) > 3u) && (*p++ == '/')
                && (*p++ == '/') && (*p != '\0') && (*p != '/')) {
                char *h = p;

                while ((*p != '\0') && (*p != '/')) {
                    p++;
                }
                if (*p == '/') {
                    *p++ = '\0';
                }
                url = p;
                
                /*
                 * Check for port
                 */
                Ns_HttpParseHost(h, NULL, &p);
                if (p != NULL) {
                    *p++ = '\0';
                    request->port = (unsigned short)strtol(p, NULL, 10);
                }
                request->host = ns_strdup(h);
            }
        }
    }
    SetUrl(request, url);
    Ns_DStringFree(&ds);
    return NS_OK;

done:
    Ns_ResetRequest(request);
    Ns_DStringFree(&ds);
    return NS_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SkipUrl --
 *
 *	Return a pointer n elements into the request's url. 
 *
 * Results:
 *	The url beginning n elements in. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SkipUrl(const Ns_Request *request, int n)
{
    size_t skip;

    NS_NONNULL_ASSERT(request != NULL);

    if (n > request->urlc) {
        return NULL;
    }
    skip = 0u;
    while (--n >= 0) {
        skip += strlen(request->urlv[n]) + 1u;
    }
    return (request->url + skip);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetRequestUrl --
 *
 *	Set the url in a request structure. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	Makes a copy of url. 
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
 *	Free the url in a request. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	None. 
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
        ns_free(request->urlv[0]);
        ns_free(request->urlv);
        request->urlv = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * SetUrl --
 *
 *	Break up an URL and put it in the request. 
 *
 * Results:
 *	None. 
 *
 * Side effects:
 *	Memory allocated for members.
 *
 *----------------------------------------------------------------------
 */

static void
SetUrl(Ns_Request *request, char *url)
{
    Tcl_DString  ds1, ds2;
    char       *p;

    NS_NONNULL_ASSERT(request != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Tcl_DStringInit(&ds1);
    Tcl_DStringInit(&ds2);

    /*
     * Look for a query string at the end of the URL.
     */
    
    p = strchr(url, '?');
    if (p != NULL) {
        *p++ = '\0';
        if (request->query != NULL) {
            ns_free(request->query);
        }
        if (*p != '\0') {
            request->query = ns_strdup(p);
        }
    }

    /*
     * Decode and normalize the URL.
     */

    p = Ns_UrlPathDecode(&ds1, url, Ns_GetUrlEncoding(NULL));
    if (p == NULL) {
        p = url;
    }
    (void)Ns_NormalizePath(&ds2, p);
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
    Tcl_DStringFree(&ds2);

    /*
     * Build the urlv and set urlc. The following loop is somewhat an
     * abuse of Tcl_DStringAppend, since we build here urlv (an array
     * of (char *)) based on operations intended on strings. However,
     * this way we can reuse the Tcl_DString infrastructure.
     */

    p = ns_strdup(request->url + 1);
    Tcl_DStringAppend(&ds1, (char *) &p, (int)sizeof(char *));
    while (*p != '\0') {
        if (*p == '/') {
            *p++ = '\0';
            if (*p == '\0') {
		/*
		 * Stop on a trailing slash.
		 */
		
                break;
            }
            Tcl_DStringAppend(&ds1, (char *) &p, (int)sizeof(char *));
        }
        ++p;
    }
    request->urlc = ds1.length / (int)sizeof(char *);
    p = NULL;
    Tcl_DStringAppend(&ds1, (char *) &p, (int)sizeof(char *));
    request->urlv = (char **) ns_malloc((size_t)ds1.length);
    memcpy(request->urlv, ds1.string, (size_t)ds1.length);
    Tcl_DStringFree(&ds1);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseHeader --
 *
 *	Consume a header line, handling header continuation, placing
 *	results in given set.
 *
 * Results:
 *	NS_OK/NS_ERROR 
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Ns_ParseHeader(Ns_Set *set, const char *line, Ns_HeaderCaseDisposition disp)
{
    char           *sep;
    char           *value;
    size_t          index;
    Ns_DString	    ds;

    /* 
     * Header lines are first checked if they continue a previous
     * header indicated by any preceeding white space.  Otherwise,
     * they must be in well form key: value form.
     */

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(line != NULL);

    if (CHARTYPE(space, *line) != 0) {
        if (Ns_SetSize(set) == 0u) {
	    return NS_ERROR;	/* Continue before first header. */
        }
	index = Ns_SetLast(set);
        while (CHARTYPE(space, *line) != 0) {
            ++line;
        }
        if (*line != '\0') {
	    value = Ns_SetValue(set, index);
	    Ns_DStringInit(&ds);
	    Ns_DStringVarAppend(&ds, value, " ", line, NULL);
	    Ns_SetPutValue(set, index, ds.string);
	    Ns_DStringFree(&ds);
	}
    } else {
        char *key;

        sep = strchr(line, ':');
        if (sep == NULL) {
	    return NS_ERROR;	/* Malformed header. */
	}
        *sep = '\0';
        value = sep + 1;
        while (*value != '\0' && CHARTYPE(space, *value) != 0) {
            ++value;
        }
        index = Ns_SetPut(set, line, value);
        key = Ns_SetKey(set, index);
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
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * GetQvalue --
 *
 *      Return the next qvalue string from accept encodings
 *
 * Results:
 *      string, setting lenghtPtr; or NULL, if no or invalie
 *      qvalue provided
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static const char *
GetQvalue(const char *str, int *lenPtr) {
    const char *resultString;

    NS_NONNULL_ASSERT(str != NULL);
    NS_NONNULL_ASSERT(lenPtr != NULL);

    for (; *str == ' '; str++) {
        ;
    }
    if (*str != ';') {
        return NULL;
    }
    for (str ++; *str == ' '; str++) {
        ;
    }
    if (*str != 'q') {
        return NULL;
    }
    for (str ++; *str == ' '; str++) {
        ;
    }
    if (*str != '=') {
        return NULL;
    }
    for (str ++; *str == ' '; str++) {
        ;
    }
    if (CHARTYPE(digit,*str) == 0) {
        return NULL;
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
    /* str should point to a valid terminator of the number */
    if (*str == ' ' || *str == ',' || *str == ';' || *str == '\0') {
        *lenPtr = (int)(str - resultString);
	return resultString;
    }
    return NULL;
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
 *      On success non-NULL value and he parsed qValue;
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
	int len = 0;
	const char *qValueString = GetQvalue(encodingStr + strlen(encodingFormat), &len);

	if (qValueString != NULL) {
	    *qValue = strtod(qValueString, NULL);
	} else {
	    *qValue = 1.0;
	}
	return encodingStr;
    }
    
    *qValue = -1.0;
    return NULL;
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
 *      0 or 1
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsParseAcceptEncoding(double version, const char *hdr) 
{
    double gzipQvalue = -1.0, starQvalue = -1.0, identityQvalue = -1.0;
    int gzip = 0;

    NS_NONNULL_ASSERT(hdr != NULL);

    if (GetEncodingFormat(hdr, "gzip", &gzipQvalue) != NULL) {
	/* we have gzip specified in accept-encoding */
	if (gzipQvalue > 0.999) {
	    /* gzip qvalue 1, use it, nothing else can be higher */
	    gzip = 1;
	} else if (gzipQvalue < 0.0009) {
	    /* gzip qvalue 0, forbid gzip */
	    gzip = 0;
	} else {
	    /* a middle gzip qvalue, compare it with identity and default */
	    if (GetEncodingFormat(hdr, "identity", &identityQvalue) != NULL) {
		/* gzip qvalue larger than identity */
		gzip = (gzipQvalue >= identityQvalue);
	    } else if (GetEncodingFormat(hdr, "*", &starQvalue) != NULL) {
		/* gzip qvalue larger than default */
		gzip = (gzipQvalue >= starQvalue);
	    } else {
		/* just the low qvalue was specified */
		gzip = 1;
	    }
	}
    } else if (GetEncodingFormat(hdr, "*", &starQvalue) != NULL) {
	/* star matches everything, so as well gzip */
	if (starQvalue < 0.0009) {
	    /* star qvalue forbids gzip */
	    gzip = 0;
	} else if (GetEncodingFormat(hdr, "identity", &identityQvalue) != NULL) {
	    /* star qvalue allows gzip in HTTP/1.1 */
	    gzip = (starQvalue >= identityQvalue) && (version >= 1.1);
	} else {
	    /* no identity specified, assume gzip is matched with * in HTTP/1.1 */
	    gzip = (version >= 1.1);
	}
    }

    return gzip;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
