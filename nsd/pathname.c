/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
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
 * pathname.c --
 *
 *	Functions that manipulate or return paths. 
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

#define isslash(c)	((c) == '/' || (c) == '\\')

/*
 * Local functions defined in this file.
 */

static int PathObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                      Tcl_Obj *CONST objv[], int cmd);
static char *MakePath(Ns_DString *dest, va_list *pap);
static char *ServerRoot(Ns_DString *dest, NsServer *servPtr, char *host);



/*
 *----------------------------------------------------------------------
 *
 * Ns_PathIsAbsolute --
 *
 *	Boolean: is the path absolute? 
 *
 * Results:
 *	NS_TRUE if it is, NS_FALSE if not. 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

int
Ns_PathIsAbsolute(char *path)
{
#ifdef _WIN32
    if (isalpha(*path) && path[1] == ':') {
	path += 2;
    }
#endif
    if (isslash(*path)) {
	return NS_TRUE;
    }
    return NS_FALSE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_NormalizePath --
 *
 *	Remove "..", "." from paths. 
 *
 * Results:
 *	dsPtr->string 
 *
 * Side effects:
 *	Will append to dsPtr. Assumes an absolute path.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_NormalizePath(Ns_DString *dsPtr, char *path)
{
    char end;
    register char *src, *part, *slash;
    Ns_DString tmp;

    Ns_DStringInit(&tmp);
    src = Ns_DStringAppend(&tmp, path);
#ifdef _WIN32
    if (isalpha(*src) && src[1] == ':') {
	if (isupper(*src)) {
	    *src = tolower(*src);
	}
	Ns_DStringNAppend(dsPtr, src, 2);
	src += 2;
    }
#endif

    /*
     * Move past leading slash(es)
     */
    
    while (isslash(*src)) {
	++src;
    }
    do {
	part = src;

	/*
	 * Move to next slash
	 */
	
	while (*src && !isslash(*src)) {
	    ++src;
	}
	end = *src;
	*src++ = '\0';

	if (part[0] == '.' && part[1] == '.' && part[2] == '\0') {

	    /*
	     * There's a "..", so wipe out one path backwards.
	     */
	    
	    slash = strrchr(dsPtr->string, '/');
	    if (slash != NULL) {
		Ns_DStringTrunc(dsPtr, slash - dsPtr->string);
	    }
	} else if (part[0] != '\0' &&
		   (part[0] != '.' || part[1] != '\0')) {

	    /*
	     * There's something non-null and not ".".
	     */

	    Ns_DStringNAppend(dsPtr, "/", 1);
	    Ns_DStringAppend(dsPtr, part);
	}
    } while (end != '\0');

    /*
     * If what remains is an empty string, change it to "/".
     */

    if (dsPtr->string[0] == '\0') {
	Ns_DStringNAppend(dsPtr, "/", 1);
    }
    Ns_DStringFree(&tmp);

    return dsPtr->string;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MakePath --
 *
 *	Append all the elements together with slashes between them. 
 *	Stop at NULL. 
 *
 * Results:
 *	dest->string 
 *
 * Side effects:
 *	Will append to dest. 
 *
 *----------------------------------------------------------------------
 */

