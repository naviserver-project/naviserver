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
 * url.c --
 *
 *      Parse URLs.
 */

#include "nsd.h"


/*
 *----------------------------------------------------------------------
 *
 * Ns_RelativeUrl --
 *
 *      If the url passed in is for this server, then the initial
 *      part of the URL is stripped off. e.g., on a server whose
 *      location is http://www.foo.com, Ns_RelativeUrl of
 *      "http://www.foo.com/hello" will return "/hello".
 *
 * Results:
 *      A pointer to the beginning of the relative url in the
 *      passed-in url, or NULL if error.
 *
 * Side effects:
 *      Will set errno on error.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_RelativeUrl(const char *url, const char *location)
{
    const char *v, *result;

    if (url == NULL || location == NULL) {
        result = NULL;
    } else {

        /*
         * Ns_Match will return the point in URL where location stops
         * being equal to it because location ends.
         *
         * e.g., if location = "http://www.foo.com" and
         * url="http://www.foo.com/a/b" then after the call,
         * v="/a/b", or NULL if there's a mismatch.
         */

        v = Ns_Match(location, url);
        if (v != NULL) {
            url = v;
        }
        while (url[0] == '/' && url[1] == '/') {
            ++url;
        }
        result = url;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseUrl --
 *
 *      Parse a URL into its component parts
 *
 * Results:
 *      NS_OK or NS_ERROR
 *
 * Side effects:
 *      Pointers to the protocol, host, port, path, and "tail" (last
 *      path element) will be set by reference in the passed-in pointers.
 *      The passed-in url will be modified.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ParseUrl(char *url, char **pprotocol, char **phost,
            char **pport, char **ppath, char **ptail)
{
    char *end;

    *pprotocol = NULL;
    *phost = NULL;
    *pport = NULL;
    *ppath = NULL;
    *ptail = NULL;

    /*
     * Set variable "end" to the end of the protocol
     * http://www.foo.com:8000/baz/blah/spoo.html
     *     ^
     *     +--end
     */

    for (end = url; CHARTYPE(alpha, *end) != 0; end++) {
        ;
    }
    if (*end == ':') {
        /*
         * There is a protocol specified. Clear out the colon.
         * Set pprotocol to the start of the protocol, and url to
         * the first character after the colon.
         *
         * http\0//www.foo.com:8000/baz/blah/spoo.html
         * ^   ^ ^
         * |   | +-- url
         * |   +-- end
         * +-------- *pprotocol
         */

        *end = '\0';
        *pprotocol = url;
        url = end + 1;
    }

    if (url[0] == '/' && url[1] == '/') {

        /*
         * There are two slashes, which means a host is specified.
         * Advance url past that and set *phost.
         *
         * http\0//www.foo.com:8000/baz/blah/spoo.html
         * ^   ^   ^
         * |   |   +-- url, *phost
         * |   +-- end
         * +-------- *pprotocol
         */

        url = url + 2;

        *phost = url;

        /*
         * Look for a port number, which is optional.
         */
        Ns_HttpParseHost(url, phost, &end);

        if (end != NULL) {

            /*
             * A port was specified. Clear the colon and
             * set *pport to the first digit.
             *
             * http\0//www.foo.com\08000/baz/blah/spoo.html
             * ^       ^          ^ ^
             * |       +-- *phost | +------ url, *pport
             * +----- *pprotocol  +--- end
             */

            *end = '\0';
            url = end + 1;
            *pport = url;
        } else {
            /*
             * No port was specified.
             *
             * If the url has the host specified in IP literal notation, the
             * host entry is terminated with a null character. The next string
             * operation has to start after the enclosing bracket.
             */
            if (*phost != NULL && *phost != url) {
                url += strlen(*phost) + 2u;
            }
        }

        /*
         * Move up to the slash which starts the path/tail.
         * Clear out the dividing slash.
         *
         * http\0//www.foo.com\08000\0baz/blah/spoo.html
         * ^       ^            ^   ^ ^
         * |       |            |   | +-- url
         * |       +-- *phost   |   +-- end
         * +----- *pprotocol    +-- *pport
         */

        end = strchr(url, INTCHAR('/'));
        if (end == NULL) {

            /*
             * No path or tail specified. Return.
             */

            *ppath = (char *)"";
            *ptail = (char *)"";

        } else {
            *end = '\0';
            url = end + 1;

            /*
             * Set the path to URL and advance to the last slash.
             * Set ptail to the character after that, or if there is none,
             * it becomes path and path becomes an empty string.
             *
             * http\0//www.foo.com\08000\0baz/blah/spoo.html
             * ^       ^            ^   ^ ^       ^^
             * |       |            |   | |       |+-- *ptail
             * |       |            |   | |       +-- end
             * |       |            |   | +-- *ppath
             * |       +-- *phost   |   +-- end
             * +----- *pprotocol    +-- *pport
             */

            *ppath = url;
            end = strrchr(url, INTCHAR('/'));
            if (end == NULL) {
                *ptail = *ppath;
                *ppath = (char *)"";
            } else {
                *end = '\0';
                *ptail = end + 1;
            }
        }
    } else {

        /*
         * This URL does not have a protocol or host. If it begins with a
         * slash, then separate the tail from the path, otherwise it's all
         * tail.
         */

        if (*url == '/') {
            url++;
            *ppath = url;

            /*
             * Find the last slash on the right and everything after that
             * becomes tail; if there are no slashes then it's all tail
             * and path is an empty string.
             */

            end = strrchr(url, INTCHAR('/'));
            if (end == NULL) {
                *ptail = *ppath;
                *ppath = (char *)"";
            } else {
                *end = '\0';
                *ptail = end + 1;
            }
        } else {
            /*
             * Just set the tail, there are no slashes.
             */

            *ptail = url;
        }
    }
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AbsoluteUrl --
 *
 *      Construct an URL based on baseurl but with as many parts of
 *      the incomplete url as possible.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_AbsoluteUrl(Ns_DString *dsPtr, const char *url, const char *base)
{
    Ns_DString    urlDs, baseDs;
    char         *proto, *host, *port, *path, *tail;
    char         *bproto, *bhost, *bport, *bpath, *btail;
    Ns_ReturnCode status;

    /*
     * Copy the URL's to allow Ns_ParseUrl to destroy them.
     */

    Ns_DStringInit(&urlDs);
    Ns_DStringInit(&baseDs);

    Ns_DStringAppend(&urlDs, url);
    (void) Ns_ParseUrl(urlDs.string, &proto, &host, &port, &path, &tail);

    Ns_DStringAppend(&baseDs, base);
    status = Ns_ParseUrl(baseDs.string, &bproto, &bhost, &bport, &bpath, &btail);

    if (bproto == NULL || bhost == NULL || bpath == NULL) {
        status = NS_ERROR;
        goto done;
    }
    if (proto == NULL) {
        proto = bproto;
    }
    assert(proto != NULL);

    if (host == NULL) {
        host = bhost;
        port = bport;
    }
    assert(host != NULL);

    if (path == NULL) {
        path = bpath;
    }
    assert(path != NULL);

    if (strchr(host, INTCHAR(':')) == NULL) {
        /*
         * We have to use IP literal notation to avoid ambiguity of colon
         * (part of address or separator for port).
         */
        Ns_DStringVarAppend(dsPtr, proto, "://", host, (char *)0);
    } else {
        Ns_DStringVarAppend(dsPtr, proto, "://[", host, "]", (char *)0);
    }
    if (port != NULL) {
        Ns_DStringVarAppend(dsPtr, ":", port, (char *)0);
    }
    if (*path == '\0') {
        Ns_DStringVarAppend(dsPtr, "/", tail, (char *)0);
    } else {
        Ns_DStringVarAppend(dsPtr, "/", path, "/", tail, (char *)0);
    }
done:
    Ns_DStringFree(&urlDs);
    Ns_DStringFree(&baseDs);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclParseUrlObjCmd --
 *
 *    Implement the "ns_parseurl" command. Offers the functionality of
 *    Ns_ParseUrl on the Tcl layer.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */
int
NsTclParseUrlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;
    char       *urlString;
    Ns_ObjvSpec args[] = {
        {"url",  Ns_ObjvString, &urlString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        char *url, *protocol, *host, *portString, *path, *tail;

        url = ns_strdup(urlString);
        if (Ns_ParseUrl(url, &protocol, &host, &portString, &path, &tail) == NS_OK) {
            Tcl_Obj *resultObj = Tcl_NewListObj(0, NULL);

            if (protocol != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("proto", 5));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(protocol, -1));
            }
            if (host != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("host", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(host, -1));
            }
            if (portString != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("port", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(portString, -1));
            }
            if (path != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("path", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(path, -1));
            }
            if (tail != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("tail", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(tail, -1));
            }

            Tcl_SetObjResult(interp, resultObj);

        } else {
            Ns_TclPrintfResult(interp, "Could not parse url \"%s\"", url);
            result = TCL_ERROR;
        }
        ns_free(url);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAbsoluteUrlObjCmd --
 *
 *    Implement the "ns_absoluteurl" command. Offers the functionality of
 *    Ns_AbsoluteUrl on the Tcl layer.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */
int
NsTclAbsoluteUrlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;
    char       *urlString, *baseString;
    Ns_ObjvSpec args[] = {
        {"partialurl", Ns_ObjvString, &urlString, NULL},
        {"baseurl",    Ns_ObjvString, &baseString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        if (Ns_AbsoluteUrl(&ds, urlString, baseString) == NS_OK) {
            Tcl_DStringResult(interp, &ds);
        } else {
            Ns_TclPrintfResult(interp, "Could not parse base url into protocol, host and path");
            Tcl_DStringFree(&ds);
            result = TCL_ERROR;
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
