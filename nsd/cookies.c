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

static int GetFirstNamedCookie(Ns_DString *dest, const Ns_Set *hdrs,
                               const char *setName, const char *name)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static int GetAllNamedCookies(Ns_DString *dest, const Ns_Set *hdrs,
                              const char *setName, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static bool DeleteNamedCookies(Ns_Set *hdrs, const char *setName,
                               const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);


typedef char* (CookieParser)(Ns_DString *dest, char *chars, const char *name,
                             size_t nameLen, char **nextPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static CookieParser GetFromCookieHeader;
static CookieParser GetFromSetCookieHeader;

static char *CopyCookieValue(Tcl_DString *dest, char *valueStart)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ObjvTable samesiteValues[] = {
    {"strict", UCHAR('s')},
    {"lax",    UCHAR('l')},
    {"none",   UCHAR('n')},
    {NULL,    0u}
};



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

static char *
CopyCookieValue(Tcl_DString *dest, char *valueStart)
{
    char save, *q;

    NS_NONNULL_ASSERT(dest != NULL);
    NS_NONNULL_ASSERT(valueStart != NULL);

    if (*valueStart == '"') {
        /*
         * Advance past optional quote.
         */
        ++valueStart;
    }
    q = valueStart;
    while (*q != '"' && *q != ';' && *q != '\0') {
        ++q;
    }
    save = *q;
    *q = '\0';
    Ns_CookieDecode(dest, valueStart, NULL);
    *q = save;

    /*
     * Advance past delimiter.
     */
    while (*q == '"' || *q == ';') {
        q++;
    }

    return q;
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
 *      On success a non-null value pointing the begin of the found
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
GetFromCookieHeader(Ns_DString *dest, char *chars, const char *name,
                    size_t nameLen, char **nextPtr)
{
    char *cookieStart = NULL, *toParse = chars;

    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    for ( ; likely(*toParse != '\0'); ) {
        /*
         * Skip optional white space.
         */
        for (; (CHARTYPE(space, *toParse) != 0); toParse++) {
        }
        if (*toParse == '\0') {
            break;
        }

        if (strncmp(toParse, name, nameLen) == 0) {
            char *q = toParse + nameLen;

            /*
             * Name starts correctly
             */
            if (likely(*q == '=')) {
                /*
                 * Full match, we found the cookie
                 */
                cookieStart = toParse;
                q++; /* advance past equals sign */
                if (dest != NULL) {
                    q = CopyCookieValue(dest, q);
                }
                toParse = q;
                break;
            }
        }
        /*
         * Look for the next semicolon
         */
        for (; (*toParse != '\0') && (*toParse != ';'); toParse++) {
            ;
        }
        if (*toParse == ';') {
            /*
             * We found a semicolon and skip it;
             */
            toParse++;
        }
    }

    if (nextPtr != NULL) {
        *nextPtr = toParse;
    }

    return cookieStart;
}


/*
 *----------------------------------------------------------------------
 *
 * GetFromSetCookieHeader --
 *
 *      Get a cookie from the set-cookie header. The set-cookie header field
 *      has a content of the form:
 *
 *         cookie1="new-value"; Expires=Fri, 01-Jan-2035 01:00:00 GMT; Path=/; HttpOnly
 *
 *      In order to get the cookie-value, the entry has to start with a
 *      name/value pair.
 *
 * Results:
 *      On success a non-null value pointing the begin of the found
 *      cookie
 *
 * Side effects:
 *      When Tcl_DString dest is provided, the value of the cookie is
 *      appended to the DString.
 *
 *----------------------------------------------------------------------
 */

static char *
GetFromSetCookieHeader(Ns_DString *dest, char *chars, const char *name,
                       size_t nameLen, char **nextPtr) {
    char *cookieStart = NULL, *toParse = chars;

    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    /*
     * Skip white space (should not be needed).
     */
    for (; (CHARTYPE(space, *toParse) != 0); toParse++) {
        ;
    }
    if (strncmp(toParse, name, nameLen) == 0) {
        char *q = toParse + nameLen;

        /*
         * Name starts correctly
         */

        if (*q == '=') {
            /*
             * Full match, we found the cookie
             */
            cookieStart = toParse;
            q++; /* advance past equals sign */
            if (dest != NULL) {
                q = CopyCookieValue(dest, q);
            }
            toParse = q;
        }
    }
    if (nextPtr != NULL) {
        *nextPtr = toParse;
    }

    return cookieStart;
}


/*
 *----------------------------------------------------------------------
 *
 * GetFirstNamedCookie --
 *
 *      Search for a cookie with the given name in the given set and
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
GetFirstNamedCookie(Ns_DString *dest, const Ns_Set *hdrs, const char *setName,
                    const char *name)
{
    int      idx = -1;
    size_t   nameLen, i;
    CookieParser *cookieParser;

    NS_NONNULL_ASSERT(hdrs != NULL);
    NS_NONNULL_ASSERT(setName != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    nameLen = strlen(name);

    cookieParser = (*setName == 'c')
        ? GetFromCookieHeader
        : GetFromSetCookieHeader;

    for (i = 0u; i < hdrs->size; ++i) {
        if (strcasecmp(hdrs->fields[i].name, setName) == 0) {
            /*
             * We have the right header.
             */
            if ((*cookieParser)(dest, hdrs->fields[i].value, name,
                                nameLen, NULL) != NULL) {
                /*
                 * We found the result.
                 */
                idx = (int) i;
                break;
            }
        }
    }

    return idx;
}

