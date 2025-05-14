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

#include "ns.h"

#define BUFSIZE          4096
#define NDSTRINGS        5

#define CGI_NPH          0x01u
#define CGI_GETHOST      0x02u
#define CGI_ECONTENT     0x04u
#define CGI_SYSENV       0x08u
#define CGI_ALLOW_STATIC 0x10u

/*
 * The following structure is allocated for each instance the module is
 * loaded (normally just once).
 */
typedef struct Mod {
    const char     *server;
    const char     *module;
    Ns_Set         *interps;
    Ns_Set         *mergeEnv;
    unsigned int    flags;
    int             maxInput;
    int             maxCgi;
    int             maxWait;
    int             activeCgi;
    Ns_Mutex        lock;
    Ns_Cond         cond;
} Mod;

/*
 * The following structure, allocated on the stack of CgiRequest, is used
 * to accumulate all the resources of a CGI.  CGI is a very messy interface
 * which requires varying degrees of resources.  Packing everything into
 * this structure allows building up the state in multiple places and
 * tearing it all down in FreeCgi, thus simplifying the CgiRequest procedure.
 */

typedef struct Cgi {
    Mod            *modPtr;
    unsigned int    flags;
    pid_t           pid;
    Ns_Set         *env;
    const char     *name;
    char           *path;
    const char     *pathinfo;
    char           *dir;
    const char     *exec;
    const char     *interp;
    Ns_Set         *interpEnv;
    int             ifd;
    int             ofd;
    ssize_t         cnt;
    char           *ptr;
    int             nextds;
    Tcl_DString     ds[NDSTRINGS];
    char            buf[BUFSIZE];
} Cgi;

/*
 * The following structure defines the context of a single CGI config
 * mapping, supporting both directory-style and pagedir-style CGI locations.
 */

typedef struct Map {
    Mod      *modPtr;
    char     *url;
    char     *path;
} Map;

/*
 * The following file descriptor is opened once on the first load and used
 * simply for duping as stdin in the child process.  This ensures the child
 * will get a proper EOF without having to allocate an empty temp file.
 */

static int devNull;
static Ns_LogSeverity Ns_LogCGIDebug;

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;
static Ns_TclTraceProc AddCmds;
static Ns_ArgProc ArgProc;

static const char *NS_EMPTY_STRING = "";

/*
 * Functions defined in this file.
 */
static Ns_OpProc CgiRequest;
static Ns_Callback CgiFreeMap;