char *
Ns_MakePath(Ns_DString *dest, ...)
{
    va_list  ap;
    char    *path;

    va_start(ap, dest);
    path = MakePath(dest, &ap);
    va_end(ap);
    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HashPath --
 *
 *      Hash the leading characters of string into a path, skipping
 *      periods and slashes. If string contains less characters than
 *      levels requested, '_' characters are used as padding.
 *
 *      For example, given the string 'foo' and the levels 2, 3, 4:
 *
 *          foo, 2 -> /f/o
 *          foo, 3 -> /f/o/o
 *          foo, 4 -> /f/o/o/_
 *
 * Results:
 *      dest->string 
 *
 * Side effects:
 *      Will append to dest.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_HashPath(Ns_DString *dest, char *string, int levels)
{
    char *p = string;
    int   i;

    for (i = 0; i < levels; ++i) {
        if (dest->string[dest->length] != '/') {
            Ns_DStringNAppend(dest, "/", 1);
        }
        while (*p == '.' || isslash(*p)) {
            ++p;
        }
        if (*p != '\0') {
            Ns_DStringNAppend(dest, p, 1);
            p++;
        } else {
            Ns_DStringNAppend(dest, "_", 1);
        }
    }

    return Ns_DStringValue(dest);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LibPath --
 *
 *	Returns the path where AOLserver libraries exist, with 
 *	varargs appended to it with slashes between each, stopping at 
 *	null arg. 
 *
 * Results:
 *	dest->string
 *
 * Side effects:
 *	Appends to dest. 
 *
 *----------------------------------------------------------------------
 */

char *
Ns_LibPath(Ns_DString *dest, ...)
{
    va_list  ap;
    char    *path;

    Ns_MakePath(dest, Ns_InfoHomePath(), "lib", NULL);
    va_start(ap, dest);
    path = MakePath(dest, &ap);
    va_end(ap);

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HomePath --
 *
 *	Build a path relative to AOLserver's home dir. 
 *
 * Results:
 *	dest->string 
 *
 * Side effects:
 *	Appends to dest. 
 *
 *----------------------------------------------------------------------
 */

char *
Ns_HomePath(Ns_DString *dest, ...)
{
    va_list  ap;
    char    *path;

    Ns_MakePath(dest, Ns_InfoHomePath(), NULL);
    va_start(ap, dest);
    path = MakePath(dest, &ap);
    va_end(ap);

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ServerPath --
 *
 *      Build a path relative to the server root dir.
 *
 * Results:
 *      dest->string or NULL on error.
 *
 * Side effects:
 *      See ServerRoot().
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ServerPath(Ns_DString *dest, char *server, ...)
{
    NsServer *servPtr;
    va_list   ap;
    char     *path;

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        return NULL;
    }
    ServerRoot(dest, servPtr, NULL);
    va_start(ap, server);
    path = MakePath(dest, &ap);
    va_end(ap);

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_PagePath --
 *
 *      Build a path relative to the server pages dir.
 *
 * Results:
 *      dest->string or NULL on error.
 *
 * Side effects:
 *      See ServerRoot().
 *
 *----------------------------------------------------------------------
 */

char *
Ns_PagePath(Ns_DString *dest, char *server, ...)
{
    NsServer *servPtr;
    va_list   ap;
    char     *path;

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        return NULL;
    }
    NsPageRoot(dest, servPtr, NULL);
    va_start(ap, server);
    path = MakePath(dest, &ap);
    va_end(ap);

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModulePath --
 *
 *	Append a path to dest:
 *	server-home/?servers/hserver?/?modules/hmodule?/...
 *	server and module may both be null.
 *
 * Results:
 *	dest->string 
 *
 * Side effects:
 *	Appends to dest. 
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ModulePath(Ns_DString *dest, char *server, char *module, ...)
{
    va_list         ap;
    char           *path;

    Ns_MakePath(dest, Ns_InfoHomePath(), NULL);
    if (server != NULL) {
       Ns_MakePath(dest, "servers", server, NULL);
    }
    if (module != NULL) {
       Ns_MakePath(dest, "modules", module, NULL);
    }
    va_start(ap, module);
    path = MakePath(dest, &ap);
    va_end(ap);
    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetServerRootProc --
 *
 *      Set pointer to custom procedure that returns the root dir path
 *      for a server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SetServerRootProc(Ns_ServerRootProc *proc, void *arg)
{
    NsServer *servPtr = NsGetInitServer();

    if (servPtr == NULL) {
        Ns_Log(Error, "Ns_SetServerRootProc: no initializing server");
        return NS_ERROR;
    }
    servPtr->vhost.serverRootProc = proc;
    servPtr->vhost.serverRootArg = arg;

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsPageRoot --
 *
 *      Return the path to the server pages directory.
 *
 * Results:
 *      dest->string.
 *
 * Side effects:
 *      See ServerRoot().
 *
 *----------------------------------------------------------------------
 */

char *
NsPageRoot(Ns_DString *dest, NsServer *servPtr, char *host)
{
    char *path;

    if (Ns_PathIsAbsolute(servPtr->fastpath.pagedir)) {
        path = Ns_DStringAppend(dest, servPtr->fastpath.pagedir);
    } else {
        ServerRoot(dest, servPtr, host);
        path = Ns_MakePath(dest, servPtr->fastpath.pagedir, NULL);
    }

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHashPathObjCmd --
 *
 *      Implements ns_hashpath obj command; a wrapper for Ns_HashPath.
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
NsTclHashPathObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_DString  path;
    int         levels;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "string levels");
        return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[2], &levels) != TCL_OK
        || levels <= 0) {

        Tcl_SetResult(interp, "levels must be an interger greater than zero",
                      TCL_STATIC);
        return TCL_ERROR;
    }
    Ns_DStringInit(&path);
    Ns_HashPath(&path, Tcl_GetString(objv[1]), levels);
    Tcl_DStringResult(interp, &path);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclModulePathObjCmd --
 *
 *	Implements ns_modulepath command; basically a wrapper around 
 *	Ns_ModulePath. 
 *
 * Results:
 *	Tcl result. 
 *
 * Side effects:
 *	None (deprecated) 
 *
 *----------------------------------------------------------------------
 */

int
NsTclModulePathObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
		      Tcl_Obj *CONST objv[])
{
    Ns_DString      ds;
    int		    i;
    char	   *module;

    Ns_DStringInit(&ds);
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "server ?module ...?");
        return TCL_ERROR;
    }
    module = objc > 2 ? Tcl_GetString(objv[2]) : NULL;
    Ns_ModulePath(&ds, Tcl_GetString(objv[1]), module, NULL);
    for (i = 3; i < objc; ++i) {
	Ns_MakePath(&ds, Tcl_GetString(objv[i]), NULL);
    }
    Tcl_DStringResult(interp, &ds);
    Ns_DStringFree(&ds);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclServerPathObjCmd, NsTclPagePathObjCmd --
 *
 *      Implements ns_serverpath, ns_pagepath commands.
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
NsTclServerPathObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return PathObjCmd(arg, interp, objc, objv, 's');
}

int
NsTclPagePathObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    return PathObjCmd(arg, interp, objc, objv, 'p');
}