/*
 *----------------------------------------------------------------------
 *
 * GetAllNamedCookies --
 *
 *      Search for a cookie with the given name in the given set and
 *      return all hits.
 *
 * Results:
 *      Number of cookies with the given name
 *
 * Side effects:
 *      Update the first argument with a list of cookie values
 *
 *----------------------------------------------------------------------
 */
static int
GetAllNamedCookies(Ns_DString *dest, const Ns_Set *hdrs, const char *setName,
                   const char *name)
{
    int           count = 0;
    size_t        nameLen, i;
    CookieParser *cookieParser;

    NS_NONNULL_ASSERT(dest != NULL);
    NS_NONNULL_ASSERT(hdrs != NULL);
    NS_NONNULL_ASSERT(setName != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    nameLen = strlen(name);
    cookieParser = (*setName == 'c')
        ? GetFromCookieHeader
        : GetFromSetCookieHeader;

    for (i = 0u; i < hdrs->size; i++) {
        if (strcasecmp(hdrs->fields[i].name, setName) == 0) {
            char *toParse;

            /*
             * We have the right header, parse the string;
             */
            for (toParse = hdrs->fields[i].value; *toParse != '\0'; ) {
                Ns_DString cookie;

                Ns_DStringInit(&cookie);
                if ((*cookieParser)(&cookie, toParse, name, nameLen,
                                    &toParse) != NULL) {
                    /*
                     * We found the named cookie;
                     */
                    count ++;
                    Tcl_DStringAppendElement(dest, cookie.string);
                }
                Ns_DStringFree(&cookie);
            }
            break;
        }
    }

    return count;
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

static bool
DeleteNamedCookies(Ns_Set *hdrs, const char *setName, const char *name)
{
    bool success = NS_FALSE;

    NS_NONNULL_ASSERT(hdrs != NULL);
    NS_NONNULL_ASSERT(setName != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    for (;;) {
        int idx = GetFirstNamedCookie(NULL, hdrs, setName, name);
        if (idx != -1) {
            Ns_SetDelete(hdrs, idx);
            success = NS_TRUE;
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
Ns_ConnSetCookieEx(const Ns_Conn *conn, const char *name, const char *value,
                   time_t maxage, const char *domain, const char *path,
                   unsigned int flags)
{
    Ns_DString  cookie;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    if ((flags & NS_COOKIE_REPLACE) != 0u) {
        (void)DeleteNamedCookies(Ns_ConnOutputHeaders(conn), "set-cookie",
                                 name);
    }

    Ns_DStringInit(&cookie);
    Ns_DStringVarAppend(&cookie, name, "=\"", (char *)0L);
    if (value != NULL) {
        Ns_CookieEncode(&cookie, value, NULL);
    }
    Ns_DStringAppend(&cookie, "\"");
    if ((flags & NS_COOKIE_EXPIRENOW) != 0u) {
        Ns_DStringAppend(&cookie, "; Expires=Fri, 01-Jan-1980 01:00:00 GMT");
    } else if (maxage == TIME_T_MAX) {
        Ns_DStringAppend(&cookie, "; Expires=Fri, 01-Jan-2035 01:00:00 GMT");
    } else if (maxage > 0) {
        Ns_DStringPrintf(&cookie, "; Max-Age=%" PRId64, (int64_t)maxage);
    } else {
        /*
         * maxage == 0, don't specify any expiry
         */
    }
    /* ignore empty domain, since IE rejects it */
    if (domain != NULL && *domain != '\0') {
        Ns_DStringVarAppend(&cookie, "; Domain=", domain, (char *)0L);
    }
    if (path != NULL) {
        Ns_DStringVarAppend(&cookie, "; Path=", path, (char *)0L);
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

    if ((flags & NS_COOKIE_SAMESITE_STRICT) != 0u) {
        Ns_DStringAppend(&cookie, "; SameSite=Strict");
    } else if ((flags & NS_COOKIE_SAMESITE_LAX) != 0u) {
        Ns_DStringAppend(&cookie, "; SameSite=Lax");
    } else if ((flags & NS_COOKIE_SAMESITE_NONE) != 0u) {
        Ns_DStringAppend(&cookie, "; SameSite=None");
    }


    Ns_ConnSetHeaders(conn, "Set-Cookie", cookie.string);
    Ns_DStringFree(&cookie);
}

void
Ns_ConnSetCookie(const Ns_Conn *conn, const char *name, const char *value,
                 time_t maxage)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    Ns_ConnSetCookieEx(conn, name, value, maxage, NULL, NULL, 0u);
}

void
Ns_ConnSetSecureCookie(const Ns_Conn *conn, const char *name, const char *value,
                       time_t maxage)
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
Ns_ConnDeleteCookie(const Ns_Conn *conn, const char *name, const char *domain,
                    const char *path)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    Ns_ConnSetCookieEx(conn, name, NULL, (time_t)0, domain, path,
                       NS_COOKIE_EXPIRENOW);
}

void
Ns_ConnDeleteSecureCookie(const Ns_Conn *conn, const char *name,
                          const char *domain, const char *path)
{
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    Ns_ConnSetCookieEx(conn, name, NULL, (time_t)0, domain, path,
                       NS_COOKIE_EXPIRENOW|NS_COOKIE_SECURE);
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
 *      Cookie value is CookieDecoded before placement in dest.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ConnGetCookie(Ns_DString *dest, const Ns_Conn *conn, const char *name)
{
    int idx;

    NS_NONNULL_ASSERT(dest != NULL);
    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    idx = GetFirstNamedCookie(dest, Ns_ConnHeaders(conn), "cookie", name);

    return idx != -1 ? Ns_DStringValue(dest) : NULL;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclSetCookieObjCmd --
 *
 *      Implements "ns_setcookie".
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
NsTclSetCookieObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                     int objc, Tcl_Obj *const* objv)
{
    Ns_Conn       *conn;
    char          *name, *data, *domain = NULL, *path = NULL;
    int            secure = 0, scriptable = 0, discard = 0, replace = 0, result;
    int            samesite = INTCHAR('l');
    Ns_Time       *expiresPtr = NULL;
    Ns_ObjvSpec    opts[] = {
        {"-discard",    Ns_ObjvBool,   &discard,    NULL},
        {"-domain",     Ns_ObjvString, &domain,     NULL},
        {"-expires",    Ns_ObjvTime,   &expiresPtr, NULL},
        {"-path",       Ns_ObjvString, &path,       NULL},
        {"-replace",    Ns_ObjvBool,   &replace,    NULL},
        {"-samesite",   Ns_ObjvIndex,  &samesite,   samesiteValues},
        {"-scriptable", Ns_ObjvBool,   &scriptable, NULL},
        {"-secure",     Ns_ObjvBool,   &secure,     NULL},
        {"--",          Ns_ObjvBreak,  NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"name", Ns_ObjvString, &name, NULL},
        {"data", Ns_ObjvString, &data, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, NS_CONN_REQUIRE_CONFIGURED, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int   flags = 0u;
        time_t         maxage;

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
         * If "-samesite none" flag was provided, and secure was not set, fall
         * back to "-samesite lax" and complain.
         */
        if (samesite == INTCHAR('n') && secure == 0) {
            Ns_Log(Warning, "cookie '%s': trying to set '-samesite none' "
                   "without the '-secure' flag. Fall back to -samesite lax", name);
            samesite = INTCHAR('l');
        }
        if (samesite == INTCHAR('s')) {
            flags |= NS_COOKIE_SAMESITE_STRICT;
        } else if (samesite == INTCHAR('l')) {
            flags |= NS_COOKIE_SAMESITE_LAX;
        } else if (samesite == INTCHAR('n')) {
            flags |= NS_COOKIE_SAMESITE_NONE;
        }

        /*
         * Accept expiry time as relative or absolute and adjust to the relative
         * time Ns_ConnSetCookieEx expects, taking account of the special value
         * -1 which is short hand for infinite.
         */

        if (expiresPtr != NULL) {
            /*
             * The start time is close enough to "now"
             */
            const Ns_Time *nowPtr = Ns_ConnStartTime(conn);
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
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGetCookieObjCmd --
 *
 *      Implements "ns_getcookie".  The given default will be
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
NsTclGetCookieObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                     int objc, Tcl_Obj *const* objv)
{
    Ns_Conn       *conn;
    char          *nameString;
    Tcl_Obj       *defaultObj = NULL;
    int            status = TCL_OK;
    int            withSetCookies = (int)NS_FALSE, withAll = (int)NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-all",                 Ns_ObjvBool,  &withAll, NULL},
        {"-include_set_cookies", Ns_ObjvBool,  &withSetCookies, NULL},
        {"--",                   Ns_ObjvBreak, NULL, NULL},
        {NULL, NULL,  NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"name",      Ns_ObjvString, &nameString,  NULL},
        {"?default",  Ns_ObjvObj,    &defaultObj,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, NS_CONN_REQUIRE_CONFIGURED, &conn) != NS_OK) {
        status = TCL_ERROR;

    } else if (withSetCookies == (int)NS_TRUE && withAll == (int)NS_TRUE) {
        Ns_TclPrintfResult(interp, "%s", "invalid combination of flags -include_set_cookies and -all");
        status = TCL_ERROR;

    } else {
        Ns_DString     ds;
        int            idx = -1;

        Ns_DStringInit(&ds);

        if (withAll == (int)NS_TRUE) {
            idx = GetAllNamedCookies(&ds, Ns_ConnHeaders(conn),
                                     "cookie", nameString);

        } else {
            if (withSetCookies == (int)NS_TRUE) {
                idx = GetFirstNamedCookie(&ds, Ns_ConnOutputHeaders(conn),
                                          "set-cookie", nameString);
            }
            if (idx == -1) {
                idx = GetFirstNamedCookie(&ds, Ns_ConnHeaders(conn),
                                          "cookie", nameString);
            }
        }

        if (idx != -1) {
            Tcl_DStringResult(interp, &ds);
        } else if (defaultObj != NULL) {
            Tcl_SetObjResult(interp, defaultObj);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("no such cookie", -1));
            status = TCL_ERROR;
        }
        Ns_DStringFree(&ds);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclDeleteCookieObjCmd --
 *
 *      Implements "ns_deletecookie".
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
NsTclDeleteCookieObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                        int objc, Tcl_Obj *const* objv)
{
    Ns_Conn        *conn;
    char           *name, *domain = NULL, *path = NULL;
    int             secure = 0, replace = 0, result;
    int             samesite = INTCHAR('l');
    Ns_ObjvSpec     opts[] = {
        {"-domain",  Ns_ObjvString, &domain,   NULL},
        {"-path",    Ns_ObjvString, &path,     NULL},
        {"-replace", Ns_ObjvBool,   &replace,  NULL},
        {"-samesite",Ns_ObjvIndex,  &samesite, samesiteValues},
        {"-secure",  Ns_ObjvBool,   &secure,   NULL},
        {"--",       Ns_ObjvBreak,  NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"name",  Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK
        || NsConnRequire(interp, NS_CONN_REQUIRE_CONFIGURED, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else {
        unsigned int flags = 0u;

        if (replace != 0) {
            flags |= NS_COOKIE_REPLACE;
        }
        if (secure != 0) {
            flags |= NS_COOKIE_SECURE;
        }

        /*
         * If "-samesite none" flag was provided, and secure was not set, fall
         * back to "-samesite lax" and complain.
         */
        if (samesite == INTCHAR('n') && secure == 0) {
            Ns_Log(Warning, "cookie '%s': trying to set '-samesite none' "
                   "without the '-secure' flag. Fall back to -samesite lax", name);
            samesite = INTCHAR('l');
        }

        if (samesite == INTCHAR('s')) {
            flags |= NS_COOKIE_SAMESITE_STRICT;
        } else if (samesite == INTCHAR('l')) {
            flags |= NS_COOKIE_SAMESITE_LAX;
        } else if (samesite == INTCHAR('n')) {
            flags |= NS_COOKIE_SAMESITE_NONE;
        }

        Ns_ConnSetCookieEx(conn, name, NULL, (time_t)0, domain, path,
                           NS_COOKIE_EXPIRENOW|flags);
        result = TCL_OK;
    }

    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
