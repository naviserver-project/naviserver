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
 * pathname.c --
 *
 *  Functions that manipulate or return paths.
 */

#include "nsd.h"

#define ISSLASH(c)  ((c) == '/' || (c) == '\\')

#define NSD_STRIP_WWW                  0x01u
#define NSD_STRIP_PORT                 0x02u

/*
 * Local functions defined in this file.
 */

static Ns_ServerInitProc ConfigServerVhost;
static int ConfigServerVhost(const char *server)
    NS_GNUC_NONNULL(1);

static int PathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, char cmd)
    NS_GNUC_NONNULL(2);
static char *MakePath(Ns_DString *dest, va_list *pap)
    NS_GNUC_NONNULL(1);
static const char *ServerRoot(Ns_DString *dest, const NsServer *servPtr, const char *rawHost)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);



/*
 *----------------------------------------------------------------------
 *
 * NsConfigVhost() --
 *
 *      Configure virtual hosting parameters.
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
NsConfigVhost(void)
{
    NsRegisterServerInit(ConfigServerVhost);
}

static Ns_ReturnCode
ConfigServerVhost(const char *server)
{
    NsServer   *servPtr = NsGetServer(server);
    Ns_DString  ds;
    const char *path;

    NS_NONNULL_ASSERT(server != NULL);

    path = Ns_ConfigGetPath(server, NULL, "vhost", (char *)0);

    servPtr->vhost.enabled = Ns_ConfigBool(path, "enabled", NS_FALSE);
    if (servPtr->vhost.enabled
	&& Ns_PathIsAbsolute(servPtr->fastpath.pagedir) == NS_TRUE) {
	Ns_Log(Error, "vhost[%s]: disabled, pagedir not relative: %s",
               server, servPtr->fastpath.pagedir);
        servPtr->vhost.enabled = NS_FALSE;
    }
    if (Ns_ConfigBool(path, "stripwww", NS_TRUE)) {
        servPtr->vhost.opts |= NSD_STRIP_WWW;
    }
    if (Ns_ConfigBool(path, "stripport", NS_TRUE)) {
        servPtr->vhost.opts |= NSD_STRIP_PORT;
    }
    servPtr->vhost.hostprefix = Ns_ConfigGetValue(path, "hostprefix");
    servPtr->vhost.hosthashlevel =
        Ns_ConfigIntRange(path, "hosthashlevel", 0, 0, 5);

    if (servPtr->vhost.enabled) {
        Ns_DStringInit(&ds);
        (void) NsPageRoot(&ds, servPtr, "www.example.com:80");
        Ns_Log(Notice, "vhost[%s]: www.example.com:80 -> %s",server,ds.string);
        Ns_DStringFree(&ds);
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_PathIsAbsolute --
 *
 *  Boolean: is the path absolute?
 *
 * Results:
 *  NS_TRUE if it is, NS_FALSE if not.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_PathIsAbsolute(const char *path)
{
    NS_NONNULL_ASSERT(path != NULL);

#ifdef _WIN32
    if (CHARTYPE(alpha, *path) != 0 && path[1] == ':') {
        path += 2;
    }
#endif
    return (ISSLASH(*path));
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_NormalizePath --
 *
 *  Remove "..", "." from paths.
 *
 * Results:
 *  dsPtr->string
 *
 * Side effects:
 *  Will append to dsPtr. Assumes an absolute path.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_NormalizePath(Ns_DString *dsPtr, const char *path)
{
    char                 end;
    register char       *src;
    register const char *slash;
    Ns_DString           tmp;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(path != NULL);
    
    Ns_DStringInit(&tmp);
    src = Ns_DStringAppend(&tmp, path);
#ifdef _WIN32
    if (CHARTYPE(alpha, *src) != 0 && src[1] == ':') {
        if (CHARTYPE(upper, *src) != 0) {
	  *src = CHARCONV(lower, *src);
        }
        Ns_DStringNAppend(dsPtr, src, 2);
        src += 2;
    } else if (ISSLASH(src[0]) && ISSLASH(src[1])) {
        /*
         * We have TWO leading slashes as in the Windows pathname
         * "//machine/foo/bar".  The code further below will write 1
         * slash, so here, add just 1 slash so that we will end up
         * with 2 total: --atp@piskorski.com, 2005/03/14 06:34 EST
         */
	Ns_DStringNAppend(dsPtr, src, 1);
	src += 2;
    }
