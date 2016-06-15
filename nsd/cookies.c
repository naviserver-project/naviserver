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
 * cookies.c --
 *
 *      Routines to manipulate HTTP cookie headers.
 *
 */

#include "nsd.h"

/*
 * Local functions defined in this file.
 */

static Ns_Conn *GetConn(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1);

static int SearchFirstCookie(Ns_DString *dest, const Ns_Set *hdrs, const char *setName, const char *name) 
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static int DeleteNamedCookies(Ns_Set *hdrs, const char *setName, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);


typedef char* (CookieParser)(Ns_DString *dest, char *chars, const char *name, size_t nameLen)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static CookieParser GetFromCookieHeader;
static CookieParser GetFromSetCookieHeader;

static void CopyCookieValue(Tcl_DString *dest, char *valueStart)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 *----------------------------------------------------------------------
 *
 * CopyCookieValue --
 *
 *      Copy the CookieValue into the provided Tcl_DString
 *
 * Results: 
 *      None
 *
 * Side effects:
 *      Append to provided Tcl_DString
 *
 *----------------------------------------------------------------------
 */

static void
CopyCookieValue(Tcl_DString *dest, char *valueStart)
{
    char save, *q;

    NS_NONNULL_ASSERT(dest != NULL);
    NS_NONNULL_ASSERT(valueStart != NULL);
                    
    if (*valueStart == '"') {
        ++valueStart; /* advance past optional quote mark */
    }
    q = valueStart;
    while (*q != '"' && *q != ';' && *q != '\0') {
        ++q;
    }
    save = *q;
    *q = '\0';
    Ns_UrlQueryDecode(dest, valueStart, NULL);
    *q = save;
}


/*
 *----------------------------------------------------------------------
 *
 * GetFromCookieHeader --
 *
 *      Get a cookie from the cookie header. The cookie header field has a
 *      content of the form:
 *
 *         cookie1="value1"; cookie2="value2"; style=null; ...
 *
 *      so we have to iterate over the cookie/value pairs separated with
 *      semicolons.
 *
 * Results: 
 *      On success a non-null value pointing the the begin of the found
 *      cookie such we can iterate to search for more cookies with the same
 *      name
 *
 * Side effects:
 *      When Tcl_DString dest is provided, the value of the cookie is
 *      appended to the DString.
 *
 *----------------------------------------------------------------------
 */

static char *
GetFromCookieHeader(Ns_DString *dest, char *chars, const char *name, size_t nameLen)
{
    char *cookieStart = NULL, *p = chars;

    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    
    for ( ; likely(*p != '\0'); ) {
        /*
         * Skip optional white space.
         */
        for (; (CHARTYPE(space, *p) != 0); p++) {
        }
        if (*p == '\0') {
            break;
        }
        
        if (strncmp(p, name, nameLen) == 0) {
            char *q = p + nameLen;

            /*
             * Name starts correctly
             */
            if (likely(*q == '=')) {
                /*
                 * Full match, we found the cookie
                 */
                cookieStart = p;
                if (dest != NULL) {
                    q++; /* advance past equals sign */
                    CopyCookieValue(dest, q);
                }
                break;
            }
        }
        /* 
         * Look for the next semicolon
         */
        for (; (*p != '\0') && (*p != ';'); p++) {
            ;
        }
        /*
         * We found a semicolon and skip it;
         */
        if (*p == ';') {
            p++;
        }
    }

    return cookieStart;
}


/*
 *----------------------------------------------------------------------
 *
 * GetFromSetCookieHeader --
 *
 *      Get a cookie from the set-cookie header. The set-cookie header field has a
 *      content of the form:
 *
 *         cookie1="new-value"; Expires=Fri, 01-Jan-2035 01:00:00 GMT; Path=/; HttpOnly
 *
 *      In order to get the cookie-value, the entry has to start with a
 *      name/value pair.
 *
 * Results: 
 *      On success a non-null value pointing the the begin of the found
 *      cookie
 *
 * Side effects:
 *      When Tcl_DString dest is provided, the value of the cookie is
 *      appended to the DString.
 *
 *----------------------------------------------------------------------
 */

