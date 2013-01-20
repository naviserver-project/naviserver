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
    int    size;
    dev_t  dev;
    ino_t  ino;
    int    refcnt;
    char   bytes[1];  /* Grown to actual file size. */
} File;


/*
 * Local functions defined in this file
 */

static Ns_Callback FreeEntry;

static void DecrEntry      (File *filePtr);
static int  UrlIs          (CONST char *server, CONST char *url, int dir);
static int  FastStat       (CONST char *path, struct stat *stPtr);
static int  FastGetRestart (Ns_Conn *conn, CONST char *page);
static int  FastReturn     (Ns_Conn *conn, int status, CONST char *type,
                            CONST char *file);
static Ns_ServerInitProc ConfigServerFastpath;


/*
 * Local variables defined in this file.
 */

static Ns_Cache *cache = NULL;  /* Global cache of pages for all virtual servers.     */
static int       maxentry;      /* Maximum size of an individual entry in the cache.  */
static int       usemmap;       /* Use the mmap() system call to read data from disk. */



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
NsConfigFastpath()
{
    char  *path;

    path    = Ns_ConfigGetPath(NULL, NULL, "fastpath", NULL);
    usemmap = Ns_ConfigBool(path, "mmap", NS_FALSE);

    if (Ns_ConfigBool(path, "cache", NS_FALSE)) {
        cache = Ns_CacheCreateSz("ns:fastpath", TCL_STRING_KEYS,
                    Ns_ConfigIntRange(path, "cachemaxsize", 1024*10000, 1024, INT_MAX),
                    FreeEntry);
        maxentry = Ns_ConfigIntRange(path, "cachemaxentry", 8192, 8, INT_MAX);
    }
    NsRegisterServerInit(ConfigServerFastpath);
}

