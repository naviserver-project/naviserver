/*
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at http://mozilla.org/.
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
 * fastpath.c --
 *
 *      Get page possibly from a file cache.
 */

#include "nsd.h"

/*
 * The following structure defines the contents of a file
 * stored in the file cache.
 */

typedef struct {
    time_t mtime;
    size_t size;
    dev_t  dev;
    ino_t  ino;
    int    refcnt;
    char   bytes[1];  /* Grown to actual file size. */
} File;


/*
 * Local functions defined in this file
 */

static void DecrEntry(File *filePtr)
    NS_GNUC_NONNULL(1);

static bool UrlIs(const char *server, const char *url, bool isDir)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ReturnCode FastGetRestart(Ns_Conn *conn, const char *page)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Ns_ReturnCode FastReturn(Ns_Conn *conn, int statusCode, const char *mimeType, const char *fileName)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

static int  CompressExternalFile(Tcl_Interp *interp, const char *cmdName, const char *fileName, const char *gzFileName)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

static void NormalizePath(const char **pathPtr)
    NS_GNUC_NONNULL(1);

static const char *
CheckStaticCompressedDelivery(
    Ns_Conn *conn,
    Tcl_DString *dsPtr,
    bool doRefresh,
    const char *ext,
    const char *cmdName,
    const char *fileName,
    const char *encoding
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6) NS_GNUC_NONNULL(7);


static Ns_Callback FreeEntry;
static Ns_ServerInitProc ConfigServerFastpath;


/*
 * Local variables defined in this file.
 */

static Ns_Cache *cache = NULL;                /* Global cache of pages for all virtual servers.     */
static int       maxentry;                    /* Maximum size of an individual entry in the cache.  */
static bool      useMmap = NS_FALSE;          /* Use the mmap() system call to read data from disk. */
static bool      useGzip = NS_FALSE;          /* Use gzip delivery if possible                      */
static bool      useGzipRefresh = NS_FALSE;   /* Update outdated gzip files automatically via ::ns_gzipfile */
static bool      useBrotli = NS_FALSE;        /* Use brotli delivery if possible                      */
static bool      useBrotliRefresh = NS_FALSE; /* Update outdated brotli files automatically via ::ns_brotlifile */



/*
 *----------------------------------------------------------------------
 *
 * NsConfigFastpath --
 *
 *      Initialize the global fastpath cache.
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
NsConfigFastpath(void)
{
    const char *path;

    path    = Ns_ConfigSectionPath(NULL, NULL, NULL, "fastpath", (char *)0L);
    useMmap = Ns_ConfigBool(path, "mmap", NS_FALSE);
    useGzip = Ns_ConfigBool(path, "gzip_static", NS_FALSE);
    useGzipRefresh = Ns_ConfigBool(path, "gzip_refresh", NS_FALSE);
    useBrotli = Ns_ConfigBool(path, "brotli_static", NS_FALSE);
    useBrotliRefresh = Ns_ConfigBool(path, "brotli_refresh", NS_FALSE);

    if (Ns_ConfigBool(path, "cache", NS_FALSE)) {
        size_t size = (size_t)Ns_ConfigMemUnitRange(path, "cachemaxsize", "10MB",
                                                    1024*10000, 1024, INT_MAX);
        cache = Ns_CacheCreateSz("ns:fastpath", TCL_STRING_KEYS, size, FreeEntry);
        maxentry = (int)Ns_ConfigMemUnitRange(path, "cachemaxentry", "8KB", 8192, 8, INT_MAX);
    }
    /*
     * Register the fastpath initialization for every server.
     */
    NsRegisterServerInit(ConfigServerFastpath);
}


/*
 *----------------------------------------------------------------------
 *
 * NormalizePath --
 *
 *      Normalize the path to provide canonical directory names. The
 *      canonicalization is called, when the path contains a slash,
 *      otherwise the provided path is not touched.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potentially replacing the string in *pathPtr.
 *
 *----------------------------------------------------------------------
 */
