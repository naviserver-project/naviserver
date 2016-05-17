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

static void ParseQuery(char *form, Ns_Set *set, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void ParseMultiInput(Conn *connPtr, const char *start, char *end)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static char *Ext2utf(Tcl_DString *dsPtr, const char *start, size_t len, Tcl_Encoding encoding, char unescape)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int GetBoundary(Tcl_DString *dsPtr, const char *contentType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static char *NextBoundry(const Tcl_DString *dsPtr, char *s, const char *e)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static bool GetValue(const char *hdr, const char *att, const char **vsPtr, const char **vePtr, char *uPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);



/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnGetQuery --
 *
 *      Return the connection query data in form of a Ns_Set. This function
 *      parses the either the query (of the URL) or the form content (in POST
 *      requests with content type "www-form-urlencoded" or
 *      "multipart/form-data"). In case the Ns_Set for the query is already
 *      set, it is treated as cached result and is returned untouched.
 *
 * Results:
 *      Query data or NULL if error 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set  *
Ns_ConnGetQuery(Ns_Conn *conn)
{
    Conn           *connPtr = (Conn *) conn;
    char           *form;

    NS_NONNULL_ASSERT(conn != NULL);
    
    if (connPtr->query == NULL) {
        connPtr->query = Ns_SetCreate(NULL);
        if (connPtr->request.method != NULL && !STREQ(connPtr->request.method, "POST")) {
            /*
             * If it is no POST resquest, parse the "form" content from the
             * query parameters.
             */
            form = connPtr->request.query;
            if (form != NULL) {
                ParseQuery(form, connPtr->query, connPtr->urlEncoding);
            }
        } else if (/* 
		    * It is unsafe to access the content when the
		    * connection is already closed due to potentially
		    * unmmapped memory.
		    */
		   (connPtr->flags & NS_CONN_CLOSED ) == 0u
		   && (form = connPtr->reqPtr->content) != NULL
		   ) {
            Tcl_DString   bound;
            const char   *contentType = Ns_SetIGet(conn->headers, "content-type");

            if (contentType != NULL) {
                Tcl_DStringInit(&bound);

                /*
                 * GetBoundary cares for "multipart/form-data"
                 */
                if (GetBoundary(&bound, contentType) == 0) {
                    if (Ns_StrCaseFind(contentType, "www-form-urlencoded") != NULL) {
                        ParseQuery(form, connPtr->query, connPtr->urlEncoding);
                    }
                    /*
                     * Don't do anything for other content-types.
                     */
                } else {
                    const char *formend = form + connPtr->reqPtr->length;
                    char       *s;
                    
                    s = NextBoundry(&bound, form, formend);
                    while (s != NULL) {
                        char  *e;
                        
                        s += bound.length;
                        if (*s == '\r') {
                            ++s;
                        }
                        if (*s == '\n') {
                            ++s;
                        }
                        e = NextBoundry(&bound, s, formend);
                        if (e != NULL) {
                            ParseMultiInput(connPtr, s, e);
                        }
                        s = e;
                    }
                }
                Tcl_DStringFree(&bound);
            }
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
    Conn           *connPtr = (Conn *) conn;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    NS_NONNULL_ASSERT(conn != NULL);
    
    if (connPtr->query == NULL) {
        return;
    }

    Ns_SetFree(connPtr->query);
    connPtr->query = NULL;

    hPtr = Tcl_FirstHashEntry(&connPtr->files, &search);
    while (hPtr != NULL) {
	FormFile *filePtr = Tcl_GetHashValue(hPtr);

        if (filePtr->hdrObj) {
            Tcl_DecrRefCount(filePtr->hdrObj);
        }
        if (filePtr->offObj) {
            Tcl_DecrRefCount(filePtr->offObj);
        }
        if (filePtr->sizeObj) {
            Tcl_DecrRefCount(filePtr->sizeObj);
        }
        ns_free(filePtr);
        
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&connPtr->files);
    Tcl_InitHashTable(&connPtr->files, TCL_STRING_KEYS);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_QueryToSet --
 *
 *      Parse query data into an Ns_Set 
 *
 * Results:
 *      NS_OK. 
 *
 * Side effects:
 *      Will add data to set without any UTF conversion.
 *
 *----------------------------------------------------------------------
 */

int
Ns_QueryToSet(char *query, Ns_Set *set)
{
    NS_NONNULL_ASSERT(query != NULL);
    NS_NONNULL_ASSERT(set != NULL);
    
    ParseQuery(query, set, NULL);
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclParseQueryObjCmd --
 *
 *      Implements the ns_parsequery command.
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
NsTclParseQueryObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Set *set;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "querystring");
        return TCL_ERROR;
    }
    set = Ns_SetCreate(NULL);
    if (Ns_QueryToSet(Tcl_GetString(objv[1]), set) != NS_OK) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
            "could not parse: \"", Tcl_GetString(objv[1]), "\"", NULL);
        Ns_SetFree(set);
        return TCL_ERROR;
    }
    return Ns_TclEnterSet(interp, set, NS_TCL_SET_DYNAMIC);
}


/*
 *----------------------------------------------------------------------
 *
 * ParseQuery --
 *
 *      Parse the given form string for URL encoded key=value pairs,
 *      converting to UTF if given encoding is not NULL.
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
ParseQuery(char *form, Ns_Set *set, Tcl_Encoding encoding)
{
    Tcl_DString  kds, vds;
    char  *p;

    NS_NONNULL_ASSERT(form != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    Tcl_DStringInit(&kds);
    Tcl_DStringInit(&vds);
    p = form;

    while (p != NULL) {
	char *v;
	const char *k;

        k = p;
        p = strchr(p, '&');
        if (p != NULL) {
            *p = '\0';
        }
        v = strchr(k, '=');
        if (v != NULL) {
            *v = '\0';
        }
        Ns_DStringSetLength(&kds, 0);
        k = Ns_UrlQueryDecode(&kds, k, encoding);
        if (v != NULL) {
            Ns_DStringSetLength(&vds, 0);
            (void) Ns_UrlQueryDecode(&vds, v+1, encoding);
            *v = '=';
            v = vds.string;
        }
        (void) Ns_SetPut(set, k, v);
        if (p != NULL) {
            *p++ = '&';
        }
    }
    Tcl_DStringFree(&kds);
    Tcl_DStringFree(&vds);
}


/*
 *----------------------------------------------------------------------
 *
 * ParseMulitInput --
 *
 *      Parse the a multipart form input.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Records offset, lengths for files.
 *
 *----------------------------------------------------------------------
 */

static void
ParseMultiInput(Conn *connPtr, const char *start, char *end)
{
    Tcl_Encoding encoding;
    Tcl_DString  kds, vds;
    char        *e, saveend, *disp, unescape;
    const char  *ks, *ke;
    Ns_Set      *set;
    int          isNew;

    NS_NONNULL_ASSERT(connPtr != NULL);
    NS_NONNULL_ASSERT(start != NULL);
    NS_NONNULL_ASSERT(end != NULL);

    encoding = connPtr->urlEncoding;
    
    Tcl_DStringInit(&kds);
    Tcl_DStringInit(&vds);
    set = Ns_SetCreate(NULL);

    /*
     * Trim off the trailing \r\n and null terminate the input.
     */

    if (end > start && *(end-1) == '\n') {--end;}
    if (end > start && *(end-1) == '\r') {--end;}
    saveend = *end;
    *end = '\0';

    /*
     * Parse header lines
     */

    ks = NULL;
    while ((e = strchr(start, '\n')) != NULL) {
	const char *s = start;
	char save;

        start = e + 1;
        if (e > s && *(e-1) == '\r') {
            --e;
        }
        if (s == e) {
            break;
        }
        save = *e;
        *e = '\0';
        (void) Ns_ParseHeader(set, s, ToLower);
        *e = save;
    }

    /*
     * Look for valid disposition header.
     */

    disp = Ns_SetGet(set, "content-disposition");
    if (disp != NULL && GetValue(disp, "name=", &ks, &ke, &unescape) == NS_TRUE) {
	const char *key = Ext2utf(&kds, ks, (size_t)(ke - ks), encoding, unescape);
	const char *value, *fs = NULL, *fe = NULL;

        if (GetValue(disp, "filename=", &fs, &fe, &unescape) == NS_FALSE) {
	    value = Ext2utf(&vds, start, (size_t)(end - start), encoding, unescape);
        } else {
	    Tcl_HashEntry *hPtr;
            FormFile      *filePtr;
            Tcl_Interp    *interp = connPtr->itPtr->interp;

            value = Ext2utf(&vds, fs, (size_t)(fe - fs), encoding, unescape);
            hPtr = Tcl_CreateHashEntry(&connPtr->files, key, &isNew);
            if (isNew != 0) {
                
                filePtr = ns_malloc(sizeof(FormFile));
                Tcl_SetHashValue(hPtr, filePtr);
                
                filePtr->hdrObj = Tcl_NewListObj(1, NULL);
                filePtr->offObj = Tcl_NewListObj(1, NULL);
                filePtr->sizeObj = Tcl_NewListObj(1, NULL);
                
                Tcl_IncrRefCount(filePtr->hdrObj);
                Tcl_IncrRefCount(filePtr->offObj);
                Tcl_IncrRefCount(filePtr->sizeObj);
            } else {
                filePtr = Tcl_GetHashValue(hPtr);
            }

            Ns_TclEnterSet(interp, set, NS_TCL_SET_DYNAMIC);
            Tcl_ListObjAppendElement(interp, filePtr->hdrObj,
                                     Tcl_GetObjResult(interp));
            Tcl_ResetResult(connPtr->itPtr->interp);
                
            Tcl_ListObjAppendElement(interp, filePtr->offObj,
                                     Tcl_NewIntObj((int)(start - connPtr->reqPtr->content)));
            
            Tcl_ListObjAppendElement(interp, filePtr->sizeObj,
                                     Tcl_NewWideIntObj((Tcl_WideInt)(end - start)));
            set = NULL;
        }
        (void) Ns_SetPut(connPtr->query, key, value);
    }

    /*
     * Restore the end marker.
     */

    *end = saveend;
    Tcl_DStringFree(&kds);
    Tcl_DStringFree(&vds);
    if (set != NULL) {
        Ns_SetFree(set);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * GetBoundary --
 *
 *      Copy multipart/form-data boundy string, if any.
 *
 * Results:
 *      1 if boundy copied, 0 otherwise.
 *
 * Side effects:
 *      Copies boundry string to given dstring.
 *
 *----------------------------------------------------------------------
 */

static int
GetBoundary(Tcl_DString *dsPtr, const char *contentType)
{
    const char *bs;

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
        return 1;
    }
    return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * NextBoundary --
 *
 *      Locate the next form boundry.
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
NextBoundry(const Tcl_DString *dsPtr, char *s, const char *e)
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
    const char *s, *e;

    NS_NONNULL_ASSERT(hdr != NULL);
    NS_NONNULL_ASSERT(att != NULL);
    NS_NONNULL_ASSERT(vsPtr != NULL);
    NS_NONNULL_ASSERT(vePtr != NULL);
    NS_NONNULL_ASSERT(uPtr != NULL);

    s = Ns_StrCaseFind(hdr, att);
    if (s == NULL) {
        return NS_FALSE;
    }
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
        while (*e != '\0' && (escaped == NS_TRUE || *e != *s)) {
	    if (escaped == NS_TRUE) {
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

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ext2utf --
 *
 *      Convert input string to UTF.
 *
 * Results:
 *      Pointer to converted string.
 *
 * Side effects:
 *      Converted string is copied to given dstring, overwriting
 *      any previous content.
 *
 *----------------------------------------------------------------------
 */

static char *
Ext2utf(Tcl_DString *dsPtr, const char *start, size_t len, Tcl_Encoding encoding, char unescape)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(start != NULL);

    if (encoding == NULL) {
        Tcl_DStringSetLength(dsPtr, 0);
        Tcl_DStringAppend(dsPtr, start, (int)len);
    } else {
        /* 
         * ExternalToUtfDString will re-init dstring. 
         */
        Tcl_DStringFree(dsPtr);
        (void) Tcl_ExternalToUtfDString(encoding, start, (int)len, dsPtr);
    }

    /*
     * In case the string contains backslash escaped characters, the
     * backslashes have to be removed. This will shorten the resulting
     * string.
     */
    if (unescape != '\0') {
      int i, j, l = (int)len;
      char *buffer = dsPtr->string;

      for (i = 0; i<l; i++) {
	if (buffer[i] == '\\' && buffer[i+1] == unescape) {
	  for (j = i; j < l; j++) {
	    buffer[j] = buffer[j+1];
	  }
	  l --;
	}
      }
      Tcl_DStringSetLength(dsPtr, l);
    }
    return dsPtr->string;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