static int
ConfigServerFastpath(CONST char *server)
{
    NsServer   *servPtr = NsGetServer(server);
    Ns_DString  ds;
    CONST char *path, *p;

    path = Ns_ConfigGetPath(server, NULL, "fastpath", NULL);
    Ns_DStringInit(&ds);

    p = Ns_ConfigString(path, "directoryfile", "index.adp index.tcl index.html index.htm");
    if (p != NULL && Tcl_SplitList(NULL, p, &servPtr->fastpath.dirc,
                                   &servPtr->fastpath.dirv) != TCL_OK) {
        Ns_Log(Error, "fastpath[%s]: directoryfile is not a list: %s", server, p);
    }

    servPtr->fastpath.serverdir = (char*)Ns_ConfigString(path, "serverdir", "");
    if (!Ns_PathIsAbsolute(servPtr->fastpath.serverdir)) {
        Ns_HomePath(&ds, servPtr->fastpath.serverdir, NULL);
        servPtr->fastpath.serverdir = Ns_DStringExport(&ds);
    }

    servPtr->fastpath.pagedir = Ns_ConfigString(path, "pagedir", "pages");
    if (Ns_PathIsAbsolute(servPtr->fastpath.pagedir)) {
        servPtr->fastpath.pageroot = servPtr->fastpath.pagedir;
    } else {
        Ns_MakePath(&ds, servPtr->fastpath.serverdir,
                    servPtr->fastpath.pagedir, NULL);
        servPtr->fastpath.pageroot = Ns_DStringExport(&ds);
    }

    p = Ns_ConfigString(path, "directorylisting", "simple");
    if (p != NULL && (STREQ(p, "simple") || STREQ(p, "fancy"))) {
        p = "_ns_dirlist";
    }
    servPtr->fastpath.dirproc = Ns_ConfigString(path, "directoryproc", p);
    servPtr->fastpath.diradp  = Ns_ConfigGetValue(path, "directoryadp");

    Ns_RegisterRequest(server, "GET", "/",  Ns_FastPathProc, NULL, NULL, 0);
    Ns_RegisterRequest(server, "HEAD", "/", Ns_FastPathProc, NULL, NULL, 0);
    Ns_RegisterRequest(server, "POST", "/", Ns_FastPathProc, NULL, NULL, 0);

    return NS_OK;
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

int
Ns_ConnReturnFile(Ns_Conn *conn, int status, CONST char *type, CONST char *file)
{
    Conn        *connPtr = (Conn *) conn;
    int          rc;
    /*char         *server;
      NsServer     *servPtr;*/

    if (!FastStat(file, &connPtr->fileInfo)) {
        return Ns_ConnReturnNotFound(conn);
    }

    /*server  = Ns_ConnServer(conn);
      servPtr = NsGetServer(server);*/

    rc = FastReturn(conn, status, type, file);
    return rc;
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

int
Ns_FastPathProc(void *arg, Ns_Conn *conn)
{
    Conn        *connPtr = (Conn *) conn;
    NsServer    *servPtr = connPtr->poolPtr->servPtr;
    char        *url = conn->request->url;
    Ns_DString   ds;
    int          result;

    Ns_DStringInit(&ds);

    if (NsUrlToFile(&ds, servPtr, url) != NS_OK
        || !FastStat(ds.string, &connPtr->fileInfo)) {
        goto notfound;
    }
    if (S_ISREG(connPtr->fileInfo.st_mode)) {

        /*
         * Return ordinary files as with Ns_ConnReturnFile.
         */

        result = FastReturn(conn, 200, NULL, ds.string);

    } else if (S_ISDIR(connPtr->fileInfo.st_mode)) {
        int i;

        /*
         * For directories, search for a matching directory file and
         * restart the connection if found.
         */

        for (i = 0; i < servPtr->fastpath.dirc; ++i) {
            Ns_DStringSetLength(&ds, 0);
            if (NsUrlToFile(&ds, servPtr, url) != NS_OK) {
                goto notfound;
            }
            Ns_DStringVarAppend(&ds, "/", servPtr->fastpath.dirv[i], NULL);
            if ((stat(ds.string, &connPtr->fileInfo) == 0) && S_ISREG(connPtr->fileInfo.st_mode)) {
                if (url[strlen(url) - 1] != '/') {
                    Ns_DStringSetLength(&ds, 0);
                    Ns_DStringVarAppend(&ds, url, "/", NULL);
                    result = Ns_ConnReturnRedirect(conn, ds.string);
                } else {
                    result = FastGetRestart(conn, servPtr->fastpath.dirv[i]);
                }
                goto done;
            }
        }

        /*
         * If no index file was found, invoke a directory listing
         * ADP or Tcl proc if configured.
         */

        if (servPtr->fastpath.diradp != NULL) {
            result = Ns_AdpRequest(conn, servPtr->fastpath.diradp);
        } else if (servPtr->fastpath.dirproc != NULL) {
            result = Ns_TclRequest(conn, servPtr->fastpath.dirproc);
        } else {
            goto notfound;
        }
    } else {
    notfound:
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

int
Ns_UrlIsFile(CONST char *server, CONST char *url)
{
    return UrlIs(server, url, 0);
}

int
Ns_UrlIsDir(CONST char *server, CONST char *url)
{
    return UrlIs(server, url, 1);
}

static int
UrlIs(CONST char *server, CONST char *url, int dir)
{
    Ns_DString   ds;
    struct stat  st;
    int          is = NS_FALSE;

    Ns_DStringInit(&ds);
    if (Ns_UrlToFile(&ds, server, url) == NS_OK
        && !stat(ds.string, &st)
        && ((dir && S_ISDIR(st.st_mode))
            || (!dir && S_ISREG(st.st_mode)))) {
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
 *      Return path name of the server pages directory.
 *      Depreciated: Use Ns_PagePath() which is virtual host aware.
 *
 * Results:
 *      Server pageroot or NULL on invalid server.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Ns_PageRoot(CONST char *server)
{
    NsServer *servPtr = NsGetServer(server);

    if (servPtr != NULL) {
        return servPtr->fastpath.pageroot;
    }

    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * FastReturn --
 *
 *      Return file contents, possibly from cache.
 *
 * Results:
 *      Standard Ns_Request result.
 *
 * Side effects:
 *      May map, cache, open, and/or send file out connection.
 *
 *----------------------------------------------------------------------
 */

static int
FastReturn(Ns_Conn *conn, int status, CONST char *type, CONST char *file)
{
    Conn        *connPtr = (Conn *) conn;
    int         isNew, fd, result = NS_ERROR;
    Ns_Entry   *entry;
    File       *filePtr;
    FileMap     fmap;

    /*
     * Determine the mime type if not given.
     */

    if (type == NULL) {
        type = Ns_GetMimeType(file);
    }

    /*
     * Set the last modified header if not set yet.
     * If not modified since last request, return now.
     */

    Ns_ConnSetLastModifiedHeader(conn, &connPtr->fileInfo.st_mtime);

    if (!Ns_ConnModifiedSince(conn, connPtr->fileInfo.st_mtime)) {
        return Ns_ConnReturnNotModified(conn);
    }
    if (!Ns_ConnUnmodifiedSince(conn, connPtr->fileInfo.st_mtime)) {
        return Ns_ConnReturnStatus(conn, 412); /* Precondition Failed. */
    }


    /*
     * For no output (i.e., HEAD request), just send required
     * headers.
     */

    if (conn->flags & NS_CONN_SKIPBODY) {
        return Ns_ConnReturnData(conn, status, "", 0, type);
    }

    /*
     * Depending on the size of the content and state of the fastpath cache,
     * either return the data directly, or cache it first and return the
     * cached copy.
     */

    if (cache == NULL || connPtr->fileInfo.st_size > maxentry
        || connPtr->fileInfo.st_ctime >= (connPtr->acceptTime.sec-1) ) {

        /*
         * Caching is disabled, the entry is too large for the cache,
	 * or the inode has been changed too recently (within 1 second
	 * of the start of this connection) so send the content 
	 * directly.
         */

        if (usemmap
                && NsMemMap(file, connPtr->fileInfo.st_size, NS_MMAP_READ, &fmap) == NS_OK) {
            result = Ns_ConnReturnData(conn, status, fmap.addr, fmap.size, type);
            NsMemUmap(&fmap);
        } else {
            fd = open(file, O_RDONLY | O_BINARY);
            if (fd < 0) {
                Ns_Log(Warning, "fastpath: open(%s) failed: '%s'",
                       file, strerror(errno));
                goto notfound;
            }
            result = Ns_ConnReturnOpenFd(conn, status, type, fd, connPtr->fileInfo.st_size);
            close(fd);
        }

    } else {

        /*
         * Search for an existing cache entry for this file, validating
         * the contents against the current file mtime, size and inode.
         */

        filePtr = NULL;
        Ns_CacheLock(cache);
        entry = Ns_CacheWaitCreateEntry(cache, file, &isNew, NULL);

        /*
         * Validate entry.
         */

        if (!isNew
            && (filePtr = Ns_CacheGetValue(entry)) != NULL
            && (filePtr->mtime != connPtr->fileInfo.st_mtime
                || filePtr->size != connPtr->fileInfo.st_size
                || filePtr->dev != connPtr->fileInfo.st_dev
                || filePtr->ino != connPtr->fileInfo.st_ino)) {
            Ns_CacheUnsetValue(entry);
            isNew = 1;
        }

        if (isNew) {

            /*
             * Read and cache new or invalidated entries in one big chunk.
             */

            Ns_CacheUnlock(cache);
            fd = open(file, O_RDONLY | O_BINARY);
            if (fd < 0) {
                filePtr = NULL;
                Ns_Log(Warning, "fastpath: open(%s') failed '%s'",
                       file, strerror(errno));
            } else {
 	        int nread;

                filePtr = ns_malloc(sizeof(File) + connPtr->fileInfo.st_size);
                filePtr->refcnt = 1;
                filePtr->size   = connPtr->fileInfo.st_size;
                filePtr->mtime  = connPtr->fileInfo.st_mtime;
                filePtr->dev    = connPtr->fileInfo.st_dev;
                filePtr->ino    = connPtr->fileInfo.st_ino;
                nread = read(fd, filePtr->bytes, filePtr->size);
                close(fd);
                if (nread != filePtr->size) {
                    Ns_Log(Warning, "fastpath: failed to read '%s': '%s'",
                           file, strerror(errno));
                    ns_free(filePtr);
                    filePtr = NULL;
                }
            }
            Ns_CacheLock(cache);
            entry = Ns_CacheCreateEntry(cache, file, &isNew);
            if (filePtr != NULL) {
                Ns_CacheSetValueSz(entry, filePtr,
                                   (size_t) (filePtr->size + sizeof(File)));
            } else {
                Ns_CacheDeleteEntry(entry);
            }
            Ns_CacheBroadcast(cache);
        }
        if (filePtr != NULL) {
            ++filePtr->refcnt;
            Ns_CacheUnlock(cache);
            result = Ns_ConnReturnData(conn, status,
                                       filePtr->bytes, filePtr->size, type);
            Ns_CacheLock(cache);
            DecrEntry(filePtr);
        }
        Ns_CacheUnlock(cache);
        if (filePtr == NULL) {
            goto notfound;
        }
    }

    return result;

 notfound:

    return Ns_ConnReturnNotFound(conn);
}


/*
 *----------------------------------------------------------------------
 *
 * FastStat --
 *
 *      Stat a file, logging an error on unexpected results.
 *
 * Results:
 *      1 if stat OK, 0 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
FastStat(CONST char *path, struct stat *stPtr)
{
    if (stat(path, stPtr) != 0) {
        if (errno != ENOENT && errno != EACCES) {
            Ns_Log(Error, "fastpath: stat(%s) failed: %s",
                   path, strerror(errno));
        }
        return 0;
    }
    return 1;
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

static int
FastGetRestart(Ns_Conn *conn, CONST char *page)
{
    int        status;
    Ns_DString ds;

    Ns_DStringInit(&ds);
    Ns_MakePath(&ds, conn->request->url, page, NULL);
    status = Ns_ConnRedirect(conn, ds.string);
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
 * NsTclCacheStatsObjCmds --
 *
 *      Returns stats on a cache. The size and expirey time of each
 *      entry in the cache is also appended if the -contents switch
 *      is given.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
// document me, maybe refactor me
int
NsTclFastPathCacheStatsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_CacheSearch  search;
    Ns_Entry       *entry;
    Ns_DString      ds;
    Ns_Time        *timePtr;
    int             contents = NS_FALSE, reset = NS_FALSE;

    Ns_ObjvSpec opts[] = {
        {"-contents", Ns_ObjvBool,  &contents, (void *) NS_TRUE},
        {"-reset",    Ns_ObjvBool,  &reset,    (void *) NS_TRUE},
        {"--",        Ns_ObjvBreak, NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    /* if there is no cache defined, return empty */
    if (cache == NULL) {
	return TCL_OK;
    }

    Ns_DStringInit(&ds);
    Ns_CacheLock(cache);

    if (contents) {
        Tcl_DStringStartSublist(&ds);
        entry = Ns_CacheFirstEntry(cache, &search);
        while (entry != NULL) {
	    size_t size = Ns_CacheGetSize(entry);
            timePtr = Ns_CacheGetExpirey(entry);
            if (timePtr->usec == 0) {
                Ns_DStringPrintf(&ds, "%" PRIdz " %" PRIu64 " ",
                                 size, (int64_t) timePtr->sec);
            } else {
                Ns_DStringPrintf(&ds, "%" PRIdz " %" PRIu64 ":%ld ",
                                 size, (int64_t) timePtr->sec, timePtr->usec);
            }
            entry = Ns_CacheNextEntry(&search);
        }
        Tcl_DStringEndSublist(&ds);
    } else {
        Ns_CacheStats(cache, &ds);
    }
    if (reset) {
        Ns_CacheResetStats(cache);
    }
    Ns_CacheUnlock(cache);

    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
}
