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

static const char *RCSID =
    "@(#) $Header$, compiled: " __DATE__ " " __TIME__;


#include "nsd.h"


/*
 * Local functions defined in this file.
 */

static Ns_Conn *GetConn(Tcl_Interp *interp);



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
Ns_ConnSetCookieEx(Ns_Conn *conn,  char *name, char *value, int maxage,
                   char *domain, char *path, int secure)
{
    Ns_DString  cookie;
    Ns_Set     *headers;

    Ns_DStringInit(&cookie);
    Ns_DStringVarAppend(&cookie, name, "=\"", NULL);
    if (value != NULL) {
        Ns_UrlQueryEncode(&cookie, value, NULL);
    }
    Ns_DStringAppend(&cookie, "\"");
    if (maxage == INT_MAX) {
        Ns_DStringAppend(&cookie, "; Expires=Fri, 01-Jan-2035 01:00:00 GMT");
    } else if (maxage > 0) {
        Ns_DStringPrintf(&cookie, "; Max-Age=%d", maxage);
    } else if (maxage < 0) {
        Ns_DStringAppend(&cookie, "; Expires=Fri, 01-Jan-1980 01:00:00 GMT");
    }
    if (domain != NULL) {
        Ns_DStringVarAppend(&cookie, "; Domain=", domain, NULL);
    }
    if (path != NULL) {
        Ns_DStringVarAppend(&cookie, "; Path=", domain, NULL);
    }
    if (secure == NS_TRUE) {
        Ns_DStringAppend(&cookie, "; Secure");
    }
    headers = Ns_ConnOutputHeaders(conn);
    Ns_SetPut(headers, "Set-Cookie", cookie.string);
    Ns_DStringFree(&cookie);
}

void
Ns_ConnSetCookie(Ns_Conn *conn,  char *name, char *value, int maxage)
{
    Ns_ConnSetCookieEx(conn, name, value, maxage, NULL, NULL, NS_FALSE);
}

void
Ns_ConnSetSecureCookie(Ns_Conn *conn,  char *name, char *value, int maxage)
{
    Ns_ConnSetCookieEx(conn, name, value, maxage, NULL, NULL, NS_TRUE);
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
Ns_ConnDeleteCookie(Ns_Conn *conn, char *name, char *domain, char *path)
{
    Ns_ConnSetCookieEx(conn, name, NULL, -1, domain, path, NS_FALSE);
}

void
Ns_ConnDeleteSecureCookie(Ns_Conn *conn, char *name, char *domain, char *path)
{
    Ns_ConnSetCookieEx(conn, name, NULL, -1, domain, path, NS_TRUE);
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
Ns_ConnGetCookie(Ns_DString *dest, Ns_Conn *conn, char *name)
{
    Ns_Set  *hdrs = Ns_ConnHeaders(conn);
    char    *p, *q, *value = NULL;
    char     save;
    int      nameLen, i;

    nameLen = strlen(name);

    for (i = 0; i < hdrs->size; ++i) {

        if (strcasecmp(hdrs->fields[i].name, "Cookie") == 0
            && (p = strstr(hdrs->fields[i].value, name)) != NULL) {

            if (*(p += nameLen) == '=') {
                ++p; /* advance past equals sign */
                if (*p == '"') {
                    ++p; /* advance past optional quote mark */
                }
                q = p;
                while (*q != '"' && *q != ';' && *q != '\0') {
                    ++q;
                }
                save = *q;
                *q = '\0';
                value = Ns_UrlQueryDecode(dest, p, NULL);
                *q = save;
                break;
            }
        }
    }

    return value;
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
NsTclSetCookieObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Conn  *conn = GetConn(interp);
    char     *name, *data, *domain = NULL, *path = NULL;
    int       maxage = 0, secure = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-secure", Ns_ObjvBool,   &secure, NULL},
        {"-domain", Ns_ObjvString, &domain, NULL},
        {"-path",   Ns_ObjvString, &path,   NULL},
        {"-maxage", Ns_ObjvInt,    &maxage, NULL},
        {"--",      Ns_ObjvBreak,  NULL,    NULL},
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

    Ns_ConnSetCookieEx(conn, name, data, maxage, domain, path, secure);

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
NsTclGetCookieObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Conn     *conn;
    Ns_DString   ds;
    int          status = TCL_OK;

    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "name ?default?");
        return TCL_ERROR;
    }
    if ((conn = GetConn(interp)) == NULL) {
        return TCL_ERROR;
    }
    Ns_DStringInit(&ds);
    if (Ns_ConnGetCookie(&ds, conn, Tcl_GetString(objv[1]))) {
        Tcl_DStringResult(interp, &ds);
    } else if (objc == 3) {
        Tcl_SetObjResult(interp, objv[2]);
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
NsTclDeleteCookieObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Conn  *conn = GetConn(interp);
    char     *name, *domain = NULL, *path = NULL;
    int       secure = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-secure", Ns_ObjvBool,   &secure, NULL},
        {"-domain", Ns_ObjvString, &domain, NULL},
        {"-path",   Ns_ObjvString, &path,   NULL},
        {"--",      Ns_ObjvBreak,  NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"name",  Ns_ObjvString, &name, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (conn == NULL || Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_ConnSetCookieEx(conn, name, NULL, -1, domain, path, secure);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * GetConn --
 *
 *      Get the conn and make sure the headers have not already
 *      been sent.
 *
 * Results:
 *      Ns_Conn pointer or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_Conn *
GetConn(Tcl_Interp *interp)
{
    Ns_Conn *conn = Ns_TclGetConn(interp);

    if (conn == NULL) {
        Tcl_SetResult(interp, "No connection available.", TCL_STATIC);
    } else if (conn->flags & NS_CONN_SENTHDRS) {
        Tcl_SetResult(interp, "Cannot set cookie, "
                      "reponse headers already sent to user-agent.", TCL_STATIC);
    }

    return conn;
}
