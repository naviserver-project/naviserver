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
 * auth.c --
 *
 *      URL level HTTP authorization support.
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
 *      Check for proper HTTP authorization of a request.
 *
 * Results:
 *      User supplied routine is expected to return NS_OK if authorization
 *      is allowed, NS_UNAUTHORIZED if a correct username/passwd could
 *      allow authorization, NS_FORBIDDEN if no username/passwd would ever
 *      allow access, or NS_ERROR on error.
 *
 * Side effects:
 *      Depends on user supplied routine. "method" and "url" could be NULL in
 *      case of non-HTTP requests.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_AuthorizeRequest(const char *server, const char *method, const char *url,
                    const char *user, const char *passwd, const char *peer)
{
    Ns_ReturnCode   status;
    const NsServer *servPtr;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(method != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    servPtr = NsGetServer(server);
    if (unlikely(servPtr == NULL) || servPtr->request.authProc == NULL) {
        status = NS_OK;
    } else {
        status = (*servPtr->request.authProc)(server, method, url, user, passwd, peer);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetRequestAuthorizeProc --
 *
 *      Set the proc to call when authorizing requests.
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
 *      Implements "ns_requestauthorize".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclRequestAuthorizeObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsInterp *itPtr = clientData;
    int             result = TCL_OK;
    char           *method, *url, *authuser, *authpasswd, *ipaddr = NULL;
    Ns_ObjvSpec     args[] = {
        {"method",     Ns_ObjvString, &method,     NULL},
        {"url",        Ns_ObjvString, &url,        NULL},
        {"authuser",   Ns_ObjvString, &authuser,   NULL},
        {"authpasswd", Ns_ObjvString, &authpasswd, NULL},
        {"?ipaddr",    Ns_ObjvString, &ipaddr,     NULL},
        {NULL, NULL, NULL, NULL}
    };

#ifdef NS_WITH_DEPRECATED
    if (strcmp(Tcl_GetString(objv[0]), "ns_checkurl") == 0) {
        Ns_LogDeprecated(objv, 1, "ns_requestauthorize ...", NULL);
    }
#endif

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_ReturnCode status;

        status = Ns_AuthorizeRequest(itPtr->servPtr->server, method, url,
                                     authuser, authpasswd, ipaddr);
        switch (status) {
        case NS_OK:
            Tcl_SetObjResult(interp, Tcl_NewStringObj("OK", 2));
            break;

        case NS_ERROR:
            Tcl_SetObjResult(interp, Tcl_NewStringObj("ERROR", 5));
            break;

        case NS_FORBIDDEN:
            Tcl_SetObjResult(interp, Tcl_NewStringObj("FORBIDDEN", TCL_INDEX_NONE));
            break;

        case NS_UNAUTHORIZED:
            Tcl_SetObjResult(interp, Tcl_NewStringObj("UNAUTHORIZED", TCL_INDEX_NONE));
            break;

        case NS_FILTER_BREAK:  NS_FALL_THROUGH; /* fall through */
        case NS_FILTER_RETURN: NS_FALL_THROUGH; /* fall through */
        case NS_TIMEOUT:
            Ns_TclPrintfResult(interp, "could not authorize \"%s %s\"",
                               Tcl_GetString(objv[1]), Tcl_GetString(objv[2]));
            result = TCL_ERROR;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AuthorizeUser --
 *
 *      Verify that a user's password matches his name.
 *      passwd is the unencrypted password.
 *
 * Results:
 *      NS_OK or NS_ERROR; if none registered, NS_ERROR.
 *
 * Side effects:
 *      Depends on the supplied routine.
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
 *      Set the proc to call when authorizing users.
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
NsParseAuth(Conn *connPtr, const char *auth)
{
    register char *p;
    Tcl_DString    authDs;

    NS_NONNULL_ASSERT(connPtr != NULL);
    NS_NONNULL_ASSERT(auth != NULL);

    if (connPtr->auth == NULL) {
        connPtr->auth = Ns_SetCreate(NS_SET_NAME_AUTH);
    }

    Tcl_DStringInit(&authDs);
    Tcl_DStringAppend(&authDs, auth, TCL_INDEX_NONE);

    p = authDs.string;
    while (*p != '\0' && CHARTYPE(space, *p) == 0) {
        ++p;
    }
    if (*p != '\0') {
        register char *q, *v;
        char           save;

        save = *p;
        *p = '\0';

        if (STRIEQ(authDs.string, "Basic")) {
            size_t     size;
            TCL_SIZE_T userLength;

            (void)Ns_SetPutSz(connPtr->auth, "AuthMethod", 10, "Basic", 5);

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }

            size = strlen(q) + 3u;
            v = ns_malloc(size);
            size = Ns_HtuuDecode(q, (unsigned char *) v, size);
            v[size] = '\0';

            q = strchr(v, INTCHAR(':'));
            if (q != NULL) {
                TCL_SIZE_T pwLength;

                *q++ = '\0';
                pwLength = (TCL_SIZE_T)((v+size) - q);
                (void)Ns_SetPutSz(connPtr->auth, "Password", 8, q, pwLength);
                userLength = (TCL_SIZE_T)size - (pwLength + 1);
            } else {
                userLength = (TCL_SIZE_T)size;
            }
            (void)Ns_SetPutSz(connPtr->auth, "Username", 8, v, userLength);
            ns_free(v);

        } else if (STRIEQ(authDs.string, "Digest")) {
            (void)Ns_SetPutSz(connPtr->auth, "AuthMethod", 10, "Digest", 6);

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }

            while (*q != '\0') {
                size_t idx;
                char   save2;

                p = strchr(q, INTCHAR('='));
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
                idx = Ns_SetPutSz(connPtr->auth, q, (TCL_SIZE_T)(v-q), NULL, 0);
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
                Ns_SetPutValueSz(connPtr->auth, idx, p, TCL_INDEX_NONE);
                *q = save2;
                /* Advance to the end of the param value, can be end or next name*/
                while (*q != '\0' && (*q == ',' || *q == '"' || CHARTYPE(space, *q) != 0)) {
                    q++;
                }
            }
        } else if (STRIEQ(authDs.string, "Bearer")) {

            (void)Ns_SetPutSz(connPtr->auth, "AuthMethod", 10, "Bearer", 6);

            /* Skip spaces */
            q = p + 1;
            while (*q != '\0' && CHARTYPE(space, *q) != 0) {
                q++;
            }
            (void)Ns_SetPutSz(connPtr->auth, "Token", 5, q,
                              (authDs.length - (TCL_SIZE_T)(q - authDs.string)));
        }
        if (p != NULL) {
            *p = save;
        }
    }
    Tcl_DStringFree(&authDs);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