static int
PathObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[], int cmd)
{
    NsInterp    *itPtr = arg;
    NsServer    *servPtr;
    Ns_DString   ds;
    char        *path, *host = NULL;
    int          i, npaths = 0;

    Ns_ObjvSpec opts[] = {
        {"-host", Ns_ObjvString, &host, NULL},
        {"--",    Ns_ObjvBreak,  NULL,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?path", Ns_ObjvArgs, &npaths, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if ((servPtr = itPtr->servPtr) == NULL
        && (servPtr = NsGetInitServer()) == NULL) {

        Tcl_SetResult(interp, "no server available", TCL_STATIC);
        return TCL_ERROR;
    }

    Ns_DStringInit(&ds);
    if (cmd == 'p') {
        path = NsPageRoot(&ds, itPtr->servPtr, host);
    } else {
        path = ServerRoot(&ds, itPtr->servPtr, host);
    }
    for (i = objc - npaths; i < objc; ++i) {
        Ns_MakePath(&ds, Tcl_GetString(objv[i]), NULL);
    }
    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclServerRootProcObjCmd --
 *
 *      Implements the ns_serverrootproc command.
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
NsTclServerRootProcObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    NsServer        *servPtr = NsGetInitServer();
    Ns_TclCallback  *cbPtr;

    if (objc != 2 && objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?arg?");
        return TCL_ERROR;
    }
    if (servPtr == NULL) {
        Tcl_AppendResult(interp, "no initializing server", TCL_STATIC);
        return TCL_ERROR;
    }
    cbPtr = Ns_TclNewCallbackObj(interp, NsTclServerRoot,
                                 objv[1], (objc > 2) ? objv[2] : NULL);
    Ns_SetServerRootProc(NsTclServerRoot, cbPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclServerRoot --
 *
 *      Tcl callback to build a path to the server root dir.
 *
 * Results:
 *      dest->string or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
NsTclServerRoot(Ns_DString *dest, char *host, void *arg)
{
    Ns_TclCallback *cbPtr = arg;

    if (Ns_TclEvalCallback(NULL, cbPtr, dest, host, NULL) != NS_OK) {
        return NULL;
    }
    return Ns_DStringValue(dest);
}


/*
 *----------------------------------------------------------------------
 *
 * MakePath --
 *
 *	Append the args with slashes between them to dest. 
 *
 * Results:
 *	dest->string 
 *
 * Side effects:
 *	None. 
 *
 *----------------------------------------------------------------------
 */

static char *
MakePath(Ns_DString *dest, va_list *pap)
{
    char *s;
    int len;

    while ((s = va_arg(*pap, char *)) != NULL) {
        if (isalpha(*s) && s[1] == ':') {
            char temp = *(s+2);
            *(s + 2) = 0;
            Ns_DStringNAppend(dest, s, 2);
            *(s + 2) = temp;
            s += 2;
        }
	while (*s) {
	    while (isslash(*s)) {
	        ++s;
	    }
	    if (*s) {
	    	Ns_DStringNAppend(dest, "/", 1);
		len = 0;
		while (s[len] != '\0' && !isslash(s[len])) {
		    ++len;
		}
	    	Ns_DStringNAppend(dest, s, len);
	    	s += len;
	    }
	}
    }
    return dest->string;
}


/*
 *----------------------------------------------------------------------
 * ServerRoot --
 *
 *      Return the path to the server root directory.  If virtual
 *      hosting is enabled then the host header is used to construct the
 *      path.  If host is given it overides the Host header of the
 *      current connection.
 *
 * Results:
 *      dest->string.
 *
 * Side effects:
 *      Value of Host header may be bashed to lower case.
 *      Depends on registered Ns_ServerRootProc, if any.
 *
 *----------------------------------------------------------------------
 */

static char *
ServerRoot(Ns_DString *dest, NsServer *servPtr, char *host)
{
    char     *p, *path, *port = NULL;
    Ns_Conn  *conn;
    Ns_Set   *headers;

    if (servPtr->vhost.serverRootProc != NULL) {

        /*
         * Prefer to run a user-registered Ns_ServerRootProc.
         */

        path = (servPtr->vhost.serverRootProc)(dest, host, servPtr->vhost.serverRootArg);
        if (path == NULL) {
            goto defpath;
        }

    } else if (servPtr->vhost.enabled
               && (host != NULL
                   || ((conn = Ns_GetConn()) != NULL
                       && (headers = Ns_ConnHeaders(conn)) != NULL
                       && (host = Ns_SetIGet(headers, "Host")) != NULL))
               && *host != '\0') {

        /*
         * Bail out if there are suspicious characters.
         */

        for (p = host; *p != '\0'; p++) {
            if (isslash(*p) || isspace(*p) || (p[0] == '.' && p[1] == '.')) {
                goto defpath;
            }
        }
        host = Ns_StrToLower(host);

        path = Ns_MakePath(dest, servPtr->fastpath.serverdir,
                           servPtr->vhost.hostprefix, NULL);

        if (servPtr->vhost.opts & NSD_STRIP_WWW) {
            if (strncmp(host, "www.", 4) == 0) {
                host = &host[4];
            }
        }
        if (servPtr->vhost.opts & NSD_STRIP_PORT) {
            if ((port = strrchr(host, ':')) != NULL) {
                *port = '\0';
            }
        }

        if (servPtr->vhost.hosthashlevel > 0) {
            Ns_HashPath(dest, host, servPtr->vhost.hosthashlevel);
        }
        Ns_NormalizePath(dest, host);

        if (port != NULL) {
            *port = ':';
        }

    } else {

        /*
         * Default to static server root.
         */

    defpath:
        path = Ns_MakePath(dest, servPtr->fastpath.serverdir, NULL);
    }

    return path;
}
