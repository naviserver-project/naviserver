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

static int PathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, char cmd)
    NS_GNUC_NONNULL(2);

static char *MakePath(Tcl_DString *dest, va_list *pap)
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

static const char *ServerRoot(Tcl_DString *dest, const NsServer *servPtr, const char *rawHost)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_RETURNS_NONNULL;

static const char *NormalizePath(Tcl_DString *dsPtr, const char *path, bool url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_RETURNS_NONNULL;

static bool IsSlashInPath(bool inUrl, const char c)
    NS_GNUC_PURE;

/*
 *----------------------------------------------------------------------
 *
 * IsSlashInPath() --
 *
 *      Should a chacter in a path treated as as slash? There are different
 *      semantics for URLs and file paths.
 *
 * Results:
 *      Boolean value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
IsSlashInPath(bool inUrl, const char c) {
    return inUrl ? (c == '/') : ISSLASH(c);
}

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
    NsServer     *servPtr;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(server != NULL);

    servPtr = NsGetServer(server);
    if (unlikely(servPtr == NULL)) {
        Ns_Log(Warning, "Could configure vhost; server '%s' unknown", server);
        result = NS_ERROR;

    } else {
        Tcl_DString ds;
        const char *section;

        assert(servPtr->fastpath.pagedir != NULL);

        section = Ns_ConfigGetPath(server, NULL, "vhost", NS_SENTINEL);

        servPtr->vhost.enabled = Ns_ConfigBool(section, "enabled", NS_FALSE);
        if (servPtr->vhost.enabled
            && Ns_PathIsAbsolute(servPtr->fastpath.pagedir) == NS_TRUE) {
            Ns_Log(Error, "vhost[%s]: disabled, pagedir not relative: %s",
                   server, servPtr->fastpath.pagedir);
            servPtr->vhost.enabled = NS_FALSE;
        }
        if (Ns_ConfigBool(section, "stripwww", NS_TRUE)) {
            servPtr->vhost.opts |= NSD_STRIP_WWW;
        }
        if (Ns_ConfigBool(section, "stripport", NS_TRUE)) {
            servPtr->vhost.opts |= NSD_STRIP_PORT;
        }
        servPtr->vhost.hostprefix = ns_strcopy(Ns_ConfigString(section, "hostprefix", NULL));
        servPtr->vhost.hosthashlevel =
            Ns_ConfigIntRange(section, "hosthashlevel", 0, 0, 5);

        if (servPtr->vhost.enabled) {
            Tcl_DStringInit(&ds);
            (void) NsPageRoot(&ds, servPtr, "www.example.com:80");
            Ns_Log(Notice, "vhost[%s]: www.example.com:80 -> %s", server, ds.string);
            Tcl_DStringFree(&ds);
        }
        result = NS_OK;
    }

    return result;
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
 * Ns_NormalizePath, Ns_NormalizeUrl --
 *
 *  Remove "..", ".", multiple consecutive slashes from paths.
 *  While Ns_NormalizePath is designed for filesystem paths
 *  including special treatment for windows, the latter is defined
 *  for normalizing URLs.
 *
 * Results:
 *  dsPtr->string
 *
 * Side effects:
 *  Will append to dsPtr. Assumes an absolute path.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_NormalizePath(Tcl_DString *dsPtr, const char *path)
{
    return NormalizePath(dsPtr, path, NS_FALSE);
}

const char *
Ns_NormalizeUrl(Tcl_DString *dsPtr, const char *path)
{
    return NormalizePath(dsPtr, path, NS_TRUE);
}

const char *
NormalizePath(Tcl_DString *dsPtr, const char *path, bool url)
{
    char                 end;
    register char       *src;
    register const char *slash;
    Tcl_DString          tmp;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(path != NULL);

    Tcl_DStringInit(&tmp);
    src = Tcl_DStringAppend(&tmp, path, TCL_INDEX_NONE);

    if (!url) {
#ifdef _WIN32
        if (CHARTYPE(alpha, *src) != 0 && src[1] == ':') {
            if (CHARTYPE(upper, *src) != 0) {
                *src = CHARCONV(lower, *src);
            }
            Tcl_DStringAppend(dsPtr, src, 2);
            src += 2;
        } else if (ISSLASH(src[0]) && ISSLASH(src[1])) {
            /*
             * We have TWO leading slashes as in the Windows pathname
             * "//machine/foo/bar".  The code further below will write 1
             * slash, so here, add just 1 slash so that we will end up
             * with 2 total: --atp@piskorski.com, 2005/03/14 06:34 EST
             */
            Tcl_DStringAppend(dsPtr, src, 1);
            src += 2;
        }