static void
NormalizePath(const char **pathPtr) {

    NS_NONNULL_ASSERT(pathPtr != NULL);
    assert(*pathPtr != NULL);

    if (strchr(*pathPtr, INTCHAR('/')) != NULL) {
        Tcl_Obj *pathObj, *normalizedPathObj;
        /*
         * The path contains a slash, it might be not normalized;
         */
        pathObj = Tcl_NewStringObj(*pathPtr, -1);
        Tcl_IncrRefCount(pathObj);

        normalizedPathObj = Tcl_FSGetNormalizedPath(NULL, pathObj);
        if (normalizedPathObj != NULL) {
            /*
             * Normalization was successful, replace the string in
             * *pathPtr with the normalized string.
             *
             * The values returned by Ns_ConfigString() are the string
             * values from the ns_set. We do not want to free *pathPtr
             * here, but we overwrite it with a freshly allocated
             * string. When this function is used from other contexts,
             * not freeing the old value could be a potential memory
             * leak.
             *
             */
            *pathPtr = ns_strdup(Tcl_GetString(normalizedPathObj));
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ConfigServerFastpath --
 *
 *      Load the config values for the specified server and register
 *      Ns_FastPathProc() for GET, HEAD and POST requests.
 *
 * Results:
 *      Return always NS_OK.
 *
 * Side effects:
 *      Updating the fastpath configuration for the specified server.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ConfigServerFastpath(const char *server)
{
    NsServer     *servPtr = NsGetServer(server);
    Ns_ReturnCode result;

    if (unlikely(servPtr == NULL)) {
        Ns_Log(Warning, "Could configure fastpath; server '%s' unknown", server);
        result = NS_ERROR;

    } else {
        Ns_DString  ds;
        const char *path, *p;

        path = Ns_ConfigSectionPath(NULL, server, NULL, "fastpath", (char *)0L);
        Ns_DStringInit(&ds);

        p = Ns_ConfigString(path, "directoryfile", "index.adp index.tcl index.html index.htm");
        if (p != NULL && Tcl_SplitList(NULL, p, &servPtr->fastpath.dirc,
                                       &servPtr->fastpath.dirv) != TCL_OK) {
            Ns_Log(Error, "fastpath[%s]: directoryfile is not a list: %s", server, p);
        }

        servPtr->fastpath.serverdir =
            ns_strcopy(Ns_ConfigString(path, "serverdir", NS_EMPTY_STRING));

        if (!Ns_PathIsAbsolute(servPtr->fastpath.serverdir)) {
            (void)Ns_HomePath(&ds, servPtr->fastpath.serverdir, (char *)0L);
            servPtr->fastpath.serverdir = Ns_DStringExport(&ds);
        }  else {
            NormalizePath(&servPtr->fastpath.serverdir);
        }

        /*
         * Not sure, we still need fastpath.pageroot AND fastpath.pagedir.
         * "pageroot" always points to the absolute path, while "pagedir"
         * might contain the relative path (or is the same as "pageroot").
         */
        servPtr->fastpath.pagedir = ns_strcopy(Ns_ConfigString(path, "pagedir", "pages"));
        if (Ns_PathIsAbsolute(servPtr->fastpath.pagedir) == NS_TRUE) {
            servPtr->fastpath.pageroot = servPtr->fastpath.pagedir;
            NormalizePath(&servPtr->fastpath.pageroot);
        } else {
            (void)Ns_MakePath(&ds, servPtr->fastpath.serverdir,
                              servPtr->fastpath.pagedir, (char *)0L);
            servPtr->fastpath.pageroot = Ns_DStringExport(&ds);
        }

        servPtr->fastpath.dirproc = ns_strcopy(Ns_ConfigString(path, "directoryproc", "_ns_dirlist"));
        servPtr->fastpath.diradp  = ns_strcopy(Ns_ConfigString(path, "directoryadp", NULL));

        Ns_RegisterRequest(server, "GET", "/",  Ns_FastPathProc, NULL, NULL, 0u);
        Ns_RegisterRequest(server, "HEAD", "/", Ns_FastPathProc, NULL, NULL, 0u);
        Ns_RegisterRequest(server, "POST", "/", Ns_FastPathProc, NULL, NULL, 0u);

        result = NS_OK;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ConnReturnFile --
 *
 *      Send the contents of a file out the conn.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      See FastReturn.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_ConnReturnFile(Ns_Conn *conn, int statusCode, const char *mimeType, const char *fileName)
{
    Conn         *connPtr = (Conn *) conn;
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(fileName != NULL);

    if (Ns_Stat(fileName, &connPtr->fileInfo) == NS_FALSE) {
        Ns_Log(Debug, "Ns_ConnReturnFile for '%s' returns 404", fileName);
        status = Ns_ConnReturnNotFound(conn);
    } else {
        status = FastReturn(conn, statusCode, mimeType, fileName);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_FastPathProc --
 *
 *      Return the contents of a URL.
 *
 * Results:
 *      Return NS_OK for success or NS_ERROR for failure.
 *
 * Side effects:
 *      Contents of file may be cached in file cache.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_FastPathProc(const void *UNUSED(arg), Ns_Conn *conn)
{
    Conn         *connPtr;
    NsServer     *servPtr;
    const char   *url;
    Ns_DString    ds;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(conn != NULL);

    connPtr = (Conn *) conn;
    servPtr = connPtr->poolPtr->servPtr;
    url = conn->request.url;

    Ns_DStringInit(&ds);

    if ((NsUrlToFile(&ds, servPtr, url) != NS_OK)
        || (Ns_Stat(ds.string, &connPtr->fileInfo) == NS_FALSE)) {
        goto notfound;
    }

    if (S_ISREG(connPtr->fileInfo.st_mode)) {

        /*
         * Return ordinary files as with Ns_ConnReturnFile.
         */
        result = FastReturn(conn, 200, NULL, ds.string);

    } else if (S_ISDIR(connPtr->fileInfo.st_mode)) {
        int i;

        Ns_Log(Debug, "FastPathProc resolves dir <%s> names %d", url, servPtr->fastpath.dirc);
        /*
         * For directories, search for a matching directory file and
         * restart the connection if found.
         */

        for (i = 0; i < servPtr->fastpath.dirc; ++i) {
            Ns_DStringSetLength(&ds, 0);
            if (NsUrlToFile(&ds, servPtr, url) != NS_OK) {
                goto notfound;
            }
            Ns_DStringVarAppend(&ds, "/", servPtr->fastpath.dirv[i], (char *)0L);

            if ((stat(ds.string, &connPtr->fileInfo) == 0)
                && S_ISREG(connPtr->fileInfo.st_mode)
                ) {
                Ns_Log(Debug, "FastPathProc checks [%d] '%s' -> found", i, ds.string);
                if (url[strlen(url) - 1u] != '/') {
                    const char* query = conn->request.query;

                    Ns_DStringSetLength(&ds, 0);
                    Ns_DStringVarAppend(&ds, url, "/", (char *)0L);
                    if (query != NULL) {
                        Ns_DStringVarAppend(&ds, "?", query, (char *)0L);
                    }
                    result = Ns_ConnReturnRedirect(conn, ds.string);
                } else {
                    result = FastGetRestart(conn, servPtr->fastpath.dirv[i]);
                }
                goto done;
            }
            Ns_Log(Debug, "FastPathProc checks [%d] '%s' -> not found", i, ds.string);
        }

        /*
         * If no index file was found, invoke a directory listing
         * ADP or Tcl proc if configured.
         */

        if (servPtr->fastpath.diradp != NULL) {
            Ns_Log(Debug, "FastPathProc lists directory listing using ADP");
            result = Ns_AdpRequest(conn, servPtr->fastpath.diradp);

        } else if (servPtr->fastpath.dirproc != NULL) {
            Ns_Log(Debug, "FastPathProc lists directory listing using Tcl");
            result = Ns_TclRequest(conn, servPtr->fastpath.dirproc);

        } else {
            goto notfound;
        }
    } else {

    notfound:
        Ns_Log(Debug, "Ns_FastPathProc for '%s' returns 404", ds.string);

        result = Ns_ConnReturnNotFound(conn);
    }

 done:
    Ns_DStringFree(&ds);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlIsFile, Ns_UrlIsDir --
 *
 *      Check if a file/directory that corresponds to a URL exists.
 *
 * Results:
 *      Return NS_TRUE if the file exists and NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_UrlIsFile(const char *server, const char *url)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    return UrlIs(server, url, NS_FALSE);
}

bool
Ns_UrlIsDir(const char *server, const char *url)
{
    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    return UrlIs(server, url, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * UrlIs --
 *
 *      Helper function for Ns_UrlIsFile and Ns_UrlIsDir.  Map the url
 *      to a file and check whether the file exists or is a directory.
 *
 * Results:
 *      Returns boolean value indicating success.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
UrlIs(const char *server, const char *url, bool isDir)
{
    Ns_DString   ds;
    struct stat  st;
    bool         is = NS_FALSE;

    NS_NONNULL_ASSERT(server != NULL);
    NS_NONNULL_ASSERT(url != NULL);

    Ns_DStringInit(&ds);
    if (Ns_UrlToFile(&ds, server, url) == NS_OK
        && stat(ds.string, &st) == 0
        && ((isDir && S_ISDIR(st.st_mode))
            || (!isDir && S_ISREG(st.st_mode)))) {
        is = NS_TRUE;
    }
    Ns_DStringFree(&ds);

    return is;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_PageRoot --
 *
 *      Return pathname of the server pages directory.
 *      Deprecated: Use Ns_PagePath() which is virtual host aware.
 *
 * Results:
 *      Server pageroot or NULL on invalid server.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_PageRoot(const char *server)
{
    const NsServer *servPtr;
    const char     *pageRoot = NULL;

    NS_NONNULL_ASSERT(server != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        pageRoot = servPtr->fastpath.pageroot;
    }

    return pageRoot;
}


/*
 *----------------------------------------------------------------------
 *
 * CompressExternalFile --
 *
 *      Compress an external file with the command configured via e.g.
 *      "gzip_cmd". We use the external program instead of in-memory
 *      operations to avoid memory boats on large source files.
 *
 * Results:
 *      Tcl Result Code
 *
 * Side effects:
 *      Compressed file in the same directory.
 *      When compression fails, the command writes a warning to the error.log.
 *
 *----------------------------------------------------------------------
 */
static int
CompressExternalFile(Tcl_Interp *interp, const char *cmdName, const char *fileName, const char *gzFileName)
{
    int result;
    Tcl_DString ds, *dsPtr = &ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(cmdName != NULL);
    NS_NONNULL_ASSERT(fileName != NULL);
    NS_NONNULL_ASSERT(gzFileName != NULL);

    Tcl_DStringInit(dsPtr);
    Tcl_DStringAppend(dsPtr, cmdName, -1);
    Tcl_DStringAppend(dsPtr, " ", 1);
    Tcl_DStringAppendElement(dsPtr, fileName);
    Tcl_DStringAppendElement(dsPtr, gzFileName);
    result = Tcl_EvalEx(interp, Tcl_DStringValue(dsPtr), Tcl_DStringLength(dsPtr), 0);
    if (result != TCL_OK) {
        Ns_Log(Warning, "%s returned: %s ", cmdName, Tcl_GetString(Tcl_GetObjResult(interp)));
    }
    Tcl_DStringFree(dsPtr);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CheckStaticCompressedDelivery --
 *
 *      Check, if there is a static compressed file available for
 *      delivery. When it is there but outdated, try to refresh it,
 *      when this is allowed be the configuration.
 *
 * Results:
 *      Bame of the compressed file, when this is available and valid.
 *
 * Side effects:
 *      Potentially recompress file in the filesystem.
 *
 *----------------------------------------------------------------------
 */

static const char *
CheckStaticCompressedDelivery(
    Ns_Conn *conn,
    Tcl_DString *dsPtr,
    bool doRefresh,
    const char *ext,
    const char *cmdName,
    const char *fileName,
    const char *encoding
) {
    const char  *result = NULL;
    struct stat  gzStat;
    const char  *compressedFileName;
    Conn        *connPtr;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(ext != NULL);
    NS_NONNULL_ASSERT(cmdName != NULL);
    NS_NONNULL_ASSERT(fileName != NULL);
    NS_NONNULL_ASSERT(encoding != NULL);

    connPtr = (Conn *)conn;

    Tcl_DStringAppend(dsPtr, fileName, -1);
    Tcl_DStringAppend(dsPtr, ext, -1);
    compressedFileName = Tcl_DStringValue(dsPtr);
    //fprintf(stderr, "=== check compressed file <%s> compressed <%s>\n", fileName, compressedFileName);


    if (Ns_Stat(compressedFileName, &gzStat)) {
        Ns_ConnCondSetHeaders(conn, "Vary", "Accept-Encoding");
        //fprintf(stderr, "=== we have the file <%s> compressed <%s>\n", fileName, compressedFileName);

        /*
         * We have a file with the compression extension
         */
        if (gzStat.st_mtime < connPtr->fileInfo.st_mtime
            && doRefresh) {
            /*
             * The modification time of the compressed file is older
             * than the modification time of the source, and the
             * configuration file indicates the we have to try to refresh the
             * compressed file (e.g. rezip the source).
             */
            if (CompressExternalFile(Ns_GetConnInterp(conn), cmdName, fileName, compressedFileName) == TCL_OK) {
                (void)Ns_Stat(compressedFileName, &gzStat);
            }
        }
        if (gzStat.st_mtime >= connPtr->fileInfo.st_mtime) {
            /*
             * The modification time of the compressed file is newer or
             * equal, so use it for delivery.
             */
            connPtr->fileInfo = gzStat;
            result = compressedFileName;
            Ns_ConnCondSetHeaders(conn, "Content-Encoding", encoding);
        } else {
            Ns_Log(Warning, "gzip: the gzip file %s is older than the uncompressed file",
                   compressedFileName);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * FastReturn --
 *
 *      Return file contents, possibly from cache.
 *
 * Results:
 *      NaviServer return code.
 *
 * Side effects:
 *      May map, cache, open, and/or send file out connection.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
FastReturn(Ns_Conn *conn, int statusCode, const char *mimeType, const char *fileName)
{
    Conn          *connPtr;
    int            isNew, fd;
    Ns_ReturnCode  status = NS_OK;
    Tcl_DString    ds, *dsPtr = &ds;
    bool           done;
    const char    *compressedFileName = NULL;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(fileName != NULL);

    connPtr = (Conn *) conn;

    if (unlikely(Ns_ConnSockPtr(conn) == NULL)) {
        Ns_Log(Warning,
               "FastReturn: called without valid connection, maybe connection already closed: %s",
               fileName);
        status = NS_ERROR;
        done = NS_TRUE;
    } else {
        /*
         * Set the last modified header if not set yet.
         * If not modified since last request, return now.
         */

        Ns_ConnSetLastModifiedHeader(conn, &connPtr->fileInfo.st_mtime);

        if (Ns_ConnModifiedSince(conn, connPtr->fileInfo.st_mtime) == NS_FALSE) {
            status = Ns_ConnReturnNotModified(conn);
            done = NS_TRUE;

        } else if (Ns_ConnUnmodifiedSince(conn, connPtr->fileInfo.st_mtime) == NS_FALSE) {
            status = Ns_ConnReturnStatus(conn, 412); /* Precondition Failed. */
            done = NS_TRUE;

        } else {
            done = NS_FALSE;
        }
    }
    if (done) {
        return status;
    }

    /*
     * Determine the mime type if not given based on the requested
     * filename (without a potential gz suffix).
     */
    if (mimeType == NULL) {
        mimeType = Ns_GetMimeType(fileName);
    }

    Tcl_DStringInit(dsPtr);

    /*
     * Check compressed versions of fileName
     */
    if (useBrotli && (connPtr->flags & NS_CONN_BROTLIACCEPTED) != 0u) {
        compressedFileName = CheckStaticCompressedDelivery(conn, dsPtr, useBrotliRefresh,
                                                           ".br", "::ns_brotlifile",
                                                           fileName, "br");
    }

    if (compressedFileName == NULL && useGzip && (connPtr->flags & NS_CONN_ZIPACCEPTED) != 0u) {
        Tcl_DStringSetLength(dsPtr, 0);
        compressedFileName = CheckStaticCompressedDelivery(conn, dsPtr, useGzipRefresh,
                                                           ".gz", "::ns_gzipfile",
                                                           fileName, "gzip");
    }

    if (compressedFileName != NULL) {
        fileName = compressedFileName;
    }

    /*
     * For no output (i.e., HEAD request), just send required
     * headers.
     */

    if ((conn->flags & NS_CONN_SKIPBODY) != 0u) {
        Ns_DStringFree(dsPtr);
        return Ns_ConnReturnData(conn, statusCode, NS_EMPTY_STRING,
                                 (ssize_t)connPtr->fileInfo.st_size, mimeType);
    }

    /*
     * Depending on the size of the content and state of the fastpath
     * cache, either return the data directly, or cache it first and
     * return the cached copy.
     */

    if ((cache == NULL)
        || (connPtr->fileInfo.st_size > maxentry)
        || (connPtr->fileInfo.st_ctime >= (time_t)(connPtr->acceptTime.sec - 1))
        ) {
        /*
         * The cache is not enabled or the entry is too large for the
         * cache, or the inode has been changed too recently (within 1
         * second of the start of this connection) so send the content
         * directly.
         */

        if (useMmap
            && NsMemMap(fileName, (size_t)connPtr->fileInfo.st_size,
                        NS_MMAP_READ, &connPtr->fmap) == NS_OK) {
            status = Ns_ConnReturnData(conn, statusCode, connPtr->fmap.addr,
                                       (ssize_t)connPtr->fmap.size, mimeType);
            if ((connPtr->flags & NS_CONN_SENT_VIA_WRITER) == 0u) {
                NsMemUmap(&connPtr->fmap);
            }
            connPtr->fmap.addr = NULL;

        } else {
            fd = ns_open(fileName, O_RDONLY | O_BINARY | O_CLOEXEC, 0);
            if (fd < 0) {
                Ns_Log(Warning, "fastpath: ns_open(%s) failed: '%s'",
                       fileName, strerror(errno));
                goto notfound;
            }
            status = Ns_ConnReturnOpenFd(conn, statusCode, mimeType, fd,
                                         (size_t)connPtr->fileInfo.st_size);
            (void) ns_close(fd);
        }

    } else {
        Ns_Entry   *entry;
        File       *filePtr;

        /*
         * Search for an existing cache entry for this file, validating
         * the contents against the current file mtime, size and inode.
         */
        Ns_CacheLock(cache);
        entry = Ns_CacheWaitCreateEntry(cache, fileName, &isNew, NULL);

        /*
         * Validate entry.
         */

        if (isNew == 0) {
            filePtr = Ns_CacheGetValue(entry);
            if (filePtr != NULL
                && (filePtr->mtime != connPtr->fileInfo.st_mtime
                    || filePtr->size != (size_t)connPtr->fileInfo.st_size
                    || filePtr->dev  != (dev_t)connPtr->fileInfo.st_dev
                    || filePtr->ino  != connPtr->fileInfo.st_ino)
                ) {
                Ns_CacheUnsetValue(entry);
                isNew = 1;
            }
        } else {
            filePtr = NULL;
        }

        if (isNew != 0) {

            /*
             * Read and cache new or invalidated entries in one big chunk.
             */

            Ns_CacheUnlock(cache);
            fd = ns_open(fileName, O_RDONLY | O_BINARY | O_CLOEXEC, 0);
            if (fd < 0) {
                filePtr = NULL;
                Ns_Log(Warning, "fastpath: ns_open(%s') failed '%s'",
                       fileName, strerror(errno));
                status = NS_ERROR;
            } else {
                ssize_t nread;

                filePtr = ns_malloc(sizeof(File) + (size_t)connPtr->fileInfo.st_size);
                filePtr->refcnt = 1;
                filePtr->size   = (size_t)connPtr->fileInfo.st_size;
                filePtr->mtime  = connPtr->fileInfo.st_mtime;
                filePtr->dev    = connPtr->fileInfo.st_dev;
                filePtr->ino    = connPtr->fileInfo.st_ino;
                nread = ns_read(fd, filePtr->bytes, filePtr->size);
                (void) ns_close(fd);
                if (nread != (ssize_t)filePtr->size) {
                    Ns_Log(Warning, "fastpath: failed to read '%s': '%s'",
                           fileName, strerror(errno));
                    ns_free(filePtr);
                    filePtr = NULL;
                    status = NS_ERROR;
                }
            }
            Ns_CacheLock(cache);
            entry = Ns_CacheCreateEntry(cache, fileName, &isNew);
            if (filePtr != NULL) {
                Ns_CacheSetValueSz(entry, filePtr, filePtr->size + sizeof(File));
            } else {
                Ns_CacheDeleteEntry(entry);
            }
            Ns_CacheBroadcast(cache);
        }
        if (filePtr != NULL) {
            ++filePtr->refcnt;
            Ns_CacheUnlock(cache);
            status = Ns_ConnReturnData(conn, statusCode, filePtr->bytes,
                                       (ssize_t)filePtr->size, mimeType);
            Ns_CacheLock(cache);
            DecrEntry(filePtr);
        }
        Ns_CacheUnlock(cache);
        if (filePtr == NULL) {
            goto notfound;
        }
    }

    Ns_DStringFree(dsPtr);
    return status;

 notfound:

    Ns_Log(Debug, "FastReturn for '%s' returns 404", fileName);

    Ns_DStringFree(dsPtr);
    return Ns_ConnReturnNotFound(conn);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Stat --
 *
 *      Stat a file, logging an error on unexpected results.
 *
 * Results:
 *      NS_TRUE if stat() was successful, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_Stat(const char *path, struct stat *stPtr)
{
    bool success = NS_TRUE;

    NS_NONNULL_ASSERT(path != NULL);
    NS_NONNULL_ASSERT(stPtr != NULL);

    if (stat(path, stPtr) != 0) {
        if (errno != ENOENT && errno != EACCES && errno != ENOTDIR) {
            Ns_Log(Error, "fastpath: stat(%s) failed: %s",
                   path, strerror(errno));
        }
        success = NS_FALSE;
    }
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * FastGetRestart --
 *
 *      Construct the full URL and redirect internally.
 *
 * Results:
 *      See Ns_ConnRedirect().
 *
 * Side effects:
 *      See Ns_ConnRedirect().
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
FastGetRestart(Ns_Conn *conn, const char *page)
{
    Ns_ReturnCode status;
    Ns_DString    ds;

    NS_NONNULL_ASSERT(conn != NULL);
    NS_NONNULL_ASSERT(page != NULL);

    Ns_DStringInit(&ds);
    status = Ns_ConnRedirect(conn, Ns_MakePath(&ds, conn->request.url, page, (char *)0L));
    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * DecrEntry --
 *
 *      Decrement reference count of cached file.
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
DecrEntry(File *filePtr)
{
    NS_NONNULL_ASSERT(filePtr != NULL);

    if (--filePtr->refcnt == 0) {
        ns_free(filePtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * FreeEntry --
 *
 *      Cache-free callback: logically remove a cached file from file cache.
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
FreeEntry(void *arg)
{
    File *filePtr = arg;

    DecrEntry(filePtr);
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclFastPathCacheStatsObjCmd --
 *
 *      Implements "ns_fastpath_cache_stats".  The command returns
 *      stats on a cache. The size and expiry time of each entry in
 *      the cache is also appended if the -contents switch is given.
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
NsTclFastPathCacheStatsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         contents = (int)NS_FALSE, reset = (int)NS_FALSE, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-contents", Ns_ObjvBool,  &contents, INT2PTR(NS_TRUE)},
        {"-reset",    Ns_ObjvBool,  &reset,    INT2PTR(NS_TRUE)},
        {"--",        Ns_ObjvBreak, NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, NULL, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (cache != NULL) {
        Ns_DString      ds;
        Ns_CacheSearch  search;

        Ns_DStringInit(&ds);
        Ns_CacheLock(cache);

        if (contents != 0) {
            const Ns_Entry *entry;

            Tcl_DStringStartSublist(&ds);
            entry = Ns_CacheFirstEntry(cache, &search);
            while (entry != NULL) {
                size_t         size    = Ns_CacheGetSize(entry);
                const Ns_Time *timePtr = Ns_CacheGetExpirey(entry);

                if (timePtr->usec == 0) {
                    Ns_DStringPrintf(&ds, "%" PRIdz " %" PRId64 " ",
                                     size, (int64_t)timePtr->sec);
                } else {
                    Ns_DStringPrintf(&ds, "%" PRIdz " " NS_TIME_FMT " ",
                                     size, (int64_t)timePtr->sec, timePtr->usec);
                }
                entry = Ns_CacheNextEntry(&search);
            }
            Tcl_DStringEndSublist(&ds);
        } else {
            (void)Ns_CacheStats(cache, &ds);
        }
        if (reset != 0) {
            Ns_CacheResetStats(cache);
        }
        Ns_CacheUnlock(cache);

        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