static char *
GetFromSetCookieHeader(Ns_DString *dest, char *chars, const char *name, size_t nameLen) {
    char *cookieStart = NULL, *p = chars;

    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    /*
     * Skip white space (should not be needed).
     */
    for (; (CHARTYPE(space, *p) != 0); p++) {
        ;
    }
    if (strncmp(p, name, nameLen) == 0) {
        char *q = p + nameLen;
        
        /*
         * Name starts correctly
         */
            
        if (*q == '=') {
            /*
             * Full match, we found the cookie
             */
            cookieStart = p;
            if (dest != NULL) {
                q++; /* advance past equals sign */
                CopyCookieValue(dest, q);
            }
        }
    }

    return cookieStart;
}


/*
 *----------------------------------------------------------------------
 *
 * SearchFirstCookie --
 *
 *      Search for a coockie with the given name in the given set and
 *      return the first hit.
 *
 * Results:
 *      index value on success, or -1
 *
 * Side effects:
 *      when NsString dest is provided, the value of the cookie is
 *      appended to the DString.
 *
 *----------------------------------------------------------------------
 */

static int 
SearchFirstCookie(Ns_DString *dest, const Ns_Set *hdrs, const char *setName, const char *name) 
{
    int      index = -1;
    size_t   nameLen, i;
    CookieParser *cookieParser;

    NS_NONNULL_ASSERT(hdrs != NULL);
    NS_NONNULL_ASSERT(setName != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    nameLen = strlen(name);

    cookieParser = (*setName == 'c') ? GetFromCookieHeader : GetFromSetCookieHeader;
    
    for (i = 0u; i < hdrs->size; ++i) {
        if (strcasecmp(hdrs->fields[i].name, setName) == 0) {
            /*
             * We have the right header.
             */
            if ((*cookieParser)(dest, hdrs->fields[i].value, name, nameLen) != NULL) {
                /*
                 * We found the result.
                 */
                index = (int) i;
                break;
            }
        }
    }

    return index;
}


/*
 *----------------------------------------------------------------------
 *
 * DeleteNamedCookies --
 *
 *      Delete all cookies with the specified name form the given set.
 *
 * Results:
 *      Boolean value.
 *
 * Side effects:
 *      Delete nsset entry.
 *
 *----------------------------------------------------------------------
 */

static int
DeleteNamedCookies(Ns_Set *hdrs, const char *setName, const char *name)
{
    int success = 0;

    NS_NONNULL_ASSERT(hdrs != NULL);
    NS_NONNULL_ASSERT(setName != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    for (;;) {
	int idx = SearchFirstCookie(NULL, hdrs, setName, name);
	if (idx != -1) {
	    Ns_SetDelete(hdrs, idx);
	    success = 1;
	} else {
	    break;
	}
    }
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnSetCookie, Ns_ConnSetCookieEx, Ns_ConnSetSecureCookie --
 *
 *      Set a cookie for the given connection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Existing cookie with same name, path, domain will be dropped
 *      by client.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnSetCookieEx(const Ns_Conn *conn, const char *name, const char *value, time_t maxage,
                   const char *domain, const char *path, unsigned int flags)
{
    Ns_DString  cookie;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    
    if ((flags & NS_COOKIE_REPLACE) != 0u) {
	(void)DeleteNamedCookies(Ns_ConnOutputHeaders(conn), "set-cookie", name);
    }

    Ns_DStringInit(&cookie);
    Ns_DStringVarAppend(&cookie, name, "=\"", NULL);
    if (value != NULL) {
        Ns_UrlQueryEncode(&cookie, value, NULL);
    }
    Ns_DStringAppend(&cookie, "\"");
    if ((flags & NS_COOKIE_EXPIRENOW) != 0u) {
        Ns_DStringAppend(&cookie, "; Expires=Fri, 01-Jan-1980 01:00:00 GMT");
    } else if (maxage == TIME_T_MAX) {
        Ns_DStringAppend(&cookie, "; Expires=Fri, 01-Jan-2035 01:00:00 GMT");
    } else if (maxage > 0) {
        Ns_DStringPrintf(&cookie, "; Max-Age=%ld", maxage);
    } else {
	/* 
	 * maxage == 0, don't specify any expiry
	 */
    }
    /* ignore empty domain, since IE rejects it */
    if (domain != NULL && *domain != '\0') {
        Ns_DStringVarAppend(&cookie, "; Domain=", domain, NULL);
    }
    if (path != NULL) {
        Ns_DStringVarAppend(&cookie, "; Path=", path, NULL);
    }
    if ((flags & NS_COOKIE_SECURE) != 0u) {
        Ns_DStringAppend(&cookie, "; Secure");
    }
    if ((flags & NS_COOKIE_DISCARD) != 0u) {
        Ns_DStringAppend(&cookie, "; Discard");
    }
    if ((flags & NS_COOKIE_SCRIPTABLE) == 0u) {
        Ns_DStringAppend(&cookie, "; HttpOnly");
    }

    Ns_ConnSetHeaders(conn, "Set-Cookie", cookie.string);
    Ns_DStringFree(&cookie);
}

void
Ns_ConnSetCookie(const Ns_Conn *conn, const char *name, const char *value, time_t maxage)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    
    Ns_ConnSetCookieEx(conn, name, value, maxage, NULL, NULL, 0u);
}

void
Ns_ConnSetSecureCookie(const Ns_Conn *conn, const char *name, const char *value, time_t maxage)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    Ns_ConnSetCookieEx(conn, name, value, maxage, NULL, NULL, NS_COOKIE_SECURE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnDeleteCookie, Ns_ConnDeleteSecureCookie --
 *
 *      Expire immediately the cookie with matching name, domain and path.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ConnDeleteCookie(const Ns_Conn *conn, const char *name, const char *domain, const char *path)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    
    Ns_ConnSetCookieEx(conn, name, NULL, (time_t)0, domain, path, NS_COOKIE_EXPIRENOW);
}

void
Ns_ConnDeleteSecureCookie(const Ns_Conn *conn, const char *name, const char *domain, const char *path)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    
    Ns_ConnSetCookieEx(conn, name, NULL, (time_t)0, domain, path, NS_COOKIE_EXPIRENOW|NS_COOKIE_SECURE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnGetCookie --
 *
 *      Get first matching cookie for the given connection.
 *
 * Results:
 *      dest->string or NULL on error.
 *
 * Side effects:
 *      Cookie value is UrlQueryDecoded before placement in dest.
 *
 *----------------------------------------------------------------------
 */


char *
Ns_ConnGetCookie(Ns_DString *dest, const Ns_Conn *conn, const char *name)
{
    int idx;

    NS_NONNULL_ASSERT(dest != NULL);
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);
      
    idx = SearchFirstCookie(dest, Ns_ConnHeaders(conn), "cookie", name);
    
    return idx != -1 ? Ns_DStringValue(dest) : NULL;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclSetCookieObjCmd --
 *
 *      Implements the ns_setcookie command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSetCookieObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn     *conn = GetConn(interp);
    const char  *name, *data, *domain = NULL, *path = NULL;
    int          secure = 0, scriptable = 0, discard = 0, replace = 0;
    unsigned int flags = 0u;
    time_t       maxage;
    Ns_Time     *expiresPtr = NULL;

    Ns_ObjvSpec opts[] = {
        {"-discard",    Ns_ObjvBool,   &discard,    NULL},
        {"-replace",    Ns_ObjvBool,   &replace,    NULL},
        {"-secure",     Ns_ObjvBool,   &secure,     NULL},
        {"-scriptable", Ns_ObjvBool,   &scriptable, NULL},
        {"-domain",     Ns_ObjvString, &domain,     NULL},
        {"-path",       Ns_ObjvString, &path,       NULL},
        {"-expires",    Ns_ObjvTime,   &expiresPtr, NULL},
        {"--",          Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {"data", Ns_ObjvString, &data, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (conn == NULL || Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (secure != 0) {
        flags |= NS_COOKIE_SECURE;
    }
    if (scriptable != 0) {
        flags |= NS_COOKIE_SCRIPTABLE;
    }
    if (discard != 0) {
        flags |= NS_COOKIE_DISCARD;
    }
    if (replace != 0) {
        flags |= NS_COOKIE_REPLACE;
    }

    /*
     * Accept expiry time as relative or absolute and adjust to the relative
     * time Ns_ConnSetCookieEx expects, taking account of the special value
     * -1 which is short hand for infinite.
     */

    if (expiresPtr != NULL) {
        Ns_Time *nowPtr = Ns_ConnStartTime(conn); /* Approximately now... */
        if (expiresPtr->sec < 0) {
            maxage = TIME_T_MAX;
        } else if (expiresPtr->sec > nowPtr->sec) {
            maxage = (time_t)expiresPtr->sec - (time_t)nowPtr->sec;
        } else {
            maxage = expiresPtr->sec;
        }
    } else {
        maxage = 0;
    }

    Ns_ConnSetCookieEx(conn, name, data, maxage, domain, path, flags);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetCookieObjCmd --
 *
 *      Implements the ns_getcookie command.  The given default will be
 *      returned if no matching cookie exists.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclGetCookieObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn     *conn;
    Ns_DString   ds;
    const char  *nameString;
    Tcl_Obj     *defaultObj = NULL;
    int          idx = -1, status = TCL_OK;
    int          withSetCookies = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-include_set_cookies", Ns_ObjvBool,  &withSetCookies, NULL},
        {"--",                   Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"name",      Ns_ObjvString, &nameString,  NULL},
        {"?default",  Ns_ObjvObj,    &defaultObj,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    
    conn = GetConn(interp);
    if (unlikely(conn == NULL)) {
        return TCL_ERROR;
    }

    Ns_DStringInit(&ds);

    if (withSetCookies == NS_TRUE) {
	idx = SearchFirstCookie(&ds, Ns_ConnOutputHeaders(conn), "set-cookie", nameString);
    }
    if (idx == -1) {
	idx = SearchFirstCookie(&ds, Ns_ConnHeaders(conn), "cookie", nameString);
    }
    
    if (idx != -1) {
        Tcl_DStringResult(interp, &ds);
    } else if (defaultObj != NULL) {
        Tcl_SetObjResult(interp, defaultObj);
    } else {
        Tcl_SetResult(interp, "no matching cookie", TCL_STATIC);
        status = TCL_ERROR;
    }
    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclDeleteCookieObjCmd --
 *
 *      Implements the ns_deletecookie command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See Ns_ConnDeleteCookie().
 *
 *----------------------------------------------------------------------
 */

int
NsTclDeleteCookieObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_Conn     *conn = GetConn(interp);
    const char  *name, *domain = NULL, *path = NULL;
    unsigned int flags = NS_COOKIE_SCRIPTABLE;
    int          secure = 0, replace = 0;

    Ns_ObjvSpec  opts[] = {
        {"-secure",  Ns_ObjvBool,   &secure,  NULL},
        {"-domain",  Ns_ObjvString, &domain,  NULL},
        {"-path",    Ns_ObjvString, &path,    NULL},
        {"-replace", Ns_ObjvBool,   &replace, NULL},
        {"--",       Ns_ObjvBreak,  NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"name",  Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (conn == NULL || Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (replace != 0) {
        flags |= NS_COOKIE_REPLACE;
    }
    if (secure != 0) {
        flags |= NS_COOKIE_SECURE;
    }

    Ns_ConnSetCookieEx(conn, name, NULL, (time_t)0, domain, path, NS_COOKIE_EXPIRENOW|flags);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * GetConn --
 *
 *      Return the conn for the given interp, logging an error if
 *      not available.
 *
 * Results:
 *      Ns_Conn pointer or NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_Conn *
GetConn(Tcl_Interp *interp)
{
    Ns_Conn *conn;

    NS_NONNULL_ASSERT(interp != NULL);
    
    conn = Ns_TclGetConn(interp);
    if (conn == NULL) {
        Tcl_SetResult(interp, "No connection available.", TCL_STATIC);
    }

    return conn;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
