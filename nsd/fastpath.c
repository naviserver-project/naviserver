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
 * Get page possibly from a file cache.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

/*
 * The following structure defines the offsets parsed
 * from Range: request header
 */

#define MAX_RANGES      (NS_CONN_MAXBUFS/3)

typedef struct {
    int status;                /* Return status updated, 206 or 416 */
    int count;                 /* Total number of valid ranges parsed */
    struct {
      unsigned long start;     /* Range start position */
      unsigned long end;       /* Range end position */
      unsigned long size;      /* Range absolute size in bytes */
    } offsets[MAX_RANGES];
    unsigned long size;        /* Total file size */
    unsigned long mtime;       /* Last modification time */
} Range;

/*
 * The following structure defines the contents of a file
 * stored in the file cache.
 */

typedef struct {
    time_t mtime;
    int size;
    int refcnt;
    char bytes[1];  /* Grown to actual file size. */
} File;

/*
 * Local functions defined in this file
 */

static Ns_Callback FreeEntry;
static void DecrEntry(File *);
static int UrlIs(char *server, char *url, int dir);
static int FastStat(char *file, struct stat *stPtr);
static int FastReturn(NsServer *servPtr, Ns_Conn *conn, int status,
                      char *type, char *file, struct stat *stPtr);
static int ParseRange(Ns_Conn *conn, Range *rnPtr);
static int ReturnRange(Ns_Conn *conn, Range *rnPtr, int fd, char *data, int len, char *type);

/*
 *----------------------------------------------------------------------
 * NsFastpathCache --
 *
 *      Initialize the fastpath cache.
 *
 * Results:
 *      Pointer to Ns_Cache.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Cache *
NsFastpathCache(char *server, int size)
{
    Ns_DString ds;
    Ns_Cache *fpCache;
    int keys;

#ifdef _WIN32
    keys = TCL_STRING_KEYS;
#else
    keys = FILE_KEYS;
#endif
    Ns_DStringInit(&ds);
    Ns_DStringVarAppend(&ds, "nsfp:", server, NULL);
    fpCache = Ns_CacheCreateSz(ds.string, keys, (size_t) size, FreeEntry);
    Ns_DStringFree(&ds);

    return fpCache;
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
Ns_ConnReturnFile(Ns_Conn *conn, int status, char *type, char *file)
{
    struct stat st;
    char *server;
    NsServer *servPtr;
    
    if (!FastStat(file, &st)) {
        return Ns_ConnReturnNotFound(conn);
    }

    server = Ns_ConnServer(conn);
    servPtr = NsGetServer(server);

    return FastReturn(servPtr, conn, status, type, file, &st);
}


/*
 *----------------------------------------------------------------------
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

char *
Ns_PageRoot(char *server)
{
    NsServer *servPtr = NsGetServer(server);
 
    if (servPtr != NULL) {
        return servPtr->fastpath.pageroot;
    }

    return NULL;
}


/*
 *----------------------------------------------------------------------
 * Ns_SetUrlToFileProc --
 *
 *      Set pointer to custom routine that acts like Ns_UrlToFile();
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
Ns_SetUrlToFileProc(char *server, Ns_UrlToFileProc *procPtr)
{
    NsServer *servPtr = NsGetServer(server);

    servPtr->fastpath.url2file = procPtr;
}


/*
 *----------------------------------------------------------------------
 * Ns_UrlToFile --
 *
 *      Construct the filename that corresponds to a URL.
 *
 * Results:
 *      Return NS_OK on success or NS_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */


int
Ns_UrlToFile(Ns_DString *dsPtr, char *server, char *url)
{
    NsServer *servPtr = NsGetServer(server);
    
    return NsUrlToFile(dsPtr, servPtr, url);
}


