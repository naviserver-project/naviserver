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
 * auth.c --
 *
 *	URL level HTTP authorization support.
 */

#include "nsd.h"

/*
 * The following proc is used for simple user authorization.  It
 * could be useful for global modules (e.g., nscp).
 */

static Ns_UserAuthorizeProc    *userProcPtr = NULL;


/*
 *----------------------------------------------------------------------
 *
 * Ns_AuthorizeRequest --
 *
 *	Check for proper HTTP authorization of a request.
 *
 * Results:
 *	User supplied routine is expected to return NS_OK if authorization
 *	is allowed, NS_UNAUTHORIZED if a correct username/passwd could
 *	allow authorization, NS_FORBIDDEN if no username/passwd would ever
 *	allow access, or NS_ERROR on error.
 *
 * Side effects:
 *	Depends on user supplied routine. method and url could be NULL in case
 *      of non HTTP request
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_AuthorizeRequest(const char *server, const char *method, const char *url,
	            const char *user, const char *passwd, const char *peer)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    
    servPtr = NsGetServer(server);
    if (unlikely(servPtr == NULL) || servPtr->request.authProc == NULL) {
    	return NS_OK;
    }
    return (*servPtr->request.authProc)(server, method, url, user, passwd, peer);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetRequestAuthorizeProc --
 *
 *	Set the proc to call when authorizing requests.
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
Ns_SetRequestAuthorizeProc(const char *server, Ns_RequestAuthorizeProc *procPtr)
{
    NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(procPtr != NULL);
    
    servPtr = NsGetServer(server);
    if (servPtr != NULL) {
	servPtr->request.authProc = procPtr;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclRequestAuthorizeObjCmd --
 *
 *	Implements ns_requestauthorize as obj command.
 *
 * Results:
 *	Tcl result.
 *
 * Side effects:
 *	See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRequestAuthorizeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp   *itPtr = arg;
    int         status;

    if ((objc != 5) && (objc != 6)) {
        Tcl_WrongNumArgs(interp, 1, objv,
			"method url authuser authpasswd ?ipaddr?");
        return TCL_ERROR;
    }
    status = Ns_AuthorizeRequest(itPtr->servPtr->server,
	    Tcl_GetString(objv[1]),
	    Tcl_GetString(objv[2]),
	    Tcl_GetString(objv[3]),
	    Tcl_GetString(objv[4]),
	    objc < 6 ? NULL : Tcl_GetString(objv[5]));

    switch (status) {
	case NS_OK:
	    Tcl_SetResult(interp, "OK", TCL_STATIC);
	    break;

	case NS_ERROR:
	    Tcl_SetResult(interp, "ERROR", TCL_STATIC);
	    break;

	case NS_FORBIDDEN:
	    Tcl_SetResult(interp, "FORBIDDEN", TCL_STATIC);
	    break;

	case NS_UNAUTHORIZED:
	    Tcl_SetResult(interp, "UNAUTHORIZED", TCL_STATIC);
	    break;

	default:
	    Tcl_AppendResult(interp, "could not authorize \"",
			Tcl_GetString(objv[1]), " ",
			Tcl_GetString(objv[2]), "\"", NULL);
	    return TCL_ERROR;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AuthorizeUser --
 *
 *	Verify that a user's password matches his name.
 *	passwd is the unencrypted password.
 *
 * Results:
 *	NS_OK or NS_ERROR; if none registered, NS_ERROR.
 *
 * Side effects:
 *	Depends on the supplied routine.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_AuthorizeUser(const char *user, const char *passwd)
{
    Ns_ReturnCode status;
    
    NS_NONNULL_ASSERT(user != NULL);
    NS_NONNULL_ASSERT(passwd != NULL);
    
    if (userProcPtr == NULL) {
	status = NS_ERROR;
    } else {
        status = (*userProcPtr)(user, passwd);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetUserAuthorizeProc --
 *
 *	Set the proc to call when authorizing users.
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
Ns_SetUserAuthorizeProc(Ns_UserAuthorizeProc *procPtr)
{
    NS_NONNULL_ASSERT(procPtr != NULL);
    
    userProcPtr = procPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * NsParseAuth --
 *
 *      Parse an HTTP authorization string.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May set the auth Passwd and User connection pointers.
 *
 *----------------------------------------------------------------------
 */

void
NsParseAuth(Conn *connPtr, char *auth)
{
    register char *p;

    NS_NONNULL_ASSERT(connPtr != NULL);
    NS_NONNULL_ASSERT(auth != NULL);
    
    if (connPtr->auth == NULL) {
        connPtr->auth = Ns_SetCreate(NULL);
    }

    p = auth;
    while (*p != '\0' && CHARTYPE(space, *p) == 0) {
        ++p;
    }
    if (*p != '\0') {
	register char *q, *v;
	char           save;

        save = *p;
        *p = '\0';

        if (STRIEQ(auth, "Basic")) {
	    size_t size;

            (void)Ns_SetPut(connPtr->auth, "AuthMethod", "Basic");

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }

            size = strlen(q) + 3u;
            v = ns_malloc(size);
            size = Ns_HtuuDecode(q, (unsigned char *) v, size);
            v[size] = '\0';
            q = strchr(v, ':');
            if (q != NULL) {
                *q++ = '\0';
                (void)Ns_SetPut(connPtr->auth, "Password", q);
            }
            (void)Ns_SetPut(connPtr->auth, "Username", v);
            ns_free(v);
        } else

        if (STRIEQ(auth, "Digest")) {
	    (void)Ns_SetPut(connPtr->auth, "AuthMethod", "Digest");

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }

            while (q != NULL && *q != '\0') {
		size_t idx;
		char   save2;

                p = strchr(q, '=');
                if (p == NULL) {
                    break;
                }
                v = p - 1;
                /* Trim trailing spaces */
                while (v > q && CHARTYPE(space, *v) != 0) {
                    v--;
                }
                /* Remember position */
                save2 = *(++v);
                *v = '\0';
                idx = Ns_SetPut(connPtr->auth, q, NULL);
                /* Restore character */
                *v = save2;
                /* Skip = and optional spaces */
                p++;
                while (*p != '\0' && CHARTYPE(space, *p) != 0) {
                    p++;
                }
                if (*p == '\0') {
                    break;
                }
                /* Find end of the value, deal with quotes strings */
                if (*p == '"') {
                    for (q = ++p; *q != '\0' && *q != '"'; q++) {
                        ;
                    }
                } else {
                    for (q = p; *q != '\0' && *q != ',' && CHARTYPE(space, *q) == 0; q++) {
                        ;
                    }
                }
                save2 = *q;
                *q = '\0';
                /* Update with current value */
                Ns_SetPutValue(connPtr->auth, idx, p);
                *q = save2;
                /* Advance to the end of the param value, can be end or next name*/
                while (*q != '\0' && (*q == ',' || *q == '"' || CHARTYPE(space, *q) != 0)) {
                    q++;
                }
            }

        }
        if (p != NULL) {
	    *p = save;
	}
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