static Ns_ReturnCode CgiInit(Cgi *cgiPtr, const Map *mapPtr, const Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void          CgiRegister(Mod *modPtr, const char *map)   NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Tcl_DString  *CgiDs(Cgi *cgiPtr)                          NS_GNUC_NONNULL(1);
static Ns_ReturnCode CgiFree(Cgi *cgiPtr)                        NS_GNUC_NONNULL(1);
static Ns_ReturnCode CgiExec(Cgi *cgiPtr, Ns_Conn *conn)         NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Ns_ReturnCode CgiSpool(Cgi *cgiPtr, const Ns_Conn *conn)  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Ns_ReturnCode CgiCopy(Cgi *cgiPtr, Ns_Conn *conn)         NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static ssize_t       CgiRead(Cgi *cgiPtr)                        NS_GNUC_NONNULL(1);
static ssize_t       CgiReadLine(Cgi *cgiPtr, Tcl_DString *dsPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static char         *NextWord(char *s)                           NS_GNUC_NONNULL(1);
static void          SetAppend(Ns_Set *set, int index, const char *sep, char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);
static void          CgiRegisterFastUrl2File(const char *server, const char *url, const char *path)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static TCL_OBJCMDPROC_T NsTclRegisterCGIObjCmd;

/*
 *----------------------------------------------------------------------
 *
 * ArgProc --
 *
 *      Append listen port info for query callback.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
ArgProc(Tcl_DString *dsPtr, const void *arg)
{
    const Map    *mapPtr = arg;

    assert(mapPtr != NULL);

    Tcl_DStringAppend(dsPtr, " url", 4);
    Tcl_DStringAppendElement(dsPtr, mapPtr->url);
    Tcl_DStringAppend(dsPtr, " path", 5);
    Tcl_DStringAppendElement(dsPtr, mapPtr->path);

    if (mapPtr->modPtr != NULL) {
        Tcl_DStringAppend(dsPtr, " module", 7);
        Tcl_DStringAppendElement(dsPtr, mapPtr->modPtr->module);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * AddCmds --
 *
 *      Add the commands provided by the nscgi module.
 *
 * Results:
 *      TCL_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AddCmds(Tcl_Interp *interp, const void *arg)
{
    const Mod *modPtr = arg;

    Ns_Log(Ns_LogCGIDebug, "nscgi: adding command ns_register_cgi");
    (void)TCL_CREATEOBJCOMMAND(interp, "ns_register_cgi", NsTclRegisterCGIObjCmd,
                               (ClientData)modPtr, NULL);
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Create a new CGI module instance.  Note: This module can
 *      be loaded multiple times.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      URL's may be registered for CGI.
 *
 *----------------------------------------------------------------------
 */
NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    const char     *section, *subSection;
    size_t          i;
    const Ns_Set   *set;
    Tcl_DString     ds;
    Mod            *modPtr;
    static bool     initialized = NS_FALSE;

    NS_NONNULL_ASSERT(module != NULL);

    /*
     * On the first (and likely only) load, register
     * the temp file cleanup routine and open devNull
     * for requests without content data.
     */

    if (!initialized) {
        devNull = ns_open(DEVNULL, O_RDONLY | O_CLOEXEC, 0);
        if (devNull < 0) {
            Ns_Log(Error, "nscgi: ns_open(%s) failed: %s",
                   DEVNULL, strerror(errno));
            return NS_ERROR;
        }
        (void)Ns_DupHigh(&devNull);
        (void)Ns_CloseOnExec(devNull);

        Ns_LogCGIDebug = Ns_CreateLogSeverity("Debug(cgi)");

        initialized = NS_TRUE;
    }

    /*
     * Config basic options.
     */
    section = Ns_ConfigSectionPath(NULL, server, module, NS_SENTINEL);
    modPtr = ns_calloc(1u, sizeof(Mod));
    modPtr->module = module;
    modPtr->server = server;
    Ns_MutexInit(&modPtr->lock);
    Ns_MutexSetName2(&modPtr->lock, "nscgi", server);
    modPtr->maxInput = (int)Ns_ConfigMemUnitRange(section, "maxinput", "1MB", 1024*1024, 0, LLONG_MAX);
    modPtr->maxCgi = Ns_ConfigInt(section, "limit", 0);
    modPtr->maxWait = Ns_ConfigInt(section, "maxwait", 30);
    if (Ns_ConfigBool(section, "gethostbyaddr", NS_FALSE)) {
        modPtr->flags |= CGI_GETHOST;
    }

    /*
     * Configure the various interp and env options.
     */
    Tcl_DStringInit(&ds);
    subSection = Ns_ConfigGetValue(section, "interps");
    if (subSection != NULL) {
        Ns_DStringVarAppend(&ds, "ns/interps/", subSection, NS_SENTINEL);
        modPtr->interps = Ns_ConfigGetSection(ds.string);
        if (modPtr->interps == NULL) {
            Ns_Log(Warning, "nscgi: no such interps section: %s",
                   ds.string);
        }
        Tcl_DStringSetLength(&ds, 0);
    }
    subSection = Ns_ConfigGetValue(section, "environment");
    if (subSection != NULL) {
        Ns_DStringVarAppend(&ds, "ns/environment/", subSection, NS_SENTINEL);
        modPtr->mergeEnv = Ns_ConfigGetSection(ds.string);
        if (modPtr->mergeEnv == NULL) {
            Ns_Log(Warning, "nscgi: no such environment section: %s",
                   ds.string);
        }
        Tcl_DStringSetLength(&ds, 0);
    }
    if (Ns_ConfigBool(section, "systemenvironment", NS_FALSE)) {
        modPtr->flags |= CGI_SYSENV;
    }
    if (Ns_ConfigBool(section, "allowstaticresources", NS_FALSE)) {
        modPtr->flags |= CGI_ALLOW_STATIC;
    }

    /*
     * Register all requested mappings.
     */
    set = Ns_ConfigGetSection(section);
    for (i = 0u; set != NULL && i < Ns_SetSize(set); ++i) {
        const char *key   = Ns_SetKey(set, i);

        if (STRIEQ(key, "map")) {
            const char *value = Ns_SetValue(set, i);
            CgiRegister(modPtr, value);
        }
    }
    Tcl_DStringFree(&ds);

    if (server == NULL) {
        Ns_Log(Warning, "nscgi: loaded as a global module,"
               " module specific commands are not loaded");
    } else {
        if (Ns_TclRegisterTrace(server, AddCmds, modPtr,
                                NS_TCL_TRACE_CREATE) != NS_OK) {
        } else {
            Ns_RegisterProcInfo((ns_funcptr_t)AddCmds, "nscgi:initinterp", NULL);
        }
        Ns_RegisterProcInfo((ns_funcptr_t)CgiRequest, "ns:cgirequest", ArgProc);
    }

    return NS_OK;
}



/*
 *----------------------------------------------------------------------
 *
 * CgiRequest -
 *
 *      Process a CGI request.
 *
 * Results:
 *      Standard NaviServer request result.
 *
 * Side effects:
 *      Program may be executed.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CgiRequest(const void *arg, Ns_Conn *conn)
{
    const Map      *mapPtr;
    Mod            *modPtr;
    Cgi             cgi;
    Ns_ReturnCode   status = NS_OK;

    mapPtr = arg;
    modPtr = mapPtr->modPtr;

    /*
     * Check for input overflow and initialize the CGI context.
     */

    if (modPtr->maxInput > 0 && (int)conn->contentLength > modPtr->maxInput) {
        return Ns_ConnReturnBadRequest(conn, "Exceeded maximum CGI input size");

    } else if (CgiInit(&cgi, mapPtr, conn) != NS_OK) {
        return Ns_ConnReturnNotFound(conn);

    } else if ((cgi.interp == NULL)
               && (access(cgi.exec, X_OK) != 0)) {
        /*
         * Can't execute interpreter. Maybe return file a static file?
         */
        Ns_Log(Ns_LogCGIDebug, "cannot execute interpreter."
               " Maybe a a static file <%s>", cgi.exec);

        if (((modPtr->flags & CGI_ALLOW_STATIC) != 0u) &&
            ( STREQ(conn->request.method, "GET") ||
              STREQ(conn->request.method, "HEAD")) ) {

            /*
             * Evidently people are storing images and such in
             * their cgi bin directory and they expect us to
             * return these files directly.
             */
            Ns_Log(Ns_LogCGIDebug, "allowstaticresources returns static file: %s", cgi.exec);
            status = Ns_ConnReturnFile(conn, 200, NULL, cgi.exec);

        } else {
            Ns_Log(Warning, "nscgi: CGI file not executable: %s", cgi.exec);

            if ( STREQ(conn->request.method, "GET") ||
                 STREQ(conn->request.method, "HEAD"))  {
                /*
                 * CGI_ALLOW_STATIC is not set, maybe the admin might want to
                 * activate it?
                 */
                Ns_Log(Warning, "nscgi: if this is a static resource, consider"
                       " serving this file via fastpath, or"
                       " setting 'allowstaticresources' in the CGI section"
                       " of the CGI configuration section");
            }
        }
        goto done;

    } else if (conn->contentLength > 0u
               && (CgiSpool(&cgi, conn) != NS_OK)) {
        /*
         * Content length failure.
         */
        if ((cgi.flags & CGI_ECONTENT) != 0u) {
            status = Ns_ConnReturnBadRequest(conn, "Insufficient Content");
        } else {
            (void)Ns_ConnTryReturnInternalError(conn, NS_ERROR, "nscgi: cannot spool data");
        }
        goto done;

    } else if (modPtr->maxCgi > 0) {
        Ns_Time       timeout;
        Ns_ReturnCode wait = NS_OK;

        /*
         * Wait for CGI access if necessary.
         */

        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, modPtr->maxWait, 0);
        Ns_MutexLock(&modPtr->lock);
        while (wait == NS_OK && modPtr->activeCgi >= modPtr->maxCgi) {
            wait = Ns_CondTimedWait(&modPtr->cond, &modPtr->lock, &timeout);
        }
        if (wait == NS_OK) {
            ++modPtr->activeCgi;
        }
        Ns_MutexUnlock(&modPtr->lock);
        if (wait != NS_OK) {
            status = Ns_ConnReturnStatus(conn, 503);
            goto done;
        }
    }

    /*
     * Execute the CGI and copy output.
     */

    status = CgiExec(&cgi, conn);
    //Ns_Log(Notice, "CgiExec returned OK %d closed %d", status == NS_OK, Ns_ConnIsClosed(conn));

    if (status != NS_OK) {
        status = Ns_ConnTryReturnInternalError(conn, status, "nscgi: CGI exec failed");
    } else {
        status = CgiCopy(&cgi, conn);
    }

    Ns_Log(Ns_LogCGIDebug, "nscgi: CGI returned status %d", status);
    //Ns_Log(Notice, "nscgi after COPY OK %d closed %d", status == NS_OK, Ns_ConnIsClosed(conn));

    /*
     * Release CGI access.
     */

    if (modPtr->maxCgi > 0) {
        Ns_MutexLock(&modPtr->lock);
        --modPtr->activeCgi;
        Ns_CondSignal(&modPtr->cond);
        Ns_MutexUnlock(&modPtr->lock);
    }

