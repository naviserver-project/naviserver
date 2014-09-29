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
 * returnresp.c --
 *
 *      Functions that return standard HTTP responses.
 */

#include "nsd.h"

#define MAX_RECURSION 3 /* Max redirect recursion limit. */


/*
 * Local functions defined in this file
 */

static Ns_ServerInitProc ConfigServerRedirects;
static int ReturnRedirect(Ns_Conn *conn, int status, int *resultPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);


/*
 *----------------------------------------------------------------------
 *
 * NsConfigRedirects --
 *
 *      Associate a URL with a status. Rather than return the
 *      default error page for this status, an internal redirect
 *      will be issued to the url.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Previous registration is deleted if url is NULL.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigRedirects(void)
{
    NsRegisterServerInit(ConfigServerRedirects);
}

static int
ConfigServerRedirects(CONST char *server)
{
    NsServer   *servPtr = NsGetServer(server);
    Ns_Set     *set;
    CONST char *path;
    int         i;

    Tcl_InitHashTable(&servPtr->request.redirect, TCL_ONE_WORD_KEYS);

    path = Ns_ConfigGetPath(server, NULL, "redirects", NULL);
    set = Ns_ConfigGetSection(path);

    for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
        CONST char *key, *map;
	int status;

        key = Ns_SetKey(set, i);
        map = Ns_SetValue(set, i);
        status = atoi(key);
        if (status <= 0 || *map == '\0') {
            Ns_Log(Error, "redirects[%s]: invalid redirect '%s=%s'",
                   server, key, map);
        } else {
            Ns_RegisterReturn(status, map);
        }
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RegisterReturn --
 *
 *      Associate a URL with a status. Rather than return the
 *      default error page for this status, an internal redirect
 *      will be issued to the url.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Previous registration is deleted if url is NULL.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RegisterReturn(int status, CONST char *url)
{
    NsServer      *servPtr;
    int            isNew;

    servPtr = NsGetInitServer();
    if (servPtr != NULL) {
        Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&servPtr->request.redirect,
						  INT2PTR(status), &isNew);
        if (!isNew) {
            ns_free(Tcl_GetHashValue(hPtr));
        }
        if (url == NULL) {
            Tcl_DeleteHashEntry(hPtr);
        } else {
            Tcl_SetHashValue(hPtr, ns_strdup(url));
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnStatus --
 *
 *      Return an arbitrary status code.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnStatus(Ns_Conn *conn, int status)
{
    int result;

    if (ReturnRedirect(conn, status, &result)) {
        return result;
    }
    Ns_ConnSetResponseStatus(conn, status);
    return Ns_ConnWriteVData(conn, NULL, 0, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnOk --
 *
 *      Return the OK status to the client.
 *
 * Results:
 *      See Ns_ReturnStatus
 *
 * Side effects:
 *      See Ns_ReturnStatus
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnOk(Ns_Conn *conn)
{
    return Ns_ConnReturnStatus(conn, 200);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnMoved --
 *
 *      Return a 301 Redirection to the client, or 204 No Content if
 *      url is null.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnMoved(Ns_Conn *conn, CONST char *url)
{
    Ns_DString ds, msg;
    int        result;

    Ns_DStringInit(&ds);
    Ns_DStringInit(&msg);
    if (url != NULL) {
        if (*url == '/') {
            Ns_ConnLocationAppend(conn, &ds);
        }
        Ns_DStringAppend(&ds, url);
        Ns_ConnSetHeaders(conn, "Location", ds.string);
        Ns_DStringVarAppend(&msg, "<A HREF=\"", ds.string,
                            "\">The requested URL has moved permanently here.</A>", NULL);
        result = Ns_ConnReturnNotice(conn, 301, "Redirection", msg.string);
    } else {
        result = Ns_ConnReturnNotice(conn, 204, "No Content", msg.string);
    }
    Ns_DStringFree(&msg);
    Ns_DStringFree(&ds);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNoResponse --
 *
 *      Return a status message to the client.
 *
 * Results:
 *      See Ns_ReturnStatus
 *
 * Side effects:
 *      See Ns_ReturnStatus
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnNoResponse(Ns_Conn *conn)
{
    return Ns_ConnReturnStatus(conn, 204);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnRedirect --
 *
 *      Return a 302 Redirection to the client, or 204 No Content if
 *      url is null.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnRedirect(Ns_Conn *conn, CONST char *url)
{
    Ns_DString ds, msg;
    int        result;

    Ns_DStringInit(&ds);
    Ns_DStringInit(&msg);
    if (url != NULL) {
        if (*url == '/') {
            Ns_ConnLocationAppend(conn, &ds);
        }
        Ns_DStringAppend(&ds, url);
        Ns_ConnSetHeaders(conn, "Location", ds.string);
        Ns_DStringVarAppend(&msg, "<A HREF=\"", ds.string,
                            "\">The requested URL has moved here.</A>", NULL);
        result = Ns_ConnReturnNotice(conn, 302, "Redirection", msg.string);
    } else {
        result = Ns_ConnReturnNotice(conn, 204, "No Content", msg.string);
    }
    Ns_DStringFree(&msg);
    Ns_DStringFree(&ds);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnBadRequest --
 *
 *      Return an 'invalid request' HTTP status line with an error
 *      message.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnBadRequest(Ns_Conn *conn, CONST char *reason)
{
    Ns_DString ds;
    int        result;

    if (ReturnRedirect(conn, 400, &result)) {
        return result;
    }
    Ns_DStringInit(&ds);
    Ns_DStringAppend(&ds,
        "The HTTP request presented by your browser is invalid.");
    if (reason != NULL) {
        Ns_DStringVarAppend(&ds, "<P>\n", reason, NULL);
    }
    result = Ns_ConnReturnNotice(conn, 400, "Invalid Request", ds.string);
    Ns_DStringFree(&ds);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnUnauthorized --
 *
 *      Return a 401 Unauthorized response, which will prompt the
 *      user for a Basic authentication username/password.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnUnauthorized(Ns_Conn *conn)
{
    Conn       *connPtr = (Conn *) conn;
    Ns_DString  ds;
    int         result;

    assert(conn != NULL);

    if (Ns_SetIGet(conn->outputheaders, "WWW-Authenticate") == NULL) {
        Ns_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, "Basic realm=\"",
                            connPtr->poolPtr->servPtr->opts.realm, "\"", NULL);
        Ns_ConnSetHeaders(conn, "WWW-Authenticate", ds.string);
        Ns_DStringFree(&ds);
    }
    if (ReturnRedirect(conn, 401, &result)) {
        return result;
    }

    return Ns_ConnReturnNotice(conn, 401, "Access Denied",
               "The requested URL cannot be accessed because a "
               "valid username and password are required.");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnForbidden --
 *
 *      Return a 403 Forbidden response.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnForbidden(Ns_Conn *conn)
{
    int result;

    if (ReturnRedirect(conn, 403, &result)) {
        return result;
    }

    return Ns_ConnReturnNotice(conn, 403, "Forbidden",
               "The requested URL cannot be accessed by this server.");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotFound --
 *
 *      Return a 404 Not Found response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnNotFound(Ns_Conn *conn)
{
    int result;

    if (ReturnRedirect(conn, 404, &result)) {
        return result;
    }

    return Ns_ConnReturnNotice(conn, 404, "Not Found",
               "The requested URL was not found on this server.");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotModified --
 *
 *      Return a 304 Not Modified response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */
int
Ns_ConnReturnNotModified(Ns_Conn *conn)
{
    return Ns_ConnReturnStatus(conn, 304);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnEntityTooLarge --
 *
 *      Return a 413 Request Entity to large response.
 *
 * Results:
 *      NS_OK/NS_ERROR 
 *
 * Side effects:
 *      Will close the connection. 
 *
 *----------------------------------------------------------------------
 */
int
Ns_ConnReturnEntityTooLarge(Ns_Conn *conn)
{
    int result;

    if (ReturnRedirect(conn, 413, &result)) {
        return result;
    }
    return Ns_ConnReturnNotice(conn, 413, "Request Entity Too Large",
	"The request entity (e.g. file to be uploaded) is too large.");
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnRequestURITooLong --
 *
 *      Return a 414 Request URI to long.
 *
 * Results:
 *      NS_OK/NS_ERROR 
 *
 * Side effects:
 *      Will close the connection. 
 *
 *----------------------------------------------------------------------
 */
int
Ns_ConnReturnRequestURITooLong(Ns_Conn *conn)
{
    int result;

    if (ReturnRedirect(conn, 414, &result)) {
        return result;
    }
    return Ns_ConnReturnNotice(conn, 414, "Request-URI Too Long",
        "The request URI is too long. "
	"You might to consider to provide a larger value for maxline in your NaviServer config file.");
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnHeaderLineTooLong --
 *
 *      Return a 431 Request Header Line to long.
 *
 * Results:
 *      NS_OK/NS_ERROR 
 *
 * Side effects:
 *      Will close the connection. 
 *
 *----------------------------------------------------------------------
 */
int
Ns_ConnReturnHeaderLineTooLong(Ns_Conn *conn)
{
    int result;

    if (ReturnRedirect(conn, 431, &result)) {
        return result;
    }
    return Ns_ConnReturnNotice(conn, 431, "Request Header Fields Too Large",
        "A provided request header line is too long. "
	"You might to consider to provide a larger value for maxline in your NaviServer config file");
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotImplemented --
 *
 *      Return a 501 Not Implemented response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnNotImplemented(Ns_Conn *conn)
{
    int result;

    if (ReturnRedirect(conn, 501, &result)) {
        return result;
    }

    return Ns_ConnReturnNotice(conn, 501, "Not Implemented",
               "The requested URL or method is not implemented "
               "by this server.");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnInternalError --
 *
 *      Return a 500 Internal Error response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnInternalError(Ns_Conn *conn)
{
    int result;

    assert(conn != NULL);

    Ns_SetTrunc(conn->outputheaders, 0);
    if (ReturnRedirect(conn, 500, &result)) {
        return result;
    }

    return Ns_ConnReturnNotice(conn, 500, "Server Error",
               "The requested URL cannot be accessed "
               "due to a system error on this server.");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnUnavailable --
 *
 *      Return a 503 busy/unavailable response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ConnReturnUnavailable(Ns_Conn *conn)
{
    int result;

    assert(conn != NULL);

    Ns_SetTrunc(conn->outputheaders, 0);
    if (ReturnRedirect(conn, 503, &result)) {
        return result;
    }

    return Ns_ConnReturnNotice(conn, 503, "Service Unavailable",
               "The server is temporarily unable to service your request. "
               "Please try again later.");
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnRedirect --
 *
 *      Redirect internally to the URL registered for the given status.
 *
 * Results:
 *      1 if a redirect exists and ran, 0 otherwise.
 *      The status of the redirected page is left in resultPtr.
 *
 * Side effects:
 *      May write and close the conn if the redirect exists.
 *
 *----------------------------------------------------------------------
 */

static int
ReturnRedirect(Ns_Conn *conn, int status, int *resultPtr)
{
    Tcl_HashEntry *hPtr;
    Conn          *connPtr = (Conn *) conn;
    NsServer      *servPtr;

    assert(connPtr != NULL);
    assert(resultPtr != NULL);

    servPtr = connPtr->poolPtr->servPtr;
    assert(servPtr != NULL);

    hPtr = Tcl_FindHashEntry(&servPtr->request.redirect, INT2PTR(status));
    if (hPtr != NULL) {
        if (++connPtr->recursionCount > MAX_RECURSION) {
            Ns_Log(Error, "return: failed to redirect '%d': "
                   "exceeded recursion limit of %d", status, MAX_RECURSION);
        } else {
            connPtr->responseStatus = status;
            *resultPtr = Ns_ConnRedirect(conn, Tcl_GetHashValue(hPtr));
            return 1;
        }
    }
    return 0;
}
