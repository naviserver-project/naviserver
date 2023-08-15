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
static bool ReturnRedirect(Ns_Conn *conn, int httpStatus, Ns_ReturnCode *resultPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);


/*
 *----------------------------------------------------------------------
 *
 * NsConfigRedirects --
 *
 *      Associate a URL with a status. Rather than return the
 *      default error page for this status, an internal redirect
 *      will be issued to the URL.
 *
 * Results:
 *      Status code (always NS_OK).
 *
 * Side effects:
 *      Previous registration is deleted if the URL is NULL.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigRedirects(void)
{
    NsRegisterServerInit(ConfigServerRedirects);
}

static Ns_ReturnCode
ConfigServerRedirects(const char *server)
{
    NsServer *servPtr = NsGetServer(server);

    if (servPtr != NULL) {
        Ns_Set *set = NULL;
        size_t  i;

        Tcl_InitHashTable(&servPtr->request.redirect, TCL_ONE_WORD_KEYS);
        (void) Ns_ConfigSectionPath(&set, server, NULL, "redirects", (char *)0L);

        for (i = 0u; set != NULL && i < Ns_SetSize(set); ++i) {
            const char *key, *map;
            int         statusCode;

            key = Ns_SetKey(set, i);
            map = Ns_SetValue(set, i);
            statusCode = (int)strtol(key, NULL, 10);
            if (statusCode <= 0 || *map == '\0') {
                Ns_Log(Error, "redirects[%s]: invalid redirect '%s=%s'",
                       server, key, map);
            } else {
                Ns_RegisterReturn(statusCode, map);
            }
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
 *      will be issued to the URL.
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
Ns_RegisterReturn(int status, const char *url)
{
    NsServer      *servPtr;
    int            isNew;

    servPtr = NsGetInitServer();
    if (servPtr != NULL) {
        Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&servPtr->request.redirect,
                                                  INT2PTR(status), &isNew);
        if (isNew == 0) {
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

Ns_ReturnCode
Ns_ConnReturnStatus(Ns_Conn *conn, int httpStatus)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, httpStatus, &result)) {
        Ns_ConnSetResponseStatus(conn, httpStatus);
        result = Ns_ConnWriteVData(conn, NULL, 0, 0u);
    }
    return result;
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

Ns_ReturnCode
Ns_ConnReturnOk(Ns_Conn *conn)
{
    NS_NONNULL_ASSERT(conn != NULL);

    return Ns_ConnReturnStatus(conn, 200);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnMoved --
 *
 *      Return a 301 "Redirection" to the client, or 204 "No Content" if
 *      URL is null.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnMoved(Ns_Conn *conn, const char *url)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (url != NULL) {
        Ns_DString urlDs, msgDs;

        Tcl_DStringInit(&urlDs);
        Tcl_DStringInit(&msgDs);

        if (*url == '/') {
            (void) Ns_ConnLocationAppend(conn, &urlDs);
        }
        Tcl_DStringAppend(&urlDs, url, -1);
        Ns_ConnSetHeaders(conn, "Location", urlDs.string);

        Tcl_DStringAppend(&msgDs, "<a href=\"", 9);
        Ns_QuoteHtml(&msgDs, urlDs.string);
        Tcl_DStringAppend(&msgDs, "\">The requested URL has moved permanently here.</a>", -1);

        result = Ns_ConnReturnNotice(conn, 301, "Redirection", msgDs.string);

        Tcl_DStringFree(&msgDs);
        Tcl_DStringFree(&urlDs);
    } else {
        result = Ns_ConnReturnNotice(conn, 204, "No Content", NS_EMPTY_STRING);
    }

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

Ns_ReturnCode
Ns_ConnReturnNoResponse(Ns_Conn *conn)
{
    NS_NONNULL_ASSERT(conn != NULL);

    return Ns_ConnReturnStatus(conn, 204);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnRedirect --
 *
 *      Return a 302 Redirection to the client, or 204 "No Content" if
 *      URL is null.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnRedirect(Ns_Conn *conn, const char *url)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (url != NULL) {
        Tcl_DString urlDs, msgDs;

        Tcl_DStringInit(&urlDs);
        Tcl_DStringInit(&msgDs);

        if (*url == '/') {
            (void) Ns_ConnLocationAppend(conn, &urlDs);
        }
        Tcl_DStringAppend(&urlDs, url, -1);

        Ns_UrlEncodingWarnUnencoded("header field location", urlDs.string);
        Ns_ConnSetHeaders(conn, "Location", urlDs.string);

        Tcl_DStringAppend(&msgDs, "<a href=\"", 9);
        Ns_QuoteHtml(&msgDs, urlDs.string);
        Tcl_DStringAppend(&msgDs, "\">The requested URL has moved here.</a>", -1);

        result = Ns_ConnReturnNotice(conn, 302, "Redirection", msgDs.string);

        Tcl_DStringFree(&msgDs);
        Tcl_DStringFree(&urlDs);

    } else {
        result = Ns_ConnReturnNotice(conn, 204, "No Content", NS_EMPTY_STRING);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnBadRequest --
 *
 *      Return an "invalid request" HTTP status line with an error
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

Ns_ReturnCode
Ns_ConnReturnBadRequest(Ns_Conn *conn, const char *reason)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 400, &result)) {
        Ns_DString    ds;

        Ns_DStringInit(&ds);
        Ns_DStringAppend(&ds,
                         "<p>The HTTP request presented by your browser is invalid.");
        if (reason != NULL) {
            Ns_DStringVarAppend(&ds, "<p>\n", reason, (char *)0L);
        }
        result = Ns_ConnReturnNotice(conn, 400, "Invalid Request", ds.string);
        Ns_DStringFree(&ds);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnUnauthorized --
 *
 *      Return a 401 "Unauthorized" response, which will prompt the
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

Ns_ReturnCode
Ns_ConnReturnUnauthorized(Ns_Conn *conn)
{
    const Conn   *connPtr = (const Conn *) conn;
    Ns_DString    ds;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (Ns_SetIGet(conn->outputheaders, "WWW-Authenticate") == NULL) {
        Ns_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, "Basic realm=\"",
                            connPtr->poolPtr->servPtr->opts.realm, "\"", (char *)0L);
        Ns_ConnSetHeaders(conn, "WWW-Authenticate", ds.string);
        Ns_DStringFree(&ds);
    }
    if (!ReturnRedirect(conn, 401, &result)) {
        result = Ns_ConnReturnNotice(conn, 401, "Access Denied",
                                     "The requested URL cannot be accessed because a "
                                     "valid username and password are required.");
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnForbidden --
 *
 *      Return a 403 "Forbidden" response.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnForbidden(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 403, &result)) {
        result = Ns_ConnReturnNotice(conn, 403, "Forbidden",
                                     "The requested URL cannot be accessed by this server.");
    }

    return result;

}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotFound --
 *
 *      Return a 404 "Not Found" response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnNotFound(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 404, &result)) {
        result = Ns_ConnReturnNotice(conn, 404, "Not Found",
                                     "The requested URL was not found on this server.");
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnInvalidMethod --
 *
 *      Return a 405 "Method Not Allowed" response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnInvalidMethod(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 405, &result)) {
        result = Ns_ConnReturnNotice(conn, 405, "Method Not Allowed",
                                     "The requested method is not allowed on this server.");
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotModified --
 *
 *      Return a 304 "Not Modified" response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_ConnReturnNotModified(Ns_Conn *conn)
{
    NS_NONNULL_ASSERT(conn != NULL);

    return Ns_ConnReturnStatus(conn, 304);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnEntityTooLarge --
 *
 *      Return a 413 "Request Entity too large" response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_ConnReturnEntityTooLarge(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 413, &result)) {
        result = Ns_ConnReturnNotice(conn, 413, "Request Entity Too Large",
                                     "The request entity (e.g. file to be uploaded) is too large.");
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnRequestURITooLong --
 *
 *      Return a 414 "Request URI too long".
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_ConnReturnRequestURITooLong(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 414, &result)) {
        result = Ns_ConnReturnNotice(conn, 414, "Request-URI Too Long",
                                     "The request URI is too long. You might "
                                     "consider to provide a larger value for "
                                     "maxline in your NaviServer configuration file.");
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnHeaderLineTooLong --
 *
 *      Return a 431 "Request Header Fields Too Large".
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_ConnReturnHeaderLineTooLong(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 431, &result)) {
        result = Ns_ConnReturnNotice(conn, 431, "Request Header Fields Too Large",
                                     "A provided request header line is too long. "
                                     "You might consider to provide a larger value "
                                     "for maxline in your NaviServer configuration file");
    }
   return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnNotImplemented --
 *
 *      Return a 501 "Not Implemented" response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnNotImplemented(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    if (!ReturnRedirect(conn, 501, &result)) {
        result = Ns_ConnReturnNotice(conn, 501, "Not Implemented",
                                     "The requested URL or method is not implemented "
                                     "by this server.");
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnTryReturnInternalError --
 *
 *      Call Ns_ConnReturnInternalError() in case the connection is not closed
 *      yet. We could handle this case also in Ns_ConnReturnInternalError()
 *      but we want to provide log entries with the calling context for such
 *      unexpected cases.
 *
 * Results:
 *      Ns_ReturnCode (when the connection is not closed, the result of the
 *      send operation).
 *
 * Side effects:
 *      Potentially sending message to the client.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_ConnTryReturnInternalError(Ns_Conn *conn, Ns_ReturnCode status, const char *causeString)
{
    Ns_ReturnCode result;

    if (Ns_ConnIsClosed(conn)) {
        /*
         * When the connection is already closed, we cannot return
         * the internal error to the client.
         */
        Ns_Log(Warning, "internal error (HTTP status 500) with already closed connection "
               "(%s, return code %d)",
               causeString, (int)status);
        result = status;
    } else {
        Ns_Log(Warning, "internal error (HTTP status 500) (%s, return code %d)",
               causeString, (int)status);
        result = Ns_ConnReturnInternalError(conn);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnInternalError --
 *
 *      Return a 500 "Internal Error" response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnInternalError(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    Ns_SetTrunc(conn->outputheaders, 0u);
    if (!ReturnRedirect(conn, 500, &result)) {
        result = Ns_ConnReturnNotice(conn, 500, "Server Error",
                                     "The requested URL cannot be accessed "
                                     "due to a system error on this server.");
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnUnavailable --
 *
 *      Return a 503 "Service Unavailable" response.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will close the connection.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnUnavailable(Ns_Conn *conn)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    Ns_SetTrunc(conn->outputheaders, 0u);
    if (!ReturnRedirect(conn, 503, &result)) {
        result = Ns_ConnReturnNotice(conn, 503, "Service Unavailable",
                                     "The server is temporarily unable to service your request. "
                                     "Please try again later.");
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ReturnRedirect --
 *
 *      Redirect internally to the URL registered for the given status.
 *
 * Results:
 *      NS_TRUE if a redirect exists and ran, NS_FALSE otherwise.
 *      The status of the redirected page is left in resultPtr.
 *
 * Side effects:
 *      May write and close the conn if the redirect exists.
 *
 *----------------------------------------------------------------------
 */

static bool
ReturnRedirect(Ns_Conn *conn, int httpStatus, Ns_ReturnCode *resultPtr)
{
    Conn *connPtr;
    bool  result = NS_FALSE;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(resultPtr != NULL);

    connPtr = (Conn *) conn;
    if ((connPtr->flags & NS_CONN_CLOSED) != 0u) {
        Ns_Log(Warning, "redirect status %d: connection already closed", httpStatus);
        *resultPtr = NS_ERROR;

    } else {
        const Tcl_HashEntry *hPtr;
        NsServer            *servPtr;

        servPtr = connPtr->poolPtr->servPtr;
        assert(servPtr != NULL);

        hPtr = Tcl_FindHashEntry(&servPtr->request.redirect, INT2PTR(httpStatus));
        if (hPtr != NULL) {
            if (++connPtr->recursionCount > MAX_RECURSION) {
                Ns_Log(Error, "return: failed to redirect '%d': "
                       "exceeded recursion limit of %d", httpStatus, MAX_RECURSION);
            } else {
                connPtr->responseStatus = httpStatus;
                if (httpStatus >= 400) {
                    ns_free((char *)connPtr->request.method);
                    connPtr->request.method = ns_strdup("GET");
                }
                Ns_Log(Debug, "ReturnRedirect '%s' to '%s'", connPtr->request.line, (const char *)Tcl_GetHashValue(hPtr));
                *resultPtr = Ns_ConnRedirect(conn, Tcl_GetHashValue(hPtr));
                result = NS_TRUE;
            }
        }
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