done:
    { Ns_ReturnCode reapStatus;

        reapStatus = CgiFree(&cgi);
        //Ns_Log(Notice, "CgiFree returned %d closed %d", reapStatus, Ns_ConnIsClosed(conn));
        if (reapStatus != NS_OK) {
            status = reapStatus;
            //Ns_Log(Notice, "nscgi reap failed status %d closed %d", status, Ns_ConnIsClosed(conn));
            status = Ns_ConnTryReturnInternalError(conn, status, "nscgi: CGI exec failed");

        } else if (status != NS_OK) {
            //Ns_Log(Notice, "nscgi: reap ok, but still, the status is not correct");
            status = Ns_ConnTryReturnInternalError(conn, status, "nscgi: invalid response from CGI");
        }
    }

    //Ns_Log(Notice, "nscgi done status %d closed %d", status, Ns_ConnIsClosed(conn));

    /*
     * Close connection unless it was closed earlier due to some error.
     */
    if (!Ns_ConnIsClosed(conn)) {
        status = Ns_ConnClose(conn);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiInit -
 *
 *      Setup a CGI context structure.  This function
 *      encapsulates the majority of the CGI semantics.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CgiInit(Cgi *cgiPtr, const Map *mapPtr, const Ns_Conn *conn)
{
    Mod                        *modPtr;
    Tcl_DString                *dsPtr;
    TCL_SIZE_T                  i;
    size_t                      ulen;
    char                       *e, *s;
    const char                 *url, *server, *fileName;
    const Ns_UrlSpaceMatchInfo *matchInfoPtr;

    NS_NONNULL_ASSERT(cgiPtr != NULL);
    NS_NONNULL_ASSERT(mapPtr != NULL);
    NS_NONNULL_ASSERT(conn != NULL);

    url = conn->request.url;
    server = Ns_ConnServer(conn);
    modPtr = mapPtr->modPtr;

    memset(cgiPtr, 0, (size_t)((char *)&cgiPtr->ds[0] - (char *)cgiPtr));
    cgiPtr->buf[0] = '\0';
    cgiPtr->modPtr = modPtr;
    cgiPtr->pid = NS_INVALID_PID;
    cgiPtr->ofd = cgiPtr->ifd = NS_INVALID_FD;
    cgiPtr->ptr = cgiPtr->buf;
    for (i = 0; i < NDSTRINGS; ++i) {
        Tcl_DStringInit(&cgiPtr->ds[i]);
    }

    /*
     * Determine the executable or script to run.
     */
    matchInfoPtr = Ns_ConnGetUrlSpaceMatchInfo(conn);
    ulen = strlen(url);
    fileName = url;

    Ns_Log(Ns_LogCGIDebug, "provided URL: '%s'", url);

    /*
     * The returned matchInfoPtr provides information, whether we have am
     * (inner) segment match. In this case PATH_INFO is the reminder of the
     * path.
     */
    if (matchInfoPtr->isSegmentMatch == NS_FALSE) {
        /*
         * Tail match (match on the last segment)
         *    SCRIPT_NAME = URL
         *    PATH_INFO   = ""
         */
        Ns_Log(Ns_LogCGIDebug, "nscgi: lastMapSegment match <%s>", mapPtr->url);

    } else {
        ssize_t offset;
        /*
         * Match on an inner segment:
         * 1. SCRIPT_NAME is the URL prefix.
         * 2. PATH_INFO is everything in the URL past SCRIPT_NAME.
         *
         * The provided offset is the determined on the request line including
         * the method. Therefore, we have to reduce the provided value by the
         * length of the method.
         */
        offset = matchInfoPtr->offset - (ssize_t)strlen(conn->request.method);

        Ns_Log(Ns_LogCGIDebug, "nscgi: url <%s> isSegmentMatch %d, offset %ld length %ld segment <%s>",
               url, matchInfoPtr->isSegmentMatch,
               matchInfoPtr->offset, matchInfoPtr->segmentLength, &url[offset]);

        dsPtr = CgiDs(cgiPtr);
        ulen = (size_t)offset + matchInfoPtr->segmentLength;
        fileName = Tcl_DStringAppend(dsPtr, url, (TCL_SIZE_T)ulen);
    }

    /*
     * Finally, perform the Url2File mapping invoking potentially
     * registered callbacks and provide the CGI context with path, name,
     * and pathinfo.
     */

    dsPtr = CgiDs(cgiPtr);
    (void) Ns_UrlToFile(dsPtr, server, fileName);
    cgiPtr->path = dsPtr->string;
    cgiPtr->name = fileName;
    cgiPtr->pathinfo = url + ulen;

    Ns_Log(Ns_LogCGIDebug,
           "nscgi: mapping for '%s'; url2file determined '%s'",
           fileName, cgiPtr->path);

    /*
     * Copy the script directory and see if the script is NPH.
     */
    s = strrchr(cgiPtr->path, INTCHAR('/'));
    if (s == NULL || access(cgiPtr->path, R_OK) != 0) {
        Ns_Log(Ns_LogCGIDebug, "nscgi: no such file: '%s'", cgiPtr->path);
        goto err;
    }
    *s = '\0';
    cgiPtr->dir = Tcl_DStringAppend(CgiDs(cgiPtr), cgiPtr->path, TCL_INDEX_NONE);
    Ns_Log(Ns_LogCGIDebug, "nscgi: dir <%s>", cgiPtr->dir);
    Ns_Log(Ns_LogCGIDebug, "nscgi: path <%s>", cgiPtr->path);
    Ns_Log(Ns_LogCGIDebug, "nscgi: name <%s>", cgiPtr->name);
    Ns_Log(Ns_LogCGIDebug, "nscgi: pathinfo <%s>", cgiPtr->pathinfo);
    *s++ = '/';
    if (strncmp(s, "nph-", 4u) == 0) {
        cgiPtr->flags |= CGI_NPH;
    }

    /*
     * Look for a script interpreter.
     */

    if (modPtr->interps != NULL
        && (s = strrchr(cgiPtr->path, INTCHAR('.'))) != NULL
        && (cgiPtr->interp = Ns_SetIGet(modPtr->interps, s)) != NULL) {
        cgiPtr->interp = Tcl_DStringAppend(CgiDs(cgiPtr), cgiPtr->interp, TCL_INDEX_NONE);
        s = strchr(cgiPtr->interp, INTCHAR('('));
        if (s != NULL) {
            *s++ = '\0';
            e = strchr(s, INTCHAR(')'));
            if (e != NULL) {
                *e = '\0';
            }
            cgiPtr->interpEnv = Ns_ConfigGetSection(s);
        }
    }
    if (cgiPtr->interp != NULL) {
        cgiPtr->exec = cgiPtr->interp;
    } else {
        cgiPtr->exec = cgiPtr->path;
    }

    Ns_Log(Ns_LogCGIDebug, "nscgi: interp '%s' exec '%s'", cgiPtr->interp, cgiPtr->exec);
    return NS_OK;

err:
    (void)CgiFree(cgiPtr);
    return NS_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiSpool --
 *
 *      Spool content to a temp file.
 *
 * Results:
 *      File descriptor of temp file or NS_INVALID_FD on error.
 *
 * Side effects:
 *      May open a new temp file.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CgiSpool(Cgi *cgiPtr, const Ns_Conn *conn)
{
    int           fd = NS_INVALID_FD;
    Ns_ReturnCode status;
    size_t        len;
    const char   *content, *err = NULL;

    NS_NONNULL_ASSERT(cgiPtr != NULL);
    NS_NONNULL_ASSERT(conn != NULL);

    len = conn->contentLength;
    content = Ns_ConnContent(conn);
    if (content == NULL) {
        if (Ns_ConnContentFile(conn) == NULL) {
            Ns_Log(Error, "nscgi: unable to access content.");
        } else {
            fd = ns_open(Ns_ConnContentFile(conn), O_RDONLY | O_BINARY | O_CLOEXEC, 0);
            if (fd == NS_INVALID_FD) {
                Ns_Log(Error, "nscgi: could not open content file: %s", strerror(errno));
            }
        }
    } else {
        fd = Ns_GetTemp();
        if (fd == NS_INVALID_FD) {
            Ns_Log(Error, "nscgi: could not allocate temp file.");
        } else if (ns_write(fd, content, len) != (ssize_t)len) {
            err = "write";
        } else if (ns_lseek(fd, 0, SEEK_SET) != 0) {
            err = "lseek";
        }
        if (err != NULL) {
            Ns_Log(Error, "nscgi: temp file %s failed: %s", err, strerror(errno));
            (void) ns_close(fd);
            fd = NS_INVALID_FD;
        }
    }

    if (fd == NS_INVALID_FD) {
        status = NS_ERROR;
    } else {
        cgiPtr->ifd = fd;
        status = NS_OK;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiDs -
 *
 *      Return the next available dstring in the CGI context.
 *
 * Results:
 *      Pointer to DString.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_DString *
CgiDs(Cgi *cgiPtr)
{
    Tcl_DString *result;

    NS_NONNULL_ASSERT(cgiPtr != NULL);

    if (cgiPtr->nextds < NDSTRINGS) {
        result = &cgiPtr->ds[cgiPtr->nextds++];
    } else {
        Ns_Fatal("nscgi: running out of configured dstrings");
        result = NULL;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiFree -
 *
 *      Free temp buffers used in CGI context.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CgiFree(Cgi *cgiPtr)
{
    Ns_ReturnCode result = NS_OK;

    NS_NONNULL_ASSERT(cgiPtr != NULL);

    /*
     * Close the pipe.
     */

    if (cgiPtr->ofd >= 0) {
        (void) ns_close(cgiPtr->ofd);
    }

    /*
     * Release the temp file.
     */

    if (cgiPtr->ifd >= 0) {
        Ns_ReleaseTemp(cgiPtr->ifd);
    }

    /*
     * Free the environment.
     */

    if (cgiPtr->env != NULL) {
        Ns_SetFree(cgiPtr->env);
    }

    /*
     * Reap the process.
     */
    if (cgiPtr->pid != NS_INVALID_PID) {
        int exitCode = 0;

        if (Ns_WaitForProcessStatus(cgiPtr->pid, &exitCode, NULL) != NS_OK) {
            Ns_Log(Error, "nscgi: wait for %s failed: %s",
                   cgiPtr->exec, strerror(errno));
        } else {
            Ns_Log(Ns_LogCGIDebug, "exit code: %d", (int8_t)exitCode);
            if (exitCode != 0) {
                result = NS_ERROR;
            }
        }
    }

    /*
     * Free all dstrings.
     */

    while (cgiPtr->nextds-- > 0) {
        Tcl_DStringFree(&cgiPtr->ds[cgiPtr->nextds]);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiExec -
 *
 *      Construct the command args and environment and execute
 *      the CGI.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CgiExec(Cgi *cgiPtr, Ns_Conn *conn)
{
    int           opipe[2], i;
    Ns_ReturnCode status;
    char         *s, *e;
    Tcl_DString  *dsPtr;
    const Mod    *modPtr;
    const char   *value;

    NS_NONNULL_ASSERT(cgiPtr != NULL);
    NS_NONNULL_ASSERT(conn != NULL);

    modPtr = cgiPtr->modPtr;
    /*
     * Get a dstring which will be used to setup env variables
     * and the arg list.
     */

    dsPtr = CgiDs(cgiPtr);

    /*
     * Setup and merge the environment set.
     */

    if (cgiPtr->interpEnv != NULL) {
        cgiPtr->env = Ns_SetCopy(cgiPtr->interpEnv);
    } else {
        cgiPtr->env = Ns_SetCreate(NULL);
    }
    if (modPtr->mergeEnv != NULL) {
        Ns_SetMerge(cgiPtr->env, modPtr->mergeEnv);
    }
    if ((modPtr->flags & CGI_SYSENV) != 0u) {
        char *const*envp = Ns_CopyEnviron(dsPtr);

        while (*envp != NULL) {
            s = *envp;
            e = strchr(s, INTCHAR('='));
            if (e != NULL) {
                int idx;

                *e = '\0';
                /*
                 * Do not overwrite already computed values in the Ns_Set.
                 */
                idx = Ns_SetFind(cgiPtr->env, s);
                if (idx < 0) {
                    (void)Ns_SetPutSz(cgiPtr->env, s, (TCL_SIZE_T)(e-s), e+1, -1);
                }
                *e = '=';
            }
            ++envp;
        }
        Tcl_DStringSetLength(dsPtr, 0);
    }

    /*
     * PATH is the only variable copied from the running environment
     * if it isn't already in the server default environment.
     */

    if (Ns_SetFind(cgiPtr->env, "PATH") < 0) {
        s = getenv("PATH");
        if (s != NULL) {
            Ns_SetUpdateSz(cgiPtr->env, "PATH", 4, s, TCL_INDEX_NONE);
        }
    }

    /*
     * Set all the CGI specified variables.
     */

    Ns_SetUpdateSz(cgiPtr->env, "SCRIPT_NAME", 11, cgiPtr->name, TCL_INDEX_NONE);
    Ns_SetUpdateSz(cgiPtr->env, "SCRIPT_FILENAME", 15, cgiPtr->path, -1);
    Ns_SetUpdateSz(cgiPtr->env, "REQUEST_URI", 11, Ns_ConnTarget(conn, dsPtr), TCL_INDEX_NONE);
    Tcl_DStringSetLength(dsPtr, 0);

    if (cgiPtr->pathinfo != NULL && *cgiPtr->pathinfo != '\0') {

        if (Ns_UrlPathDecode(dsPtr, cgiPtr->pathinfo, NULL) != NULL) {
            Ns_SetUpdateSz(cgiPtr->env, "PATH_INFO", 9, dsPtr->string, dsPtr->length);
        } else {
            Ns_SetUpdateSz(cgiPtr->env, "PATH_INFO", 9, cgiPtr->pathinfo, TCL_INDEX_NONE);
        }
    } else {
        /*
         * We have no pathinfo, must be a wildcard map
         */
        Ns_SetUpdateSz(cgiPtr->env, "PATH_INFO", 9, NS_EMPTY_STRING, 0);
    }
    Ns_SetUpdateSz(cgiPtr->env, "PATH_TRANSLATED", 15, cgiPtr->path, TCL_INDEX_NONE);

    if (cgiPtr->interp != NULL) {
        /*
         * We have a registered interpreter. In the PHP case, one has to
         * communicate this fact via the "REDIRECT_STATUS" variable.
         */
        Ns_SetUpdateSz(cgiPtr->env, "REDIRECT_STATUS", 15, "1", 1);
    }
    Tcl_DStringSetLength(dsPtr, 0);
    Ns_SetUpdateSz(cgiPtr->env, "GATEWAY_INTERFACE", 17, "CGI/1.1", 7);
    Ns_DStringVarAppend(dsPtr, Ns_InfoServerName(), "/", Ns_InfoServerVersion(), NS_SENTINEL);
    Ns_SetUpdateSz(cgiPtr->env, "SERVER_SOFTWARE", 15, dsPtr->string, dsPtr->length);
    Tcl_DStringSetLength(dsPtr, 0);
    Ns_DStringPrintf(dsPtr, "HTTP/%2.1f", conn->request.version);
    Ns_SetUpdateSz(cgiPtr->env, "SERVER_PROTOCOL", 15, dsPtr->string, dsPtr->length);
    Tcl_DStringSetLength(dsPtr, 0);

#if 0
    /*
     * Determine SERVER_NAME and SERVER_PORT from the request information. The
     * values in the request structure are already syntactically
     * validated. However, the "request.host" will contain as well untrusted
     * host header fields, whereas the location contains for untrusted hoost
     * values the default name (see e.g., hacker.com) in nscgi.test
     */
    Ns_SetUpdateSz(cgiPtr->env, "SERVER_NAME", 11, conn->request.host, TCL_INDEX_NONE);
    Ns_DStringPrintf(dsPtr, "%hu", conn->request.port);
    Ns_SetUpdateSz(cgiPtr->env, "SERVER_PORT", 11, dsPtr->string, dsPtr->length);
    Tcl_DStringSetLength(dsPtr, 0);
#else
    s = Ns_ConnLocationAppend(conn, dsPtr);
    if (likely(*s != '\0')) {
        /*
         * Determine SERVER_NAME and SERVER_PORT from the conn location.
         */
        char *end, *portString, *hostString;
        bool  hostParsedOk;

        s = strchr(s, INTCHAR(':'));
        s += 3;                        /* Get past the protocol "://"  */
        hostParsedOk = Ns_HttpParseHost2(s, NS_FALSE, &hostString, &portString, &end);

        if (!hostParsedOk) {
            /*
             * This should not happen, since in this cases, the fallback
             * mechanism of the driver should have kicked in already.
             */
            Ns_Log(Warning, "nscgi: invalid hostname: '%s'", s);
            Ns_SetUpdateSz(cgiPtr->env, "SERVER_NAME", 11, "", 0);
            Tcl_DStringSetLength(dsPtr, 0);
            Ns_DStringPrintf(dsPtr, "%hu", Ns_ConnPort(conn));
            Ns_SetUpdateSz(cgiPtr->env, "SERVER_PORT", 11, dsPtr->string, dsPtr->length);
        } else {
            Ns_SetUpdateSz(cgiPtr->env, "SERVER_NAME", 11, hostString, TCL_INDEX_NONE);
            if (portString != NULL) {
                Ns_SetUpdateSz(cgiPtr->env, "SERVER_PORT", 11, portString, TCL_INDEX_NONE);
            } else {
                Tcl_DStringSetLength(dsPtr, 0);
                Ns_DStringPrintf(dsPtr, "%hu", Ns_ConnPort(conn));
                Ns_SetUpdateSz(cgiPtr->env, "SERVER_PORT", 11, dsPtr->string, dsPtr->length);
            }
        }
    } else {
        /*
         * If for whatever reason the location cannot be determined (e.g.,
         * running behind a proxy server, where we cannot validate the host
         * header field), use the provided information.
         */
        Ns_SetUpdateSz(cgiPtr->env, "SERVER_NAME", 11, conn->request.host, TCL_INDEX_NONE);
        Ns_DStringPrintf(dsPtr, "%hu", conn->request.port);
        Ns_SetUpdateSz(cgiPtr->env, "SERVER_PORT", 11, dsPtr->string, dsPtr->length);
    }
    Tcl_DStringSetLength(dsPtr, 0);
#endif
    /*
     * Provide Authentication information
     */
    {
        const Ns_Set *authSet =  Ns_ConnAuth(conn);

        if (authSet != NULL) {
            const char *authMethod = Ns_SetIGet(authSet, "authmethod");

            Ns_SetUpdateSz(cgiPtr->env, "AUTH_TYPE", 9, authMethod ? authMethod : "", TCL_INDEX_NONE);
        } else {
            Ns_SetUpdateSz(cgiPtr->env, "AUTH_TYPE", 9, "", 0);
        }
    }
    Ns_SetUpdateSz(cgiPtr->env, "REMOTE_USER", 11, Ns_ConnAuthUser(conn), TCL_INDEX_NONE);

    {
        const char *peer = Ns_ConnPeerAddr(conn);

        if (peer != NULL) {
            Ns_SetUpdateSz(cgiPtr->env, "REMOTE_ADDR", 11, peer, TCL_INDEX_NONE);
            if ((modPtr->flags & CGI_GETHOST) != 0u) {
                if (Ns_GetHostByAddr(dsPtr, peer) == NS_TRUE) {
                    Ns_SetUpdateSz(cgiPtr->env, "REMOTE_HOST", 11, dsPtr->string, dsPtr->length);
                }
                Tcl_DStringSetLength(dsPtr, 0);
            } else {
                Ns_SetUpdateSz(cgiPtr->env, "REMOTE_HOST", 11, peer, TCL_INDEX_NONE);
            }
        }
    }

    /*
     * Provide request information.
     */

    Ns_SetUpdateSz(cgiPtr->env, "REQUEST_METHOD", 14, conn->request.method, TCL_INDEX_NONE);
    Ns_SetUpdateSz(cgiPtr->env, "QUERY_STRING", 12, conn->request.query, TCL_INDEX_NONE);

    value = Ns_SetIGet(conn->headers, "content-type");
    if (value == NULL) {
        if (STREQ("POST", conn->request.method)) {
            value = "application/x-www-form-urlencoded";
        } else {
            value = NS_EMPTY_STRING;
        }
    }
    Ns_SetUpdateSz(cgiPtr->env, "CONTENT_TYPE", 12, value, TCL_INDEX_NONE);

    if (conn->contentLength == 0u) {
        Ns_SetUpdateSz(cgiPtr->env, "CONTENT_LENGTH", 14, NS_EMPTY_STRING, 0);
    } else {
        Ns_DStringPrintf(dsPtr, "%u", (unsigned) conn->contentLength);
        Ns_SetUpdateSz(cgiPtr->env, "CONTENT_LENGTH", 14, dsPtr->string, dsPtr->length);
        Tcl_DStringSetLength(dsPtr, 0);
    }

    /*
     * Set the HTTP_ header variables.
     */

    Tcl_DStringAppend(dsPtr, "HTTP_", 5);
    for (i = 0; (size_t)i < Ns_SetSize(conn->headers); ++i) {
        int idx;

        s = Ns_SetKey(conn->headers, i);
        e = Ns_SetValue(conn->headers, i);
        Tcl_DStringAppend(dsPtr, s, TCL_INDEX_NONE);
        s = dsPtr->string + 5;
        while (*s != '\0') {
            if (*s == '-') {
                *s = '_';
            } else if (CHARTYPE(lower, *s) != 0) {
                *s = CHARCONV(upper, *s);
            }
            ++s;
        }
        idx = Ns_SetFind(cgiPtr->env, dsPtr->string);
        if (idx < 0) {
            (void)Ns_SetPutSz(cgiPtr->env, dsPtr->string, dsPtr->length, e, TCL_INDEX_NONE);
        } else {
            SetAppend(cgiPtr->env, idx, ", ", e);
        }
        Tcl_DStringSetLength(dsPtr, 5);
    }

    /*
     * Build up the argument block.
     */

    Tcl_DStringSetLength(dsPtr, 0);
    if (cgiPtr->interp != NULL) {
        Ns_DStringAppendArg(dsPtr, cgiPtr->interp);
    }
    if (cgiPtr->path != NULL) {
        Ns_DStringAppendArg(dsPtr, cgiPtr->path);
    }
    s = conn->request.query;
    if (s != NULL) {
        if (strchr(s, INTCHAR('=')) == NULL) {
            do {
                e = strchr(s, INTCHAR('+'));
                if (e != NULL) {
                    *e = '\0';
                }
                (void) Ns_UrlQueryDecode(dsPtr, s, NULL, NULL);
                Tcl_DStringAppend(dsPtr, NS_EMPTY_STRING, 1);
                if (e != NULL) {
                    *e++ = '+';
                }
                s = e;
            } while (s != NULL);
        }
        Tcl_DStringAppend(dsPtr, NS_EMPTY_STRING, 1);
    }

    /*
     * Create the output pipe.
     */

    if (unlikely(ns_pipe(opipe) != 0)) {
        Ns_Log(Error, "nscgi: pipe() failed: %s", strerror(errno));
        status = NS_ERROR;
    } else {

        /*
         * Execute the CGI.
         */
        cgiPtr->pid = Ns_ExecProcess(cgiPtr->exec, cgiPtr->dir,
                                     cgiPtr->ifd < 0 ? devNull : cgiPtr->ifd,
                                     opipe[1], dsPtr->string, cgiPtr->env);

        Ns_Log(Ns_LogCGIDebug,
               "nscgi: execute cgi script in directory '%s' returned pid %ld",
               cgiPtr->dir, (long)cgiPtr->pid);

        (void) ns_close(opipe[1]);
        if (unlikely(cgiPtr->pid == NS_INVALID_PID)) {
            (void) ns_close(opipe[0]);
            status = NS_ERROR;
        } else {
            cgiPtr->ofd = opipe[0];
            status = NS_OK;
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiRead -
 *
 *      Read content from pipe into the CGI buffer.
 *
 * Results:
 *      Number of bytes read or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
CgiRead(Cgi *cgiPtr)
{
    ssize_t n;

    NS_NONNULL_ASSERT(cgiPtr != NULL);

    cgiPtr->ptr = cgiPtr->buf;
    do {
        n = ns_read(cgiPtr->ofd, cgiPtr->buf, sizeof(cgiPtr->buf));
    } while (n < 0 && errno == NS_EINTR);
    if (n > 0) {
        cgiPtr->cnt = n;
    } else if (n < 0) {
        Ns_Log(Error, "nscgi: pipe ns_read() from %s failed: %s",
               cgiPtr->exec, strerror(errno));
    }
    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiReadLine -
 *
 *      Read and right trim a line from the pipe.
 *
 * Results:
 *      Length of header read or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
CgiReadLine(Cgi *cgiPtr, Tcl_DString *dsPtr)
{
    char    c;
    ssize_t n;

    NS_NONNULL_ASSERT(cgiPtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    do {
        while (cgiPtr->cnt > 0) {
            c = *cgiPtr->ptr;
            ++cgiPtr->ptr;
            --cgiPtr->cnt;
            if (c == '\n') {
                while (dsPtr->length > 0
                    && CHARTYPE(space, dsPtr->string[dsPtr->length - 1]) != 0) {
                    Tcl_DStringSetLength(dsPtr, dsPtr->length-1);
                }
                return (ssize_t)dsPtr->length;
            }
            Tcl_DStringAppend(dsPtr, &c, 1);
        }
    } while ((n = CgiRead(cgiPtr)) > 0);
    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * CgiCopy
 *
 *      Read and parse headers and then copy output.
 *
 * Results:
 *      NaviServer request result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
CgiCopy(Cgi *cgiPtr, Ns_Conn *conn)
{
    Tcl_DString     ds;
    int             last, httpstatus;
    Ns_ReturnCode   status;
    char           *value;
    Ns_Set         *hdrs;
    ssize_t         n, lines = 0;
    bool            statusProvided = NS_FALSE;

    NS_NONNULL_ASSERT(cgiPtr != NULL);
    NS_NONNULL_ASSERT(conn != NULL);

    /*
     * Skip to copy for nph CGI's.
     */

    if ((cgiPtr->flags & CGI_NPH) != 0u) {
        goto copy;
    }

    /*
     * Read and parse headers up to the blank line or end of file.
     */
    Tcl_DStringInit(&ds);
    last = -1;
    httpstatus = 200;
    hdrs = conn->outputheaders;
    while ((n = CgiReadLine(cgiPtr, &ds)) > 0) {
        Ns_Log(Ns_LogCGIDebug, "=== header line n %ld <%s>", n, ds.string);

        if (CHARTYPE(space, *ds.string) != 0) {
            /*
             * Continued header.
             */
            if (last == -1) {
                /*
                 * Silently ignore bad header.
                 */
                continue;
            }
            SetAppend(hdrs, last, "\n", ds.string);
            lines ++;
        } else {
            value = strchr(ds.string, INTCHAR(':'));
            if (value == NULL) {
                /*
                 * Silently ignore bad header.
                 */
                continue;
            }
            *value++ = '\0';
            while (CHARTYPE(space, *value) != 0) {
                ++value;
            }
            lines ++;
            if (STRIEQ(ds.string, "status")) {
                statusProvided = NS_TRUE;
                httpstatus = (int)strtol(value, NULL, 10);

            } else if (STRIEQ(ds.string, "location")) {
                if (!statusProvided) {
                    httpstatus = 302;
                }
#if defined(NS_ALLOW_RELATIVE_REDIRECTS) && NS_ALLOW_RELATIVE_REDIRECTS
                last = (int)Ns_SetPutSz(hdrs, ds.string, ds.length, value, TCL_INDEX_NONE);
#else
                if (*value == '/') {
                    Tcl_DString redir;

                    Tcl_DStringInit(&redir);
                    (void)Ns_ConnLocationAppend(conn, &redir);
                    Tcl_DStringAppend(&redir, value, TCL_INDEX_NONE);
                    last = (int)Ns_SetPutSz(hdrs, ds.string, ds.length, redir.string, TCL_INDEX_NONE);
                    Tcl_DStringFree(&redir);
                } else {
                    last = (int)Ns_SetPutSz(hdrs, ds.string, ds.length, value, TCL_INDEX_NONE);
                }
#endif
            } else {
                last = (int)Ns_SetPutSz(hdrs, ds.string, ds.length, value, TCL_INDEX_NONE);
            }
        }
        Tcl_DStringSetLength(&ds, 0);
    }
    Tcl_DStringFree(&ds);
    Ns_Log(Ns_LogCGIDebug, "=== header lines %ld", lines);

    if (n < 0) {
        status = Ns_ConnTryReturnInternalError(conn, NS_ERROR, "nscgi: reading client data failed");

    } else if (lines == 0) {
        status = NS_ERROR;

    } else {

        /*
         * Queue the headers and copy remaining content up to end of file.
         */

        Ns_ConnSetResponseStatus(conn, httpstatus);
    copy:
        do {
            struct iovec vbuf;

            vbuf.iov_base = cgiPtr->ptr;
            vbuf.iov_len  = (size_t)cgiPtr->cnt;
            status = Ns_ConnWriteVData(conn, &vbuf, 1, NS_CONN_STREAM);
            //Ns_Log(Ns_LogCGIDebug, "=== content %ld\n%s", cgiPtr->cnt, (char*)cgiPtr->ptr);

        } while (status == NS_OK && CgiRead(cgiPtr) > 0);
#if 0
        /*
         * Close connection now so it will not linger on
         * waiting for process exit.
         */

        if (status == NS_OK) {
            status = Ns_ConnClose(conn);
        }
#endif
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NextWord -
 *
 *      Locate next word in CGI mapping.
 *
 * Results:
 *      Pointer to next word.
 *
 * Side effects:
 *      String is modified in place.
 *
 *----------------------------------------------------------------------
 */

static char    *
NextWord(char *s)
{
    NS_NONNULL_ASSERT(s != NULL);

    while (*s != '\0' && CHARTYPE(space, *s) == 0) {
        ++s;
    }
    if (*s != '\0') {
        *s++ = '\0';
        while (CHARTYPE(space, *s) != 0) {
            ++s;
        }
    }
    return s;
}

/*----------------------------------------------------------------------
 *
 * CgiRegisterFastUrl2File -
 *
 *      Helper file for achieving consistent behavior when an FastUrl2File
 *      handler is registered via the configuration file or via
 *      NsTclRegisterCGIObjCmd (Tcl command "ns_register_cgi").
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May register or re-register a mapping.
 *
 *----------------------------------------------------------------------
 */
static void
CgiRegisterFastUrl2File(const char *server, const char *url, const char *path)
{
    char *tailSegment;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(path != NULL);

    tailSegment = strrchr(url, INTCHAR('/'));
    /*
     * When there is a tail segment and it contains a wildchard character,
     * strip it away for the mapping. This means, that all files in this
     * folder are mapped.
     */
    if (tailSegment != NULL && strchr(tailSegment, INTCHAR('*')) != NULL) {
        *tailSegment = '\0';
        Ns_RegisterFastUrl2File(server, url, path, 0u);
        *tailSegment = '/';
    } else {
        Ns_RegisterFastUrl2File(server, url, path, 0u);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * CgiRegister -
 *
 *      Register a CGI request mapping.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May register or re-register a mapping.
 *
 *----------------------------------------------------------------------
 */

static void
CgiRegister(Mod *modPtr, const char *map)
{
    char           *method;
    char           *url;
    const char     *path;
    Tcl_DString     ds1, ds2;
    Map            *mapPtr;

    NS_NONNULL_ASSERT(modPtr != NULL);
    NS_NONNULL_ASSERT(map != NULL);

    Tcl_DStringInit(&ds1);
    Tcl_DStringInit(&ds2);

    Tcl_DStringAppend(&ds1, map, TCL_INDEX_NONE);
    method = ds1.string;
    url = NextWord(method);
    if (*method == '\0' || *url == '\0') {
        Ns_Log(Error, "nscgi: invalid mapping: %s", map);
        goto done;
    }

    path = NextWord(url);
    if (*path == '\0') {
        path = NULL;
    } else {
        path = Ns_NormalizePath(&ds2, path);
        if (Ns_PathIsAbsolute(path) == NS_FALSE || access(path, R_OK) != 0) {
            Ns_Log(Error, "nscgi: invalid directory: %s", path);
            goto done;
        }
    }

    mapPtr = ns_malloc(sizeof(Map));
    mapPtr->modPtr = modPtr;
    mapPtr->url = ns_strdup(url);
    mapPtr->path = ns_strcopy(path);
    Ns_Log(Notice, "nscgi: %s %s%s%s", method, url,
           (path != NULL) ? " -> " : NS_EMPTY_STRING,
           (path != NULL) ? path : NS_EMPTY_STRING);

    (void) Ns_RegisterRequest2(NULL, modPtr->server, method, url,
                               CgiRequest, CgiFreeMap, mapPtr, NS_OP_SEGMENT_MATCH, NULL);
    if (path != NULL) {
        /*
         * When a path is provided, register it to the Url2File
         * mappings. These are used for determining the source locations for
         * static files and CGI programs.
         */
        CgiRegisterFastUrl2File(modPtr->server, url, mapPtr->path);
    }

done:
    Tcl_DStringFree(&ds1);
    Tcl_DStringFree(&ds2);
}


/*
 *----------------------------------------------------------------------
 *
 * CgiFreeMap -
 *
 *      Free a request mapping context.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
CgiFreeMap(void *arg)
{
    Map  *mapPtr = (Map *) arg;

    ns_free(mapPtr->url);
    ns_free(mapPtr->path);
    ns_free(mapPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * SetAppend -
 *
 *      Append data to an existing Ns_Set value.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
SetAppend(Ns_Set *set, int index, const char *sep, char *value)
{
    Tcl_DString ds;

    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(sep != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    Tcl_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, Ns_SetValue(set, index),
                        sep, value, NS_SENTINEL);
    Ns_SetPutValueSz(set, (size_t)index, ds.string, ds.length);
    Tcl_DStringFree(&ds);
}


/*----------------------------------------------------------------------
 *
 * NsTclRegisterCGIObjCmd --
 *
 *      Implements "ns_register_cgi".
 *
 * Results:
 *      Return TCL_OK upon success and TCL_ERROR otherwise.
 *
 * Side effects:
 *      Might register CGI handlers.
 *
 *----------------------------------------------------------------------
 */
static int
NsTclRegisterCGIObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *method, *url, *path = NULL;
    int         noinherit = 0, matchsegments = 0, result = TCL_OK;
    void       *specPtr = NULL;   /* use void, since no NsUrlSpaceContextSpec declared */
    Ns_ObjvSpec opts[] = {
        {"-constraints", Ns_ObjvUrlspaceSpec, &specPtr,  NULL},
        {"-noinherit",     Ns_ObjvBool,        &noinherit,     INT2PTR(NS_OP_NOINHERIT)},
        {"-matchsegments", Ns_ObjvBool,        &matchsegments, INT2PTR(NS_OP_SEGMENT_MATCH)},
        {"-path",          Ns_ObjvString,      &path,          NULL},
        {"--",             Ns_ObjvBreak,       NULL,           NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"method",     Ns_ObjvString, &method, NULL},
        {"url",        Ns_ObjvString, &url,    NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Map            *mapPtr;
        Mod            *modPtr = clientData;
        unsigned int    flags = 0u;

        if (noinherit != 0) {
            flags |= NS_OP_NOINHERIT;
        }
        if (matchsegments != 0) {
            flags |= NS_OP_SEGMENT_MATCH;
        }

        mapPtr = ns_malloc(sizeof(Map));
        mapPtr->modPtr = modPtr;
        mapPtr->url = ns_strdup(url);
        mapPtr->path = ns_strcopy(path);
        Ns_Log(Notice, "nscgi: %s %s%s%s", method, url,
               (path != NULL) ? " -> " : NS_EMPTY_STRING,
               (path != NULL) ? path : NS_EMPTY_STRING);

        result = Ns_RegisterRequest2(interp, modPtr->server, method, url,
                                     CgiRequest, CgiFreeMap, mapPtr, flags, specPtr);
        if (path != NULL) {
            /*
             * When a path is provided, register it to the Url2File
             * mappings. These are used for determining the source locations
             * for static files and CGI programs.
             */
            CgiRegisterFastUrl2File(modPtr->server, url, mapPtr->path);
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