#endif

    /*
     * Move past leading slash(es)
     */

    while (ISSLASH(*src)) {
        ++src;
    }
    do {
	register const char *part = src;

        /*
         * Move to next slash
         */

        while (*src != '\0' && !ISSLASH(*src)) {
            ++src;
        }
        end = *src;
        *src++ = '\0';

        if (part[0] == '.' && part[1] == '.' && part[2] == '\0') {

            /*
             * There's a "..", so wipe out one path backwards.
             */

            slash = strrchr(dsPtr->string, INTCHAR('/'));
            if (slash != NULL) {
	      Ns_DStringSetLength(dsPtr, (int)(slash - dsPtr->string));
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
 *  Append all the elements together with slashes between them.
 *  Stop at NULL.
 *
 * Results:
 *  dest->string
 *
 * Side effects:
 *  Will append to dest.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_MakePath(Ns_DString *dsPtr, ...)
{
    va_list  ap;
    char    *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    va_start(ap, dsPtr);
    path = MakePath(dsPtr, &ap);
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
Ns_HashPath(Ns_DString *dsPtr, const char *path, int levels)
{
    const char *p = path;
    int         i;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(path != NULL);
    
    for (i = 0; i < levels; ++i) {
        if (dsPtr->string[dsPtr->length] != '/') {
            Ns_DStringNAppend(dsPtr, "/", 1);
        }
        while (*p == '.' || ISSLASH(*p)) {
            ++p;
        }
        if (*p != '\0') {
            Ns_DStringNAppend(dsPtr, p, 1);
            p++;
        } else {
            Ns_DStringNAppend(dsPtr, "_", 1);
        }
    }

    return Ns_DStringValue(dsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_LibPath --
 *
 *  Returns the path where server libraries exist, with
 *  varargs appended to it with slashes between each,
 *  stopping at null arg.
 *
 * Results:
 *  dest->string
 *
 * Side effects:
 *  Appends to dest.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_LibPath(Ns_DString *dsPtr, ...)
{
    va_list  ap;
    char    *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    Ns_MakePath(dsPtr, Ns_InfoHomePath(), "lib", (char *)0);
    va_start(ap, dsPtr);
    path = MakePath(dsPtr, &ap);
    va_end(ap);

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_BinPath --
 *
 *  Returns the path where server binaries exist, with
 *  varargs appended to it with slashes between each,
 *  stopping at null arg.
 *
 * Results:
 *  dest->string
 *
 * Side effects:
 *  Appends to dest.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_BinPath(Ns_DString *dsPtr, ...)
{
    va_list  ap;
    char    *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MakePath(dsPtr, Ns_InfoHomePath(), "bin", (char *)0);
    va_start(ap, dsPtr);
    path = MakePath(dsPtr, &ap);
    va_end(ap);

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HomePath --
 *
 *  Build a path relative to NaviServer's home dir.
 *
 * Results:
 *  dest->string
 *
 * Side effects:
 *  Appends to dest.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_HomePath(Ns_DString *dsPtr, ...)
{
    va_list  ap;
    char    *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MakePath(dsPtr, Ns_InfoHomePath(), (char *)0);
    va_start(ap, dsPtr);
    path = MakePath(dsPtr, &ap);
    va_end(ap);

    return path;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_HomePathExists --
 *
 *  Check that a path exists relative to NaviServer's home dir.
 *
 * Results:
 *  1 if exists
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */

bool
Ns_HomePathExists(const char *path, ...)
{
    va_list      ap;
    int          status;
    Tcl_Obj     *obj;
    Ns_DString   ds;
    Tcl_StatBuf *stPtr;

    NS_NONNULL_ASSERT(path != NULL);

    Ns_DStringInit(&ds);
    Ns_MakePath(&ds, Ns_InfoHomePath(), path, (char *)0);

    va_start(ap, path);
    MakePath(&ds, &ap);
    va_end(ap);

    obj = Tcl_NewStringObj(ds.string, -1);
    Tcl_IncrRefCount(obj);
    stPtr = Tcl_AllocStatBuf();
    status = Tcl_FSStat(obj, stPtr);
    Tcl_Free((char*)stPtr);
    Tcl_DecrRefCount(obj);
    Ns_DStringFree(&ds);

    return status == 0 ? NS_TRUE : NS_FALSE;
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
Ns_ServerPath(Ns_DString *dsPtr, const char *server, ...)
{
    const NsServer *servPtr;
    char           *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);
    
    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        path = NULL;

    } else {
        va_list ap;

        (void) ServerRoot(dsPtr, servPtr, NULL);
        va_start(ap, server);
        path = MakePath(dsPtr, &ap);
        va_end(ap);
    }

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
Ns_PagePath(Ns_DString *dsPtr, const char *server, ...)
{
    const NsServer *servPtr;
    char           *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(server != NULL);

    servPtr = NsGetServer(server);
    if (servPtr == NULL) {
        path = NULL;
  
    } else {
        va_list         ap;

        (void) NsPageRoot(dsPtr, servPtr, NULL);
        va_start(ap, server);
        path = MakePath(dsPtr, &ap);
        va_end(ap);
    }

    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModulePath --
 *
 *  Append a path to dest:
 *  server-home/?servers/hserver?/?modules/hmodule?/...
 *  server and module may both be null.
 *
 * Results:
 *  dest->string
 *
 * Side effects:
 *  Appends to dest.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_ModulePath(Ns_DString *dsPtr, const char *server, const char *module, ...)
{
    va_list         ap;
    char           *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    Ns_MakePath(dsPtr, Ns_InfoHomePath(), (char *)0);
    if (server != NULL) {
       Ns_MakePath(dsPtr, "servers", server, (char *)0);
    }
    if (module != NULL) {
       Ns_MakePath(dsPtr, "modules", module, (char *)0);
    }
    va_start(ap, module);
    path = MakePath(dsPtr, &ap);
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
 *      Result code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_SetServerRootProc(Ns_ServerRootProc *proc, void *arg)
{
    NsServer      *servPtr = NsGetInitServer();
    Ns_ReturnCode  status = NS_OK;

    if (unlikely(servPtr == NULL)) {
        Ns_Log(Error, "Ns_SetServerRootProc: no initializing server");
        status = NS_ERROR;
    } else {
        servPtr->vhost.serverRootProc = proc;
        servPtr->vhost.serverRootArg = arg;
    }

    return status;
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
NsPageRoot(Ns_DString *dsPtr, const NsServer *servPtr, const char *host)
{
    char *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);

    if (Ns_PathIsAbsolute(servPtr->fastpath.pagedir) == NS_TRUE) {
        path = Ns_DStringAppend(dsPtr, servPtr->fastpath.pagedir);
    } else {
	(void) ServerRoot(dsPtr, servPtr, host);
        path = Ns_MakePath(dsPtr, servPtr->fastpath.pagedir, (char *)0);
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
NsTclHashPathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_DString  path;
    int         levels, result = TCL_OK;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "string levels");
        result = TCL_ERROR;
        
    } else if (Tcl_GetIntFromObj(interp, objv[2], &levels) != TCL_OK
        || levels <= 0) {
        Ns_TclPrintfResult(interp, "levels must be an integer greater than zero");
        result = TCL_ERROR;
        
    } else {
        Ns_DStringInit(&path);
        Ns_HashPath(&path, Tcl_GetString(objv[1]), levels);
        Tcl_DStringResult(interp, &path);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclModulePathObjCmd --
 *
 *  Implements ns_modulepath command; basically a wrapper around
 *  Ns_ModulePath.
 *
 * Results:
 *  Tcl result.
 *
 * Side effects:
 *  None (deprecated)
 *
 *----------------------------------------------------------------------
 */

int
NsTclModulePathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result = TCL_OK;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "server ?module ...?");
        result = TCL_ERROR;
        
    } else {
        Ns_DString  ds;
        int         i;
        const char *module = objc > 2 ? Tcl_GetString(objv[2]) : NULL;

        Ns_DStringInit(&ds);        
        /* 
         * Use (char *)0 as sentinel instead of NULL to make the function
         * portable. Cppcheck showed this problem in a first step.  See
         * e.g. http://ewontfix.com/11/.
         */
        Ns_ModulePath(&ds, Tcl_GetString(objv[1]), module, (char *)0);
        for (i = 3; i < objc; ++i) {
            Ns_MakePath(&ds, Tcl_GetString(objv[i]), (char *)0);
        }
        Tcl_DStringResult(interp, &ds);
    }

    return result;
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
NsTclServerPathObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return PathObjCmd(clientData, interp, objc, objv, 's');
}

int
NsTclPagePathObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    return PathObjCmd(clientData, interp, objc, objv, 'p');
}

static int
PathObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv, char cmd)
{
    char *host = NULL;
    int   npaths = 0, result = TCL_OK;

    Ns_ObjvSpec opts[] = {
        {"-host", Ns_ObjvString, &host, NULL},
        {"--",    Ns_ObjvBreak,  NULL,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?path", Ns_ObjvArgs, &npaths, NULL},
        {NULL, NULL, NULL, NULL}
    };
    NS_NONNULL_ASSERT(interp != NULL);

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const NsServer *servPtr;
        const NsInterp *itPtr = clientData;
    
        servPtr = itPtr->servPtr;
        if (servPtr == NULL) {
            servPtr = NsGetInitServer();
        }
        if (servPtr == NULL) {
            Tcl_SetResult(interp, (char *)"no server available", TCL_STATIC);
            result = TCL_ERROR;
            
        } else {
            Ns_DString      ds;
            int             i;

            Ns_DStringInit(&ds);
            if (cmd == 'p') {
                (void) NsPageRoot(&ds, servPtr, host);
            } else {
                (void) ServerRoot(&ds, servPtr, host);
            }
            for (i = objc - npaths; i < objc; ++i) {
                Ns_MakePath(&ds, Tcl_GetString(objv[i]), (char *)0);
            }
            Tcl_DStringResult(interp, &ds);
        }
    }
    
    return result;
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
NsTclServerRootProcObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsServer *servPtr = NsGetInitServer();
    int             result = TCL_OK;

    if (unlikely(objc < 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?args?");
        result = TCL_ERROR;
        
    } else if (unlikely(servPtr == NULL)) {
        Ns_TclPrintfResult(interp, "no initializing server");
        result = TCL_ERROR;
        
    } else {
        Ns_TclCallback *cbPtr;
            
        cbPtr = Ns_TclNewCallback(interp, (Ns_Callback *)NsTclServerRoot, objv[1],
                                  objc - 2, objv + 2);
        if (unlikely(Ns_SetServerRootProc(NsTclServerRoot, cbPtr) != NS_OK)) {
            result = TCL_ERROR;
        }
    }

    return result;
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

const char *
NsTclServerRoot(Ns_DString *dest, const char *host, const void *arg)
{
    const Ns_TclCallback *cbPtr = arg;
    const char           *result = NULL;

    if (Ns_TclEvalCallback(NULL, cbPtr, dest, host, NULL) == TCL_OK) {
        result = Ns_DStringValue(dest);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * MakePath --
 *
 *  Append the args with slashes between them to dest.
 *
 * Results:
 *  dest->string
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static char *
MakePath(Ns_DString *dest, va_list *pap)
{
    char *s;
    int len;

    NS_NONNULL_ASSERT(dest != NULL);

    for (s = va_arg(*pap, char *); s != NULL; s = va_arg(*pap, char *)) {
        if ((CHARTYPE(alpha, *s) != 0) && (s[1] == ':')) {
            char temp = *(s+2);

            *(s + 2) = '\0';
            Ns_DStringNAppend(dest, s, 2);
            *(s + 2) = temp;
            s += 2;
        }
        while (*s != '\0') {
            while (ISSLASH(*s)) {
                ++s;
            }
            if (*s != '\0') {
                Ns_DStringNAppend(dest, "/", 1);
                len = 0;
                while (s[len] != '\0' && !ISSLASH(s[len])) {
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
static const char *
ServerRoot(Ns_DString *dest, const NsServer *servPtr, const char *rawHost)
{
    char           *safehost;
    const char     *path;
    const Ns_Conn  *conn;
    const Ns_Set   *headers;
    Ns_DString      ds;

    NS_NONNULL_ASSERT(dest != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);

    if (servPtr->vhost.serverRootProc != NULL) {

        /*
         * Prefer to run a user-registered Ns_ServerRootProc.
         */

        path = (servPtr->vhost.serverRootProc)(dest, rawHost, servPtr->vhost.serverRootArg);
        if (path == NULL) {
            goto defpath;
        }

    } else if (servPtr->vhost.enabled
               && (rawHost != NULL
                   || ((conn = Ns_GetConn()) != NULL
                       && (headers = Ns_ConnHeaders(conn)) != NULL
                       && (rawHost = Ns_SetIGet(headers, "Host")) != NULL))
               && *rawHost != '\0') {

        /*
         * Bail out if there are suspicious characters in the unprocessed Host.
         */

        if (!Ns_StrIsHost(rawHost)) {
            goto defpath;
        }

        /*
         * Normalize the Host string.
         */

        Ns_DStringInit(&ds);
        safehost = Ns_DStringAppend(&ds, rawHost);

        (void) Ns_StrToLower(safehost);
        if ((servPtr->vhost.opts & NSD_STRIP_WWW) != 0u
            && strncmp(safehost, "www.", 4u) == 0) {
            safehost = &safehost[4];
        }
        if ((servPtr->vhost.opts & NSD_STRIP_PORT) != 0u) {
            char *p = strrchr(safehost, INTCHAR(':'));
            if (p != NULL) {
                *p = '\0';
            }
        }

        /*
         * Build the final path.
         */

        path = Ns_MakePath(dest, servPtr->fastpath.serverdir,
                           servPtr->vhost.hostprefix, (char *)0);
        if (servPtr->vhost.hosthashlevel > 0) {
            Ns_HashPath(dest, safehost, servPtr->vhost.hosthashlevel);
        }
        Ns_NormalizePath(dest, safehost);
        Ns_DStringFree(&ds);

    } else {

        /*
         * Default to static server root.
         */

    defpath:
        path = Ns_MakePath(dest, servPtr->fastpath.serverdir, (char *)0);
    }

    return path;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