/*
 *----------------------------------------------------------------------
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
Ns_UrlIsFile(char *server, char *url)
{
    return UrlIs(server, url, 0);
}

int
Ns_UrlIsDir(char *server, char *url)
{
    return UrlIs(server, url, 1);
}

static int
UrlIs(char *server, char *url, int dir)
{
    Ns_DString ds;
    int is = NS_FALSE;
    struct stat st;

    Ns_DStringInit(&ds);
    if (Ns_UrlToFile(&ds, server, url) == NS_OK 
        && (stat(ds.string, &st) == 0)
        && ((dir && S_ISDIR(st.st_mode))
            || (dir == NS_FALSE && S_ISREG(st.st_mode)))) {
        is = NS_TRUE;
    }
    Ns_DStringFree(&ds);

    return is;
}


/*
 *----------------------------------------------------------------------
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
FastGetRestart(Ns_Conn *conn, char *page)
{
    int status;
    Ns_DString ds;

    Ns_DStringInit(&ds);
    Ns_MakePath(&ds, conn->request->url, page, NULL);
    status = Ns_ConnRedirect(conn, ds.string);
    Ns_DStringFree(&ds);

    return status;
}


/*
 *----------------------------------------------------------------------
 * NsFastGet --
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
NsFastGet(void *arg, Ns_Conn *conn)
{
    Ns_DString ds;
    NsServer *servPtr = arg;
    char *url = conn->request->url;
    int result, i;
    struct stat st;

    Ns_DStringInit(&ds);
    if (NsUrlToFile(&ds, servPtr, url) != NS_OK || !FastStat(ds.string, &st)) {
        goto notfound;
    }
    if (S_ISREG(st.st_mode)) {

        /*
         * Return ordinary files as with Ns_ConnReturnFile.
         */

        result = FastReturn(servPtr, conn, 200, NULL, ds.string, &st);

    } else if (S_ISDIR(st.st_mode)) {

        /*
         * For directories, search for a matching directory file and 
         * restart the connection if found.
         */

        for (i = 0; i < servPtr->fastpath.dirc; ++i) {
            Ns_DStringTrunc(&ds, 0);
            if (NsUrlToFile(&ds, servPtr, url) != NS_OK) {
                goto notfound;
            }
            Ns_DStringVarAppend(&ds, "/", servPtr->fastpath.dirv[i], NULL);
            if ((stat(ds.string, &st) == 0) && S_ISREG(st.st_mode)) {
                if (url[strlen(url) - 1] != '/') {
                    Ns_DStringTrunc(&ds, 0);
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
 * FreeEntry --
 *
 *      Logically remove a cached file from file cache.
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
    File *filePtr = (File *) arg;

    DecrEntry(filePtr);
}


/*
 *----------------------------------------------------------------------
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
 * FastStat --
 *
 *      Stat a file, logging an error on unexpected results.
 *
 * Results:
 *      1 if stat ok, 0 otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
FastStat(char *file, struct stat *stPtr)
{
    if (stat(file, stPtr) != 0) {
        if (errno != ENOENT && errno != EACCES) {
            Ns_Log(Error, "fastpath: stat(%s) failed: %s",
                   file, strerror(errno));
        }
        return 0;
    }

    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * FastReturn --
 *
 *      Return an open file, possibly from cache.
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
FastReturn(NsServer *servPtr, Ns_Conn *conn, int status,
           char *type, char *file, struct stat *stPtr)
{
    int fd, new, nread;
    int result = NS_ERROR;
    Range range;
    char *key;
    Ns_Entry *entPtr;
    File *filePtr;
    FileMap fmap;

#ifndef _WIN32
    FileKey ukey;
#endif

    /*
     *  Initialize the structure for possible Range: requests
     */

    range.status = status;
    range.size = stPtr->st_size;
    range.mtime = stPtr->st_mtime;

    /*
     * Determine the mime type if not given.
     */
    
    if (type == NULL) {
        type = Ns_GetMimeType(file);
    }
    
    /*
     * Set the last modified header if not set yet and, if not
     * modified since last request, return now.
     */
     
    Ns_ConnSetLastModifiedHeader(conn, &stPtr->st_mtime);
    if (Ns_ConnModifiedSince(conn, stPtr->st_mtime) == NS_FALSE) {
        return Ns_ConnReturnNotModified(conn);
    }

    /*
     * For no output (i.e., HEAD request), just send required
     * headers.
     */
     
    if (conn->flags & NS_CONN_SKIPBODY) {
        Ns_ConnSetRequiredHeaders(conn, type, (int) stPtr->st_size);
        return Ns_ConnFlushHeaders(conn, status);
    }

    /*
     * Check if this is a Range: request, if so return requested
     * portion(s) of the file.
     */

    if (ParseRange(conn, &range) == NS_ERROR) {
        Ns_ConnPrintfHeaders(conn, "Content-Range", "bytes */%lu", range.size);
        return Ns_ConnReturnStatus(conn, range.status);
    }

    if (servPtr->fastpath.cache == NULL
        || stPtr->st_size > servPtr->fastpath.cachemaxentry) {

        /*
         * Caching is disabled or the entry is too large for the cache
         * so just send the content directly.
         * First, attempt to map the file, and if not configured or
         * not successful, revert to open/read/close.
         */

        if (servPtr->fastpath.mmap
            && NsMemMap(file, stPtr->st_size, NS_MMAP_READ, &fmap) == NS_OK) {
            result = ReturnRange(conn, &range, -1, fmap.addr, fmap.size, type);
            NsMemUmap(&fmap);
        } else {
            fd = open(file, O_RDONLY | O_BINARY);
            if (fd == -1) {
                Ns_Log(Warning, "fastpath: open(%s) failed: %s", file,
                       strerror(errno));
                goto notfound;
            }
            result = ReturnRange(conn, &range, fd, 0, stPtr->st_size, type);
            close(fd);
        }
        
    } else {

        /*
         * Search for an existing cache entry for this file, validating 
         * the contents against the current file mtime and size.
         */
        
#ifdef _WIN32
        key = file;
#else
        ukey.dev = stPtr->st_dev;
        ukey.ino = stPtr->st_ino;
        key = (char *) &ukey;
#endif
        filePtr = NULL;
        Ns_CacheLock(servPtr->fastpath.cache);
        entPtr = Ns_CacheCreateEntry(servPtr->fastpath.cache, key, &new);

        if (!new) {
            while (entPtr && (filePtr = Ns_CacheGetValue(entPtr)) == NULL) {
                Ns_CacheWait(servPtr->fastpath.cache);
                entPtr = Ns_CacheFindEntry(servPtr->fastpath.cache, key);
            }
            if (filePtr 
                && (filePtr->mtime != stPtr->st_mtime 
                    || filePtr->size != stPtr->st_size)) {
                Ns_CacheUnsetValue(entPtr);
                new = 1;
            }
        }

        if (new) {
            
            /*
             * Read and cache new or invalidated entries in one big chunk.
             */

            Ns_CacheUnlock(servPtr->fastpath.cache);
            fd = open(file, O_RDONLY|O_BINARY);
            if (fd < 0) {
                filePtr = NULL;
                Ns_Log(Warning, "fastpath: failed to open '%s': '%s'", file,
                       strerror(errno));
            } else {
                filePtr = ns_malloc(sizeof(File) + stPtr->st_size);
                filePtr->refcnt = 1;
                filePtr->size = stPtr->st_size;
                filePtr->mtime = stPtr->st_mtime;
                nread = read(fd, filePtr->bytes, (size_t)filePtr->size);
                close(fd);
                if (nread != filePtr->size) {
                    Ns_Log(Warning, "fastpath: failed to read '%s': '%s'",
                           file, strerror(errno));
                    ns_free(filePtr);
                    filePtr = NULL;
                }
            }
            Ns_CacheLock(servPtr->fastpath.cache);
            entPtr = Ns_CacheCreateEntry(servPtr->fastpath.cache, key, &new);
            if (filePtr != NULL) {
                Ns_CacheSetValueSz(entPtr, filePtr, (size_t)filePtr->size);
            } else {
                Ns_CacheFlushEntry(entPtr);
            }
            Ns_CacheBroadcast(servPtr->fastpath.cache);
        }
        if (filePtr != NULL) {
            ++filePtr->refcnt;
            Ns_CacheUnlock(servPtr->fastpath.cache);
            result = ReturnRange(conn, &range, -1, filePtr->bytes, filePtr->size, type);
            Ns_CacheLock(servPtr->fastpath.cache);
            DecrEntry(filePtr);
        }
        Ns_CacheUnlock(servPtr->fastpath.cache);
        if (filePtr == NULL) {
            goto notfound;
        }
    }

    return result;

 notfound:

    return Ns_ConnReturnNotFound(conn);
}


int
NsUrlToFile(Ns_DString *dsPtr, NsServer *servPtr, char *url)
{
    int status;
    
    if (servPtr->fastpath.url2file != NULL) {
        status = (*servPtr->fastpath.url2file)(dsPtr, servPtr->server, url);
    } else {
        NsPageRoot(dsPtr, servPtr, NULL);
        Ns_MakePath(dsPtr, url, NULL);
        status = NS_OK;
    }
    if (status == NS_OK) {
        while (dsPtr->length > 0 && dsPtr->string[dsPtr->length-1] == '/') {
            Ns_DStringTrunc(dsPtr, dsPtr->length-1);
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseRange --
 *
 *      Checks for presence of Range: header, parses it and returns 
 *      the requested offsets.
 *
 *
 * Results:
 *      NS_ERROR if byte-range is syntactically correct but unsatisfiable
 *      NS_OK otherwise and rnPtr->count will contain number of byte ranges
 *      parsed.
 *
 * Side effects:
 *      - All byte-range-sets beyond MAX_RANGES will be ignored
 *      - range->count will be updated with number of byte-range-sets parsed
 *      - range->status may be updated with 206 or 416 codes
 *
 *----------------------------------------------------------------------
 */

static int
ParseRange(Ns_Conn *conn, Range *rnPtr)
{
    int idx = 0;
    char *str;

    rnPtr->count = 0;
    if ((str = Ns_SetIGet(conn->headers, "Range")) == NULL
        || (str = strstr(str,"bytes=")) == NULL) {
        return NS_OK;
    }
    str += 6;

    while(*str && idx < MAX_RANGES-1) {
        /*
         *  Parse the format: first-byte-pos "-" last-byte-pos
         *  The byte positions specified are inclusive. Byte count start at zero.
         */
        if (isdigit(*str)) {
            rnPtr->offsets[idx].start = atol(str);
            while (isdigit(*str)) str++;
            if (*str == '-') {
                str++;
                if (!isdigit(*str)) {
                    rnPtr->offsets[idx].end = rnPtr->size - 1;
                } else {
                    rnPtr->offsets[idx].end = atol(str);
                    if (rnPtr->offsets[idx].start > rnPtr->offsets[idx].end) {
                        return NS_OK;
                    }
                    if (rnPtr->offsets[idx].end >= rnPtr->size) {
                        rnPtr->offsets[idx].end = rnPtr->size - 1;
                    }
                    while (isdigit(*str)) str++;
                }
                /* At this point we have syntactically valid byte-str-set */
                switch (*str) {
                 case ',':
                     str++;
                 case '\0':
                     break;
                 default:
                     return NS_OK;
                }
                /* Range is unsatisfiable */
                if (rnPtr->offsets[idx].start > rnPtr->offsets[idx].end) {
                    rnPtr->status = 416;
                    return NS_ERROR;
                }
                /* Calculate range size */
                rnPtr->offsets[idx].size = (rnPtr->offsets[idx].end - rnPtr->offsets[idx].start) + 1;
                idx++;
                continue;
            }
        } else if (*str == '-') {
            /*
             *  Parse the format: "-" suffix-length
             *  Specifies the last suffix-length bytes of an entity-body
             */
            str++;
            if (!isdigit(*str)) {
                return NS_OK;
            }
            rnPtr->offsets[idx].end = atol(str);
            if (rnPtr->offsets[idx].end > rnPtr->size) {
                rnPtr->offsets[idx].end = rnPtr->size;
            }
            /* Size from the end requested, convert into count */
            rnPtr->offsets[idx].start = rnPtr->size - rnPtr->offsets[idx].end;
            rnPtr->offsets[idx].end = rnPtr->offsets[idx].start + rnPtr->offsets[idx].end - 1;
            /* At this point we have syntactically valid byte-range-set */
            while (isdigit(*str)) str++;
            switch (*str) {
             case ',':
                 str++;
             case '\0':
                 break;
             default:
                 return NS_OK;
            }
            /* Range is unsatisfiable */
            if (rnPtr->offsets[idx].start > rnPtr->offsets[idx].end) {
                rnPtr->status = 416;
                return NS_ERROR;
            }
            /* Calculate range size */
            rnPtr->offsets[idx].size = (rnPtr->offsets[idx].end - rnPtr->offsets[idx].start) + 1;
            idx++;
            continue;
        }
        /* Invalid syntax */
        return NS_OK;
    }
    /* No valid ranges found */
    if (idx == 0) {
        return NS_OK;
    }
    /*
     * Check for If-Range: header here because it depends on valid Range:
     * header, return the whole file if it has been changed
     */
    str = Ns_SetIGet(conn->headers, "If-Range");
    if (str != NULL && rnPtr->mtime > Ns_ParseHttpTime(str)) {
        return NS_OK;
    }
    /*
     * Tell the caller how many ranges are parsed and the what return code
     * should be used
     */
    rnPtr->status = 206;
    rnPtr->count = idx;
    /*
     * Scan all offsets and see if they form one continious range
     */
    for (idx = 1;idx < rnPtr->count;idx++) {
       if (rnPtr->offsets[idx].start - rnPtr->offsets[idx-1].end > 1) {
           break;
       }
    }
    /* It looks like they all one after another, use the first one */
    if (idx == rnPtr->count) {
        rnPtr->offsets[0].end = rnPtr->offsets[idx - 1].end;
        rnPtr->offsets[0].size = (rnPtr->offsets[0].end - rnPtr->offsets[0].start) + 1;
        rnPtr->count = 1;
    }
    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * ReturnRange --
 *
 *	Sets required headers, dumps them, and then writes your data.
 *
 * Results:
 *	NS_OK/NS_ERROR
 *
 * Side effects:
 *	May set numerous headers, will close connection.
 *      MAX_RANGES depends on NS_CONN_MAXBUFS which is used by Ns_ConnSend
 *
 *----------------------------------------------------------------------
 */

static int
ReturnRange(Ns_Conn *conn, Range *rnPtr, int fd, char *data, int len, char *type)
{
    struct iovec bufs[MAX_RANGES*3], *iovPtr = bufs;
    int          i,result = NS_ERROR;
    char         boundary[32];
    time_t       now = time(0);
    Ns_DString   ds;

    switch (rnPtr->count) {
     case 0:
        /* No ranges, return all data */
        if (fd != -1) {
            return Ns_ConnReturnOpenFd(conn, rnPtr->status, type, fd, len);
        }
        result = Ns_ConnReturnData(conn, rnPtr->status, data, len, type);
        break;

     case 1:
        /*
         * For single byte-range-set, global Content-Range: header should be
         * included in the reply
         */
        Ns_ConnPrintfHeaders(conn, "Content-range", "bytes %lu-%lu/%i",
                             rnPtr->offsets[0].start, rnPtr->offsets[0].end, len);
        if (fd != -1) {
            lseek(fd, rnPtr->offsets[0].start, SEEK_SET);
            return Ns_ConnReturnOpenFd(conn, rnPtr->status, type, fd, rnPtr->offsets[0].size);
        }
        Ns_ConnSetRequiredHeaders(conn, type, rnPtr->offsets[0].size);
        Ns_ConnQueueHeaders(conn, rnPtr->status);
        iovPtr->iov_base = data + rnPtr->offsets[0].start;
        iovPtr->iov_len = rnPtr->offsets[0].size;
        result = Ns_ConnSend(conn, iovPtr, 1);
        break;

     default:
        Ns_DStringInit(&ds);
        sprintf(boundary,"%lu",now);
        /* Multiple ranges, return as multipart/byterange */
        Ns_ConnPrintfHeaders(conn, "Content-type","multipart/byteranges; boundary=%s",
                             boundary);
        
        /*
         * Use 3 iovec structures for each range to
         * contain starting boundary and headers, data and closing boundary
         * and send all iov buffers for all ranges at once in mmap mode or
         * in seperate calls for fd mode.
         */

        /* First pass, produce headers and calculate content length */
        rnPtr->size = 0;
        
        for (i = 0;i < rnPtr->count;i++) {
           /* Point to first iov struct for the given index */
           iovPtr = &bufs[i*3];
           /* First io vector in the triple will hold the headers */
           iovPtr->iov_base = &ds.string[ds.length];
           Ns_DStringPrintf(&ds,"--%s\r\n",boundary);
           Ns_DStringPrintf(&ds,"Content-type: %s\r\n",type);
           Ns_DStringPrintf(&ds,"Content-range: bytes %lu-%lu/%i\r\n\r\n",
                            rnPtr->offsets[i].start, rnPtr->offsets[i].end, len);
           iovPtr->iov_len = strlen(iovPtr->iov_base);
           rnPtr->size += iovPtr->iov_len;

           /*
            * Second io vector will contain actual range buffer offset and
            * size. It will be ignored in fd mode.
            */

           iovPtr++;
           iovPtr->iov_base = data + rnPtr->offsets[i].start;
           iovPtr->iov_len = rnPtr->offsets[i].size;
           rnPtr->size += iovPtr->iov_len;

           /* Third io vector will hold closing boundary */
           iovPtr++;
           iovPtr->iov_base = &ds.string[ds.length];
           /* Last boundary should have trailing -- */
           if (i == rnPtr->count - 1) {
               Ns_DStringPrintf(&ds,"\r\n--%s--",boundary);
           }
           Ns_DStringAppend(&ds,"\r\n");
           iovPtr->iov_len = strlen(iovPtr->iov_base);
           rnPtr->size += iovPtr->iov_len;
        }

        /* Second pass, content length is ready, send http headers and data now */
        Ns_ConnSetRequiredHeaders(conn, type, rnPtr->size);
        Ns_ConnQueueHeaders(conn, rnPtr->status);

        /* In fd mode, we send headers and file contents in separate calls */
        if (fd != -1) {
            for (i = 0;i < rnPtr->count;i++) {
               /* Point iovPtr to headers iov buffer */
               iovPtr = &bufs[i*3];
               result = Ns_ConnSend(conn, iovPtr, 1);
               if (result == NS_ERROR) {
                   break;
               }
               /* Send file content directly from open fd */
               lseek(fd, rnPtr->offsets[i].start, SEEK_SET);
               result = Ns_ConnSendFd(conn, fd, rnPtr->offsets[i].size);
               if (result == NS_ERROR) {
                   break;
               }

               /*
                * Point iovPtr to the closing boundary iov buffer,
                * second iov buffer is not used in fd mode
                */

               iovPtr += 2;
               result = Ns_ConnSend(conn, iovPtr, 1);
               if (result == NS_ERROR) {
                   break;
               }
            }
        } else {
            /* In mmap mode we send all iov buffers at once */
            result = Ns_ConnSend(conn, bufs, rnPtr->count * 3);
        }
        Ns_DStringFree(&ds);
        break;
    }

    if (result == NS_OK) {
        result = Ns_ConnClose(conn);
    }
    return result;
}
