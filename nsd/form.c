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
 * form.c --
 *
 *      Routines for dealing with HTML FORM's.
 */

#include "nsd.h"

/*
 * Local functions defined in this file.
 */

static Ns_ReturnCode ParseQuery(char *form, Ns_Set *set, Tcl_Encoding encoding, bool translate)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ReturnCode ParseQueryWithFallback(Tcl_Interp *interp, NsServer *servPtr,
                                            char *toParse, Ns_Set *set,
                                            Tcl_Encoding encoding, bool translate,
                                            Tcl_Obj *fallbackCharsetObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static Ns_ReturnCode ParseMultipartEntry(Conn *connPtr, Tcl_Encoding valueEncoding, const char *start, char *end)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static char *Ext2utf(Tcl_DString *dsPtr, const char *start, size_t len, Tcl_Encoding encoding, char unescape)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static bool GetBoundary(Tcl_DString *dsPtr, const char *contentType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static char *NextBoundary(const Tcl_DString *dsPtr, char *s, const char *e)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PURE;

static bool GetValue(const char *hdr, const char *att, const char **vsPtr, const char **vePtr, char *uPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);



/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnGetQuery --
 *
 *      Return the connection query data in form of an Ns_Set. This function
 *      parses the either the query (of the request URL) or the form content
 *      (in POST requests with content type "www-form-urlencoded" or
 *      "multipart/form-data"). In case the Ns_Set for the query is already
 *      set, it is treated as cached result and is returned untouched.
 *
 * Results:
 *      Query data or NULL if no form data is available.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_ConnGetQuery(Tcl_Interp *interp, Ns_Conn *conn, Tcl_Obj *fallbackCharsetObj, Ns_ReturnCode *rcPtr)
{
    Conn *connPtr;

    NS_NONNULL_ASSERT(conn != NULL);
    connPtr = (Conn *) conn;

    /*
     * connPtr->query is used to cache the result, in case this function is
     * called multiple times during a single request.
     */
    if (connPtr->query == NULL) {
        const char   *contentType, *charset = NULL;
        char         *content = NULL, *toParse = NULL;
        size_t        charsetOffset;
        bool          haveFormData = NS_FALSE;
        Ns_ReturnCode status = NS_OK;

        /*
         * We are called the first time, so create an ns_set named (slightly
         * misleading "connPtr->query")
         */
        if (connPtr->formData == NULL) {
            connPtr->formData = Ns_SetCreate(NS_SET_NAME_QUERY);
        }
        connPtr->query = connPtr->formData;
        contentType = Ns_SetIGet(connPtr->headers, "content-type");

        if (contentType != NULL) {
            charset = NsFindCharset(contentType, &charsetOffset);
            if (strncmp(contentType, "application/x-www-form-urlencoded", 33u) == 0) {
                haveFormData = NS_TRUE;
            } else if (strncmp(contentType, "multipart/form-data", 19u) == 0) {
                haveFormData = NS_TRUE;
            }
        }

        if (haveFormData) {
            /*
             * It is unsafe to access the content when the
             * connection is already closed due to potentially
             * unmmapped memory.
             */
            if ((connPtr->flags & NS_CONN_CLOSED) == 0u) {
                content = connPtr->reqPtr->content;
            } else {
                /*
                 * Formdata is unavailable, but do not fall back to the
                 * query-as-formdata tradition. We should keep a consistent
                 * behavior.
                 */
            }
        } else if (connPtr->request.query != NULL) {
            /*
             * The content has none of the "FORM" content types, so get it
             * in good old AOLserver tradition from the query variables.
             */
            toParse = connPtr->request.query;
            status = ParseQueryWithFallback(interp, connPtr->poolPtr->servPtr,
                                            toParse, connPtr->query, connPtr->urlEncoding,
                                            NS_FALSE, fallbackCharsetObj);
        }

        if (content != NULL) {
            Tcl_DString boundaryDs;
            /*
             * We have one of the accepted content types AND the data is
             * provided via content string.
             */
            Tcl_DStringInit(&boundaryDs);

            if (*contentType == 'a') {
                /*
                 * The content-type is "application/x-www-form-urlencoded"
                 */
                bool         translate;
                Tcl_Encoding encoding;
#ifdef _WIN32
                /*
                 * Keep CRLF
                 */
                translate = NS_FALSE;
#else
                /*
                 * Translate CRLF -> LF, since browsers translate all
                 * LF to CRLF in the body of POST requests.
                 */
                translate = NS_TRUE;
#endif
                if (charset != NULL) {
                    encoding = Ns_GetCharsetEncoding(charset);
                } else {
                    encoding = connPtr->urlEncoding;
                }
                toParse = content;
                status = ParseQueryWithFallback(interp, connPtr->poolPtr->servPtr,
                                                content, connPtr->query, encoding,
                                                translate, fallbackCharsetObj);

            } else if (GetBoundary(&boundaryDs, contentType)) {
                /*
                 * GetBoundary cares for "multipart/form-data; boundary=...".
                 */
                const char  *formEndPtr = content + connPtr->reqPtr->length;
                char        *firstBoundary = NextBoundary(&boundaryDs, content, formEndPtr), *s;
                Tcl_Encoding valueEncoding = connPtr->urlEncoding;

                s = firstBoundary;
                for (;;) {
                    const char *defaultCharset;

                    while (s != NULL) {
                        char  *e;

                        s += boundaryDs.length;
                        if (*s == '\r') {
                            ++s;
                        }
                        if (*s == '\n') {
                            ++s;
                        }
                        e = NextBoundary(&boundaryDs, s, formEndPtr);
                        if (e != NULL) {
                            status = ParseMultipartEntry(connPtr, valueEncoding, s, e);
                            if (status == NS_ERROR) {
                                toParse = s;
                            }
                        }
                        s = e;
                    }
                    /*
                     * We have now parsed all form fields into
                     * connPtr->query. According to the HTML5 standard, we
                     * have to check for a form entry named "_charset_"
                     * specifying the "default charset".
                     * https://datatracker.ietf.org/doc/html/rfc7578#section-4.6
                     */
                    defaultCharset = Ns_SetGet(connPtr->query, "_charset_");
                    if (defaultCharset != NULL && strcmp(defaultCharset, "utf-8") != 0) {
                        /*
                         * We got an explicit charset different from UTF-8. We
                         * have to reparse the input data.
                         */
                        Tcl_Encoding defaultEncoding = Ns_GetCharsetEncoding(defaultCharset);

                        if (valueEncoding != NULL) {
                            if (valueEncoding != defaultEncoding) {
                                valueEncoding = defaultEncoding;
                                s = firstBoundary;
                                Ns_SetTrunc(connPtr->query, 0u);
                                Ns_Log(Debug, "form: retry with default charset %s", defaultCharset);
                                continue;
                            }
                        } else {
                            Ns_Log(Error, "multipart form: invalid charset specified"
                                   " inside of form '%s'", defaultCharset);
                            status = NS_ERROR;
                        }
                    }
                    break;
                }
            }
            Tcl_DStringFree(&boundaryDs);
        }

        if (status == NS_ERROR) {
            Ns_Log(Warning, "formdata: could not parse '%s'", toParse);
            Ns_ConnClearQuery(conn);
            if (rcPtr != NULL) {
                *rcPtr = status;
                if (interp != NULL) {
                    Ns_TclPrintfResult(interp,
                                       "cannot decode '%s'; contains invalid UTF-8",
                                       toParse);
                    Tcl_SetErrorCode(interp, "NS_INVALID_UTF8", NULL);
                }
            }
            return NULL;
        }
    }

    return connPtr->query;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnClearQuery --
 *
 *      Release the any query set cached up from a previous call
 *      to Ns_ConnGetQuery.  Useful if the query data requires
 *      reparsing, as when the encoding changes.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnClearQuery(Ns_Conn *conn)
{
    Conn *connPtr;

    NS_NONNULL_ASSERT(conn != NULL);
    connPtr = (Conn *) conn;

    if (connPtr->query != NULL) {
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;

        Ns_SetTrunc(connPtr->query, 0);
        connPtr->query = NULL;

        hPtr = Tcl_FirstHashEntry(&connPtr->files, &search);
        while (hPtr != NULL) {
            FormFile *filePtr = Tcl_GetHashValue(hPtr);

            if (filePtr->hdrObj != NULL) {
                Tcl_DecrRefCount(filePtr->hdrObj);
            }
            if (filePtr->offObj != NULL) {
                Tcl_DecrRefCount(filePtr->offObj);
            }
            if (filePtr->sizeObj != NULL) {
                Tcl_DecrRefCount(filePtr->sizeObj);
            }
            ns_free(filePtr);

            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&connPtr->files);
        Tcl_InitHashTable(&connPtr->files, TCL_STRING_KEYS);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_QueryToSet --
 *
 *      Parse query data into a given Ns_Set.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      Will add data to set.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_QueryToSet(char *query, Ns_Set *set, Tcl_Encoding encoding)
{
    NS_NONNULL_ASSERT(query != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    return ParseQuery(query, set, encoding, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclParseQueryObjCmd --
 *
 *      Implements "ns_parsequery".
 *
 * Results:
 *      The Tcl result is a Tcl set with the parsed name-value pairs from
 *      the querystring argument
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclParseQueryObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int       result;
    NsInterp *itPtr = clientData;
    char     *charset = NULL, *chars = (char *)NS_EMPTY_STRING;
    Tcl_Obj  *fallbackCharsetObj = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-charset",         Ns_ObjvString, &charset, NULL},
        {"-fallbackcharset", Ns_ObjvObj,    &fallbackCharsetObj, NULL},
        {"--",               Ns_ObjvBreak,  NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"querystring", Ns_ObjvString, &chars, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_Encoding encoding;
        Ns_Set      *set = Ns_SetCreate(NS_SET_NAME_PARSEQ);

        if (charset != NULL) {
            encoding = Ns_GetCharsetEncoding(charset);
        } else {
            encoding = Ns_GetUrlEncoding(NULL);
        }

        if (ParseQueryWithFallback(interp, itPtr->servPtr,
                                   chars, set, encoding,
                                   NS_FALSE, fallbackCharsetObj)) {
            Ns_TclPrintfResult(interp, "could not parse query: \"%s\"", chars);
            Tcl_SetErrorCode(interp, "NS_INVALID_UTF8", NULL);
            Ns_SetFree(set);
            result = TCL_ERROR;
        } else {
            result = Ns_TclEnterSet(interp, set, NS_TCL_SET_DYNAMIC);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseQuery --
 *
 *      Parse the given form string for URL encoded key=value pairs,
 *      converting to UTF8 if given encoding is not NULL.
 *
 * Results:
 *      TCL_OK or TCL_ERROR in case parsing was not possible.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
ParseQuery(char *form, Ns_Set *set, Tcl_Encoding encoding, bool translate)
{
    Tcl_DString   kds, vds, vds2;
    char         *p;
    Ns_ReturnCode result = NS_OK;

    NS_NONNULL_ASSERT(form != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    Tcl_DStringInit(&kds);
    Tcl_DStringInit(&vds);
    Tcl_DStringInit(&vds2);
    p = form;

    while (p != NULL) {
        char       *v;
        const char *k;

        k = p;
        p = strchr(p, INTCHAR('&'));
        if (p != NULL) {
            *p = '\0';
        }
        v = strchr(k, INTCHAR('='));
        if (v != NULL) {
            *v = '\0';
        }
        Ns_DStringSetLength(&kds, 0);
        k = Ns_UrlQueryDecode(&kds, k, encoding, &result);
        if (v != NULL) {
            Ns_DStringSetLength(&vds, 0);

            (void) Ns_UrlQueryDecode(&vds, v+1, encoding, &result);
            *v = '=';
            v = vds.string;
            if (translate) {
                char *q = strchr(v, INTCHAR('\r'));

                if (q != NULL) {
                    /*
                     * We have one or more CR in the field content.
                     * Remove these.
                     */
                    Ns_DStringSetLength(&vds2, 0);
                    do {
                        Tcl_DStringAppend(&vds2, v, (int)(q - v));
                        v = q +1;
                        q = strchr(v, INTCHAR('\r'));
                    } while (q != NULL);
                    /*
                     * Append the remaining string.
                     */
                    Tcl_DStringAppend(&vds2, v, -1);
                    v = vds2.string;
                }
            }
        }
        if (result == TCL_OK) {
            (void) Ns_SetPut(set, k, v);
        }
        if (p != NULL) {
            *p++ = '&';
        }
    }
    Tcl_DStringFree(&kds);
    Tcl_DStringFree(&vds);
    Tcl_DStringFree(&vds2);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ParseQueryWithFallback --
 *
 *      Helper function for ParseQuery(), which handles fallback charset for
 *      cases, where converting to UTF8 fails (due to invalid UTF-8).
 *
 * Results:
 *      TCL_OK or TCL_ERROR in case parsing was not possible.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ParseQueryWithFallback(Tcl_Interp *interp, NsServer *servPtr, char *toParse,
                       Ns_Set *set, Tcl_Encoding encoding,
                       bool translate, Tcl_Obj *fallbackCharsetObj)
{
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(toParse != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    status = ParseQuery(toParse, set, encoding,  translate);
    if (status == NS_ERROR) {
        Tcl_Encoding fallbackEncoding = NULL;
        Ns_ReturnCode rc;

        /*
         * ParseQuery failed. This might be due to invalid UTF-8. Retry with
         * fallbackCharset if specified.
         */
        rc = NsGetFallbackEncoding(interp, servPtr, fallbackCharsetObj, NS_TRUE, &fallbackEncoding);
        if (rc == NS_OK && fallbackEncoding != NULL && fallbackEncoding != encoding) {
            Ns_Log(Notice, "Retry ParseQuery with encoding %s",
                   Ns_GetEncodingCharset(fallbackEncoding));
            Ns_SetTrunc(set, 0u);
            status = ParseQuery(toParse, set, fallbackEncoding,  translate);
        }
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseMultipartEntry --
 *
 *      Parse a single part of a multipart form.
 *
 * Results:
 *      Ns_ReturnCode (NS_OK or NS_ERROR).
 *
 * Side effects:
 *      Records offset, lengths for files.  After execution, connPtr->query
 *      contains the parsed form in form of an Ns_Set.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
ParseMultipartEntry(Conn *connPtr, Tcl_Encoding valueEncoding, const char *start, char *end)
{
    Tcl_Encoding  encoding;
    Tcl_DString   kds, vds;
    char         *e, saveend, unescape;
    const char   *ks = NULL, *ke, *disp;
    Ns_Set       *set;
    int           isNew;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(connPtr != NULL);
    NS_NONNULL_ASSERT(start != NULL);
    NS_NONNULL_ASSERT(end != NULL);

    encoding = connPtr->urlEncoding;

    Tcl_DStringInit(&kds);
    Tcl_DStringInit(&vds);
    set = Ns_SetCreate(NS_SET_NAME_MP);

    /*
     * Trim off the trailing \r\n and null terminate the input.
     */

    if (end > start && *(end-1) == '\n') {
        --end;
    }
    if (end > start && *(end-1) == '\r') {
        --end;
    }
    saveend = *end;
    *end = '\0';

    /*
     * Parse header lines
     */

    while ((e = strchr(start, INTCHAR('\n'))) != NULL) {
        const char *s = start;
        char        save;

        start = e + 1;
        if (e > s && *(e-1) == '\r') {
            --e;
        }
        if (s == e) {
            /*
             * Reached empty line, end of header.
             */
            break;
        }
        save = *e;
        *e = '\0';
        (void) Ns_ParseHeader(set, s, NULL, ToLower, NULL);
        *e = save;
    }

    /*
     * Look for valid disposition header.
     */

    disp = Ns_SetGet(set, "content-disposition");
    if (disp != NULL && GetValue(disp, "name=", &ks, &ke, &unescape) == NS_TRUE) {
        const char *key = Ext2utf(&kds, ks, (size_t)(ke - ks), encoding, unescape);
        const char *value, *fs = NULL, *fe = NULL;

        if (key == NULL) {
            status = NS_ERROR;
            goto bailout;
        }
        Ns_Log(Debug, "ParseMultipartEntry disp '%s'", disp);

        if (GetValue(disp, "filename=", &fs, &fe, &unescape) == NS_FALSE) {
            /*
             * Plain (non-file) entry.
             */
            if (valueEncoding == NULL) {
                valueEncoding = encoding;
            }
            Ns_Log(Debug, "ParseMultipartEntry LINE '%s'", start);
            value = Ext2utf(&vds, start, (size_t)(end - start), valueEncoding, unescape);
            if (value == NULL) {
                status = NS_ERROR;
                goto bailout;
            }
        } else {
            Tcl_HashEntry *hPtr;
            FormFile      *filePtr;
            Tcl_Interp    *interp = connPtr->itPtr->interp;

            assert(fs != NULL);
            value = Ext2utf(&vds, fs, (size_t)(fe - fs), encoding, unescape);
            if (value == NULL) {
                status = NS_ERROR;
                goto bailout;
            }

            hPtr = Tcl_CreateHashEntry(&connPtr->files, key, &isNew);
            if (isNew != 0) {

                filePtr = ns_malloc(sizeof(FormFile));
                Tcl_SetHashValue(hPtr, filePtr);

                filePtr->hdrObj = Tcl_NewListObj(0, NULL);
                filePtr->offObj = Tcl_NewListObj(0, NULL);
                filePtr->sizeObj = Tcl_NewListObj(0, NULL);

                Tcl_IncrRefCount(filePtr->hdrObj);
                Tcl_IncrRefCount(filePtr->offObj);
                Tcl_IncrRefCount(filePtr->sizeObj);
            } else {
                filePtr = Tcl_GetHashValue(hPtr);
            }

            (void) Ns_TclEnterSet(interp, set, NS_TCL_SET_DYNAMIC);
            (void) Tcl_ListObjAppendElement(interp, filePtr->hdrObj,
                                            Tcl_GetObjResult(interp));
            Tcl_ResetResult(connPtr->itPtr->interp);

            (void) Tcl_ListObjAppendElement(interp, filePtr->offObj,
                                            Tcl_NewIntObj((int)(start - connPtr->reqPtr->content)));

            (void) Tcl_ListObjAppendElement(interp, filePtr->sizeObj,
                                            Tcl_NewWideIntObj((Tcl_WideInt)(end - start)));
            set = NULL;
        }
        Ns_Log(Debug, "ParseMultipartEntry sets '%s': '%s'", key, value);
        (void) Ns_SetPut(connPtr->query, key, value);
    }

    /*
     * Restore the end marker.
     */
 bailout:
    *end = saveend;
    Tcl_DStringFree(&kds);
    Tcl_DStringFree(&vds);
    if (set != NULL) {
        Ns_SetFree(set);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * GetBoundary --
 *
 *      Copy multipart/form-data boundary string, if any.
 *
 * Results:
 *      NS_TRUE if boundary copied, NS_FALSE otherwise.
 *
 * Side effects:
 *      Copies boundary string to given dstring.
 *
 *----------------------------------------------------------------------
 */

static bool
GetBoundary(Tcl_DString *dsPtr, const char *contentType)
{
    const char *bs;
    bool        success = NS_FALSE;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(contentType != NULL);

    if ((Ns_StrCaseFind(contentType, "multipart/form-data") != NULL)
        && ((bs = Ns_StrCaseFind(contentType, "boundary=")) != NULL)) {
        const char *be;

        bs += 9;
        be = bs;
        while ((*be != '\0') && (CHARTYPE(space, *be) == 0)) {
            ++be;
        }
        Tcl_DStringAppend(dsPtr, "--", 2);
        Tcl_DStringAppend(dsPtr, bs, (int)(be - bs));
        success = NS_TRUE;
    }
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * NextBoundary --
 *
 *      Locate the next form boundary.
 *
 * Results:
 *      Pointer to start of next input field or NULL on end of fields.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
NextBoundary(const Tcl_DString *dsPtr, char *s, const char *e)
{
    char c, sc;
    const char *find;
    size_t len;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(s != NULL);
    NS_NONNULL_ASSERT(e != NULL);

    find = dsPtr->string;
    c = *find++;
    len = (size_t)(dsPtr->length - 1);
    e -= len;
    do {
        do {
            sc = *s++;
            if (s > e) {
                return NULL;
            }
        } while (sc != c);
    } while (strncmp(s, find, len) != 0);
    s--;

    return s;
}


/*
 *----------------------------------------------------------------------
 *
 * GetValue --
 *
 *      Determine start and end of a multipart form input value.
 *
 * Results:
 *      NS_TRUE if attribute found and value parsed, NS_FALSE otherwise.
 *
 * Side effects:
 *      Start and end are stored in given pointers, quoted character,
 *      when it was preceded by a backslash.
 *
 *----------------------------------------------------------------------
 */

static bool
GetValue(const char *hdr, const char *att, const char **vsPtr, const char **vePtr, char *uPtr)
{
    const char *s;
    bool        success = NS_TRUE;

    NS_NONNULL_ASSERT(hdr != NULL);
    NS_NONNULL_ASSERT(att != NULL);
    NS_NONNULL_ASSERT(vsPtr != NULL);
    NS_NONNULL_ASSERT(vePtr != NULL);
    NS_NONNULL_ASSERT(uPtr != NULL);

    s = Ns_StrCaseFind(hdr, att);
    if (s == NULL) {
        success = NS_FALSE;
    } else {
        const char *e;

        s += strlen(att);
        e = s;
        if (*s != '"' && *s != '\'') {
            /*
             * End of unquoted att=value is next space.
             */
            while (*e != '\0' && CHARTYPE(space, *e) == 0) {
                ++e;
            }
            *uPtr = '\0';
        } else {
            bool escaped = NS_FALSE;

            *uPtr = '\0';
            /*
             * End of quoted att="value" is next quote.  A quote within
             * the quoted string could be escaped with a backslash. In
             * case, an escaped quote was detected, report the quote
             * character as result.
             */
            ++e;
            while (*e != '\0' && (escaped || *e != *s)) {
                if (escaped) {
                    escaped = NS_FALSE;
                } else if (*e == '\\') {
                    *uPtr = *s;
                    escaped = NS_TRUE;
                }
                ++e;
            }
            ++s;
        }
        *vsPtr = s;
        *vePtr = e;
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ext2utf --
 *
 *      Convert input string to UTF.
 *
 * Results:
 *      Pointer to converted string or NULL, when conversion to UTF-8 fails.
 *
 * Side effects:
 *      Converted string is copied to given dString, overwriting
 *      any previous content.
 *
 *----------------------------------------------------------------------
 */

static char *
Ext2utf(Tcl_DString *dsPtr, const char *start, size_t len, Tcl_Encoding encoding, char unescape)
{
    char *buffer;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(start != NULL);

    if (encoding == NULL) {
        Tcl_DStringSetLength(dsPtr, 0);
        Tcl_DStringAppend(dsPtr, start, (int)len);
        buffer = dsPtr->string;
    } else {
        Tcl_DString ds;
        /*
         * Actual to UTF conversion.
         */
        if (NsEncodingIsUtf8(encoding) && !Ns_Valid_UTF8((const unsigned char *)start, len, &ds)) {
            Ns_Log(Warning, "form: multipart contains invalid UTF8: %s", ds.string);
            Tcl_DStringFree(&ds);
            buffer = NULL;
        } else {
            /*
             * ExternalToUtfDString will re-init dstring.
             */
            Tcl_DStringFree(dsPtr);
            (void) Tcl_ExternalToUtfDString(encoding, start, (int)len, dsPtr);
            buffer = dsPtr->string;
        }
    }

    /*
     * In case the string contains backslash escaped characters, the
     * backslashes have to be removed. This will shorten the resulting
     * string.
     */
    if (buffer != NULL && unescape != '\0') {
      int i, j, l = (int)len;

      for (i = 0; i<l; i++) {
        if (buffer[i] == '\\' && buffer[i+1] == unescape) {
          for (j = i; j < l; j++) {
            buffer[j] = buffer[j+1];
          }
          l --;
        }
      }
      Tcl_DStringSetLength(dsPtr, l);
      buffer = dsPtr->string;
    }

    return buffer;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