#endif
    }

    /*
     * Move past leading slash(es)
     */
    while (IsSlashInPath(url, *src)) {
        ++src;
    }

    do {
        const char *part = src;

        /*
         * Move to next slash
         */
        while (*src != '\0' && !IsSlashInPath(url, *src)) {
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
                Tcl_DStringSetLength(dsPtr, (TCL_SIZE_T)(slash - dsPtr->string));
            }
        } else if (part[0] != '\0' &&
               (part[0] != '.' || part[1] != '\0')) {

            /*
             * There's something non-null and not ".".
             */

            Tcl_DStringAppend(dsPtr, "/", 1);
            Tcl_DStringAppend(dsPtr, part, TCL_INDEX_NONE);
        }
    } while (end != '\0');

    /*
     * If what remains is an empty string, change it to "/".
     */

    if (dsPtr->string[0] == '\0') {
        Tcl_DStringAppend(dsPtr, "/", 1);
    }
    Tcl_DStringFree(&tmp);

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

const char *
Ns_MakePath(Tcl_DString *dsPtr, ...)
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

const char *
Ns_HashPath(Tcl_DString *dsPtr, const char *path, int levels)
{
    const char *p = path;
    int         i;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(path != NULL);

    for (i = 0; i < levels; ++i) {
        if (dsPtr->string[dsPtr->length] != '/') {
            Tcl_DStringAppend(dsPtr, "/", 1);
        }
        while (*p == '.' || ISSLASH(*p)) {
            ++p;
        }
        if (*p != '\0') {
            Tcl_DStringAppend(dsPtr, p, 1);
            p++;
        } else {
            Tcl_DStringAppend(dsPtr, "_", 1);
        }
    }

    return dsPtr->string;
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

const char *
Ns_LibPath(Tcl_DString *dsPtr, ...)
{
    va_list  ap;
    char    *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MakePath(dsPtr, Ns_InfoHomePath(), "lib", NS_SENTINEL);
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

const char *
Ns_BinPath(Tcl_DString *dsPtr, ...)
{
    va_list  ap;
    char    *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MakePath(dsPtr, nsconf.binDir, NS_SENTINEL);
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

const char *
Ns_HomePath(Tcl_DString *dsPtr, ...)
{
    va_list  ap;
    char    *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MakePath(dsPtr, Ns_InfoHomePath(), NS_SENTINEL);
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
 *  NS_TRUE if path exists.
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
    Tcl_DString  ds;
    Tcl_StatBuf *stPtr;

    NS_NONNULL_ASSERT(path != NULL);

    Tcl_DStringInit(&ds);
    Ns_MakePath(&ds, Ns_InfoHomePath(), path, NS_SENTINEL);

    va_start(ap, path);
    MakePath(&ds, &ap);
    va_end(ap);

    obj = Tcl_NewStringObj(ds.string, ds.length);
    Tcl_IncrRefCount(obj);
    stPtr = Tcl_AllocStatBuf();
    status = Tcl_FSStat(obj, stPtr);
    Tcl_Free((char*)stPtr);
    Tcl_DecrRefCount(obj);
    Tcl_DStringFree(&ds);

    return (status == 0);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RequireDirectory --
 *
 *      Ensures that the specified directory exists. If it does not
 *      exist, the function attempts to create it. If creation fails,
 *      an error is logged and NS_ERROR is returned; otherwise, NS_OK
 *      is returned.
 *
 * Results:
 *      NS_OK or NS_ERROR
 *
 * Side effects:
 *      May create the directory if it does not already exist.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_RequireDirectory(const char *path)
{
    Ns_ReturnCode result = TCL_OK;
    struct stat   fileStat;

    NS_NONNULL_ASSERT(path != NULL);

    if (!Ns_Stat(path, &fileStat)) {
        Tcl_Obj *pathObj;
        int      rc;

        pathObj = Tcl_NewStringObj(path, TCL_INDEX_NONE);
        Tcl_IncrRefCount(pathObj);
        rc = Tcl_FSCreateDirectory(pathObj);
        Tcl_DecrRefCount(pathObj);

        if (rc != TCL_OK && Tcl_GetErrno() != EEXIST && Tcl_GetErrno() != EISDIR) {
            Ns_Log(Error, "nslog: create directory (%s) failed: '%s'",
                   path, strerror(Tcl_GetErrno()));
            result = NS_ERROR;
        }
    }
    return result;
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

const char *
Ns_ServerPath(Tcl_DString *dsPtr, const char *server, ...)
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

const char *
Ns_PagePath(Tcl_DString *dsPtr, const char *server, ...)
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
 * Ns_LogPath --
 *
 *      Constructs and returns the absolute path for a log file for a given server.
 *      If the provided filename is absolute, it is returned unchanged.
 *
 *      If the filename is relative and server root processing is enabled for
 *      the specified server, the function retrieves the server-specific log
 *      directory from the configuration (falling back to the global
 *      "ns/parameters" logdir if necessary).  It then constructs the full
 *      path by appending the filename to the resolved log directory. The
 *      function logs details about whether a relative or absolute server log
 *      directory was used.
 *
 *      If server root processing is not enabled, the function builds the log path using
 *      the default log directory obtained via Ns_ServerLogDir().

 *
 * Parameters:
 *      dsPtr       - Pointer to a Tcl_DString used as a buffer for
 *                    constructing the path.
 *      server      - Name of the server.
 *      configValue - Configured log directory value (may be relative or absolute).
 *
 * Results:
 *      Returns a pointer to a null-terminated string containing the resolved
 *      absolute log directory.
 *
 * Side Effects:
 *      The Tcl_DString referenced by dsPtr may be modified during the
 *      construction of the path.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_LogPath(Tcl_DString *dsPtr, const char *server, const char *filename)
{
    const char *result;

    if (Ns_PathIsAbsolute(filename)) {
        result = filename;

    } else if (Ns_ServerRootProcEnabled(server)) {
        const char *value;
        const char *section = Ns_ConfigSectionPath(NULL, server, NULL, NS_SENTINEL);
        const char *serverLogDir = Ns_ConfigGetValue(section, "logdir");
        Tcl_DString message;

        Tcl_DStringInit(&message);

        if (serverLogDir == NULL) {
            serverLogDir = Ns_ConfigGetValue("ns/parameters", "logdir");
            Ns_DStringPrintf(&message, "use global logdir <%s> ", serverLogDir);
        }
        if (!Ns_PathIsAbsolute(serverLogDir)) {
            Tcl_DString scratch;
            /*
             * Serverroot + relative server log + filename
             */
            Tcl_DStringInit(&scratch);
            value = Ns_MakePath(dsPtr, Ns_ServerPath(&scratch, server, NS_SENTINEL),
                                serverLogDir, filename, NS_SENTINEL);
            Tcl_DStringFree(&scratch);
            Ns_DStringPrintf(&message, "relative server logdir '%s' ", serverLogDir);
        } else {
            /*
             * Absolute server log + filename
             */
            value = Ns_MakePath(dsPtr, serverLogDir, filename, NS_SENTINEL);
            Ns_DStringPrintf(&message, "absolute server logdir '%s'", serverLogDir);
        }
        Ns_Log(Debug, "Ns_LogPath %s --> %s", message.string, value);
        Tcl_DStringFree(&message);

        result = value;

    } else {
        result = Ns_MakePath(dsPtr, Ns_ServerLogDir(server), filename, NS_SENTINEL);
    }

    return result;
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

const char *
Ns_ModulePath(Tcl_DString *dsPtr, const char *server, const char *module, ...)
{
    va_list         ap;
    char           *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MakePath(dsPtr, Ns_InfoHomePath(), NS_SENTINEL);
    if (server != NULL) {
       Ns_MakePath(dsPtr, "servers", server, NS_SENTINEL);
    }
    if (module != NULL) {
       Ns_MakePath(dsPtr, "modules", module, NS_SENTINEL);
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
        if (servPtr->vhost.serverRootArg != NULL) {
            Ns_TclFreeCallback(servPtr->vhost.serverRootArg);
        }
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

const char *
NsPageRoot(Tcl_DString *dsPtr, const NsServer *servPtr, const char *host)
{
    const char *path;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);

    assert(servPtr->fastpath.pagedir != NULL);

    if (Ns_PathIsAbsolute(servPtr->fastpath.pagedir) == NS_TRUE) {
        Ns_Log(Debug, "NsPageRoot is absolute <%s>", servPtr->fastpath.pagedir);
        path = Tcl_DStringAppend(dsPtr, servPtr->fastpath.pagedir, TCL_INDEX_NONE);
    } else {
        (void) ServerRoot(dsPtr, servPtr, host);
        Ns_Log(Debug, "NsPageRoot is not absolute <%s>, ServerRoot <%s>",
               servPtr->fastpath.pagedir, dsPtr->string);
        path = Ns_MakePath(dsPtr, servPtr->fastpath.pagedir, NS_SENTINEL);
    }

    //Ns_Log(Notice, "--- NsPagerRoot returns path <%s>", path);
    return path;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHashPathObjCmd --
 *
 *      Implements "ns_hashpath". a wrapper for Ns_HashPath.
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
NsTclHashPathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int               levels = 1, result = TCL_OK;
    char             *inputString;
    Ns_ObjvValueRange range = {1, INT_MAX};
    Ns_ObjvSpec       args[] = {
        {"string", Ns_ObjvString, &inputString, NULL},
        {"levels", Ns_ObjvInt,    &levels,     &range},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString path;

        Tcl_DStringInit(&path);
        Ns_HashPath(&path, inputString, levels);
        Tcl_DStringResult(interp, &path);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclModulePathObjCmd --
 *
 *  Implements "ns_modulepath". The command is basically a wrapper around
 *  Ns_ModulePath.
 *
 * Results:
 *  Tcl result.
 *
 * Side effects:
 *  None
 *
 *----------------------------------------------------------------------
 */

int
NsTclModulePathObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "/server/ ?/module .../?");
        result = TCL_ERROR;

    } else {
        Tcl_DString ds;
        TCL_SIZE_T  i;
        const char *module = objc > 2 ? Tcl_GetString(objv[2]) : NULL;

        Tcl_DStringInit(&ds);
        /*
         * Use (char *)0 as sentinel instead of NULL to make the function
         * portable. Cppcheck showed this problem in a first step.  See
         * e.g. http://ewontfix.com/11/.
         */
        Ns_ModulePath(&ds, Tcl_GetString(objv[1]), module, NS_SENTINEL);
        for (i = 3; i < objc; ++i) {
            Ns_MakePath(&ds, Tcl_GetString(objv[i]), NS_SENTINEL);
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
 *      Implements "ns_serverpath" and "ns_pagepath".
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
NsTclServerPathObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return PathObjCmd(clientData, interp, objc, objv, 's');
}

int
NsTclPagePathObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return PathObjCmd(clientData, interp, objc, objv, 'p');
}

static int
PathObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv, char cmd)
{
    char       *host = NULL;
    TCL_SIZE_T  npaths = 0;
    int         result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-host", Ns_ObjvString, &host, NULL},
        {"--",    Ns_ObjvBreak,  NULL,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?path-segment", Ns_ObjvArgs, &npaths, NULL},
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
            Ns_TclPrintfResult(interp, "no server available");
            result = TCL_ERROR;

        } else {
            Tcl_DString ds;
            TCL_SIZE_T  i;

            Tcl_DStringInit(&ds);
            if (cmd == 'p') {
                (void) NsPageRoot(&ds, servPtr, host);
            } else {
                (void) ServerRoot(&ds, servPtr, host);
            }
            for (i = objc - (TCL_SIZE_T)npaths; i < objc; ++i) {
                Ns_MakePath(&ds, Tcl_GetString(objv[i]), NS_SENTINEL);
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
 *      Implements "ns_serverrootproc".
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
NsTclServerRootProcObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const NsServer *servPtr = NsGetInitServer();
    int             result = TCL_OK;

    if (unlikely(objc < 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "/script/ ?/arg .../?");
        result = TCL_ERROR;

    } else if (unlikely(servPtr == NULL)) {
        Ns_TclPrintfResult(interp, "no initializing server");
        result = TCL_ERROR;

    } else {
        Ns_TclCallback *cbPtr;

        cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclServerRoot, objv[1],
                                  (TCL_SIZE_T)(objc - 2), objv + 2);
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
NsTclServerRoot(Tcl_DString *dest, const char *host, const void *arg)
{
    const Ns_TclCallback *cbPtr = arg;
    const char           *result = NULL;
    Ns_ReturnCode         rc = Ns_TclEvalCallback(NULL, cbPtr, dest, host, NS_SENTINEL);

    if (rc == TCL_OK || rc == TCL_RETURN) {
        result = dest->string;
    }

    /*Ns_Log(Notice, "??? NsTclServerRoot rc %s -> '%s'", Ns_TclReturnCodeString(rc), result);*/
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
MakePath(Tcl_DString *dest, va_list *pap)
{
    char      *s;
    TCL_SIZE_T len;

    NS_NONNULL_ASSERT(dest != NULL);

    for (s = va_arg(*pap, char *); s != NULL; s = va_arg(*pap, char *)) {
        if ((CHARTYPE(alpha, *s) != 0) && (s[1] == ':')) {
            char temp = *(s+2);

            *(s + 2) = '\0';
            Tcl_DStringAppend(dest, s, 2);
            *(s + 2) = temp;
            s += 2;
        }
        while (*s != '\0') {
            while (ISSLASH(*s)) {
                ++s;
            }
            if (*s != '\0') {
                Tcl_DStringAppend(dest, "/", 1);
                len = 0;
                while (s[len] != '\0' && !ISSLASH(s[len])) {
                    ++len;
                }
                Tcl_DStringAppend(dest, s, len);
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
 *      path.  If host is given it overrides the Host header of the
 *      current connection.
 *
 * Results:
 *      dest->string.
 *
 * Side effects:
 *      Value of Host header may be bashed to lowercase.
 *      Depends on registered Ns_ServerRootProc, if any.
 *
 *----------------------------------------------------------------------
 */
static const char *
ServerRoot(Tcl_DString *dest, const NsServer *servPtr, const char *rawHost)
{
    char           *safehost;
    const char     *path = NULL;
    Ns_Conn        *conn;
    const Ns_Set   *headers;
    Tcl_DString     ds;

    NS_NONNULL_ASSERT(dest != NULL);
    NS_NONNULL_ASSERT(servPtr != NULL);

    if (servPtr->vhost.serverRootProc != NULL) {
        /*
         * Configured to run a user-registered Ns_ServerRootProc.
         */

        conn = Ns_GetConn();
        if (conn != NULL && conn->request.serverRoot != NULL) {
            /*
             * Use the cached value.
             */
            Tcl_DStringAppend(dest, conn->request.serverRoot, TCL_INDEX_NONE);
            path = dest->string;
        } else if (conn != NULL) {
            /*
             * Call the registered proc which is typically, a Tcl
             * call. Therefore, make sure, the connection has already an
             * interpreter associated.
             */
            Ns_GetConnInterp(conn);

            path = (servPtr->vhost.serverRootProc)(dest, rawHost, servPtr->vhost.serverRootArg);
            if (path != NULL) {
                if (!Ns_PathIsAbsolute(path)) {
                    /*
                     * The computed path is not absolute. Prepend it with the
                     * static serverdir.
                     */
                    Tcl_DStringInit(&ds);
                    Ns_MakePath(&ds, servPtr->opts.serverdir, path, NS_SENTINEL);
                    Tcl_DStringSetLength(dest, 0);
                    Tcl_DStringAppend(dest, ds.string, ds.length);
                    Tcl_DStringFree(&ds);
                }
            }

            if (path != NULL) {
                Ns_Log(Debug, "cache value <%s>", path);
                conn->request.serverRoot = ns_strdup(path);
            }
        }
    } else if (servPtr->vhost.enabled
               && (rawHost != NULL
                   || ((conn = Ns_GetConn()) != NULL
                       && (headers = Ns_ConnHeaders(conn)) != NULL
                       && (rawHost = Ns_SetIGet(headers, "host")) != NULL))
               && *rawHost != '\0') {

        /*
         * Bail out if there are suspicious characters in the unprocessed Host.
         */

        if (Ns_StrIsValidHostHeaderContent(rawHost)) {

            /*
             * Normalize the Host string.
             */
            Tcl_DStringInit(&ds);
            safehost = Tcl_DStringAppend(&ds, rawHost, TCL_INDEX_NONE);

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
            path = Ns_MakePath(dest, servPtr->opts.serverdir,
                               servPtr->vhost.hostprefix, NS_SENTINEL);
            if (servPtr->vhost.hosthashlevel > 0) {
                Ns_HashPath(dest, safehost, servPtr->vhost.hosthashlevel);
            }
            Ns_NormalizePath(dest, safehost);
            Tcl_DStringFree(&ds);
        }
    }

    if (path == NULL) {
        /*
         * Default to static server root.
         */
        path = Ns_MakePath(dest, servPtr->opts.serverdir, NS_SENTINEL);
    }

    Ns_Log(Debug, "--- ServerRoot %s returns path <%s>", servPtr->server, path);
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
