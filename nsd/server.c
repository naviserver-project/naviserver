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
 * server.c --
 *
 *      Routines for managing NsServer structures.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

/*
 * Local functions defined in this file.
 */

static void CreatePool(NsServer *servPtr, char *pool);

/*
 * Static variables defined in this file.
 */

static NsServer *initServPtr; /* Holds currently initializing server. */


/*
 *----------------------------------------------------------------------
 *
 * NsGetServer --
 *
 *      Return the NsServer structure, allocating if necessary.
 *
 * Results:
 *      Pointer to NsServer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NsServer *
NsGetServer(CONST char *server)
{
    Tcl_HashEntry *hPtr;

    if (server != NULL) {
        hPtr = Tcl_FindHashEntry(&nsconf.servertable, server);
        if (hPtr != NULL) {
            return Tcl_GetHashValue(hPtr);
        }
    }

    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetInitServer --
 *
 *      Return the currently initializing server.
 *
 * Results:
 *      Pointer to NsServer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NsServer *
NsGetInitServer(void)
{
    return initServPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartServers --
 *
 *      Start all configured servers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See NsStartServer.
 *
 *----------------------------------------------------------------------
 */

void
NsStartServers(void)
{
    NsServer      *servPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    hPtr = Tcl_FirstHashEntry(&nsconf.servertable, &search);
    while (hPtr != NULL) {
        servPtr = Tcl_GetHashValue(hPtr);
        NsStartServer(servPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsStopServers --
 *
 *      Signal stop and wait for all configured servers.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See NsStopServer and NsWaitServer.
 *
 *----------------------------------------------------------------------
 */

void
NsStopServers(Ns_Time *toPtr)
{
    NsServer      *servPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    hPtr = Tcl_FirstHashEntry(&nsconf.servertable, &search);
    while (hPtr != NULL) {
        servPtr = Tcl_GetHashValue(hPtr);
        NsStopServer(servPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    hPtr = Tcl_FirstHashEntry(&nsconf.servertable, &search);
    while (hPtr != NULL) {
        servPtr = Tcl_GetHashValue(hPtr);
        NsWaitServer(servPtr, toPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitServer --
 *
 *      Initialize a virtual server and all its crazy state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Server will later be started.
 *
 *----------------------------------------------------------------------
 */

void
NsInitServer(char *server, Ns_ServerInitProc *initProc)
{
    Tcl_HashEntry *hPtr;
    Ns_DString     ds;
    NsServer      *servPtr;
    CONST char    *path, *spath, *map, *key, *p;
    Ns_Set        *set;
    int            i, n, status;

    hPtr = Tcl_CreateHashEntry(&nsconf.servertable, server, &n);
    if (!n) {
        Ns_Log(Error, "duplicate server: %s", server);
        return;
    }
    Tcl_DStringAppendElement(&nsconf.servers, server);
    servPtr = ns_calloc(1, sizeof(NsServer));
    Tcl_SetHashValue(hPtr, servPtr);
    initServPtr = servPtr;

    /*
     * Create a new NsServer.
     */

    Ns_DStringInit(&ds);
    spath = path = Ns_ConfigGetPath(server, NULL, NULL);
    servPtr->server = server;

    /*
     * Set some server options.
     */

    servPtr->opts.realm = Ns_ConfigString(path, "realm", server);
    servPtr->opts.modsince = Ns_ConfigBool(path, "checkmodifiedsince", NS_TRUE);
    servPtr->opts.flushcontent = Ns_ConfigBool(path, "flushcontent", NS_FALSE);
    servPtr->opts.noticedetail = Ns_ConfigBool(path, "noticedetail", NS_TRUE);
    servPtr->opts.errorminsize = Ns_ConfigInt(path, "errorminsize", 514);
    servPtr->opts.hdrcase = Preserve;
    p = Ns_ConfigString(path, "headercase", "preserve");
    if (STRIEQ(p, "tolower")) {
        servPtr->opts.hdrcase = ToLower;
    } else if (STRIEQ(p, "toupper")) {
        servPtr->opts.hdrcase = ToUpper;
    }

    if (!Ns_ConfigGetBool(path, "gzip", &i) || i) {
    	servPtr->opts.flags |= SERV_GZIP;
    }
    if (!Ns_ConfigGetInt(path, "gzipmin", &i) || i <= 0) {
	i = 4 * 1024;
    }
    servPtr->opts.gzipmin = i;
    if (!Ns_ConfigGetInt(path, "gziplevel", &i) || i < 0 || i > 9) {
	i = 4;
    }
    servPtr->opts.gziplevel = i;

    /*
     * Encoding defaults for the server
     */

    servPtr->encoding.outputCharset = Ns_ConfigGetValue(path, "outputCharset");
    if (servPtr->encoding.outputCharset != NULL) {
        servPtr->encoding.outputEncoding =
            Ns_GetCharsetEncoding(servPtr->encoding.outputCharset);
        if (servPtr->encoding.outputEncoding == NULL) {
            Ns_Fatal("could not find encoding for default output charset \"%s\"",
                     servPtr->encoding.outputCharset);
        }
    } else {
        servPtr->encoding.outputCharset = nsconf.encoding.outputCharset;
        servPtr->encoding.outputEncoding = nsconf.encoding.outputEncoding;
        nsconf.encoding.hackContentTypeP = nsconf.encoding.hackContentTypeP;
    }
    if (servPtr->encoding.outputEncoding != NULL) {
        servPtr->encoding.hackContentTypeP = NS_TRUE;
        Ns_ConfigGetBool(path, "HackContentType",
                         &servPtr->encoding.hackContentTypeP);
    } else {
        nsconf.encoding.hackContentTypeP = NS_FALSE;
    }
    servPtr->encoding.urlCharset = Ns_ConfigGetValue(path, "urlCharset");
    if (servPtr->encoding.urlCharset != NULL) {
        servPtr->encoding.urlEncoding =
            Ns_GetCharsetEncoding(servPtr->encoding.urlCharset);
        if ( servPtr->encoding.urlEncoding == NULL ) {
            Ns_Log(Warning, "no encoding found for charset \"%s\" from config",
                   servPtr->encoding.urlCharset);
        }
    } else {
        servPtr->encoding.urlCharset = nsconf.encoding.urlCharset;
        servPtr->encoding.urlEncoding = nsconf.encoding.urlEncoding;
    }

    /*
     * Initialize Tcl.
     */

    path = Ns_ConfigGetPath(server, NULL, "tcl", NULL);
    servPtr->tcl.library = (char*)Ns_ConfigString(path, "library", "modules/tcl");
    if (!Ns_PathIsAbsolute(servPtr->tcl.library)) {
        Ns_HomePath(&ds, servPtr->tcl.library, NULL);
        servPtr->tcl.library = Ns_DStringExport(&ds);
    }

    servPtr->tcl.initfile = (char*)Ns_ConfigString(path, "initfile", "bin/init.tcl");
    if (!Ns_PathIsAbsolute(servPtr->tcl.initfile) ) {
        Ns_HomePath(&ds, servPtr->tcl.initfile, NULL);
        servPtr->tcl.initfile = Ns_DStringExport(&ds);
    }
    servPtr->tcl.modules = Tcl_NewObj();
    Tcl_IncrRefCount(servPtr->tcl.modules);
    Ns_RWLockInit(&servPtr->tcl.lock);
    Tcl_InitHashTable(&servPtr->tcl.runTable, TCL_STRING_KEYS);
    Ns_MutexInit(&servPtr->tcl.cachelock);
    Tcl_InitHashTable(&servPtr->tcl.caches, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.mutexTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.csTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.semaTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.condTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->tcl.rwTable, TCL_STRING_KEYS);

    servPtr->nsv.nbuckets = Ns_ConfigIntRange(path, "nsvbuckets", 8, 1, INT_MAX);
    servPtr->nsv.buckets = NsTclCreateBuckets(server, servPtr->nsv.nbuckets);
    Tcl_InitHashTable(&servPtr->share.inits, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->share.vars, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->share.lock, "nstcl:share", server);
    Tcl_InitHashTable(&servPtr->var.table, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->sets.table, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->sets.lock, "nstcl:sets", server);

    /*
     * Initialize the list of connection headers to log for Tcl errors.
     */

    p = Ns_ConfigGetValue(path, "errorlogheaders");
    if (p != NULL && Tcl_SplitList(NULL, p, &n, &servPtr->tcl.errorLogHeaders)
        != TCL_OK) {
        Ns_Log(Error, "config: errorlogheaders is not a list: %s", p);
    }

    /*
     * Initialize the Tcl detached channel support.
     */

    Tcl_InitHashTable(&servPtr->chans.table, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->chans.lock, "nstcl:chans", server);

    /*
     * Initialize the fastpath.
     */

    path = Ns_ConfigGetPath(server, NULL, "fastpath", NULL);

    p = Ns_ConfigString(path, "directoryfile", "index.adp index.tcl index.html index.htm");
    if (p != NULL && Tcl_SplitList(NULL, p, &servPtr->fastpath.dirc,
                                   &servPtr->fastpath.dirv) != TCL_OK) {
        Ns_Log(Error, "config: directoryfile is not a list: %s", p);
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
    servPtr->fastpath.diradp = Ns_ConfigGetValue(path, "directoryadp");

    /*
     * Initialize virtual hosting.
     */

    path = Ns_ConfigGetPath(server, NULL, "vhost", NULL);
    servPtr->vhost.enabled = Ns_ConfigBool(path, "enabled", NS_FALSE);
    if (servPtr->vhost.enabled
        && Ns_PathIsAbsolute(servPtr->fastpath.pagedir)) {
        Ns_Log(Error, "virtual hosting disabled, pagedir not relative: %s",
               servPtr->fastpath.pagedir);
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
        NsPageRoot(&ds, servPtr, "www.example.com:80");
        Ns_Log(Notice, "vhost[%s]: www.example.com:80 -> %s",server,ds.string);
        Ns_DStringSetLength(&ds, 0);
    }

    /*
     * Configure the url, proxy and redirect requests.
     */

    Tcl_InitHashTable(&servPtr->request.proxy, TCL_STRING_KEYS);
    Ns_MutexInit(&servPtr->request.plock);
    Ns_MutexSetName2(&servPtr->request.plock, "nsd:proxy", server);
    path = Ns_ConfigGetPath(server, NULL, "redirects", NULL);
    set = Ns_ConfigGetSection(path);
    Tcl_InitHashTable(&servPtr->request.redirect, TCL_ONE_WORD_KEYS);
    for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
        key = Ns_SetKey(set, i);
        map = Ns_SetValue(set, i);
        status = atoi(key);
        if (status <= 0 || *map == '\0') {
            Ns_Log(Error, "return: invalid redirect '%s=%s'", key, map);
        } else {
            Ns_RegisterReturn(status, map);
        }
    }

    /*
     * Register the fastpath requests.
     */

    Ns_RegisterRequest(server, "GET", "/", NsFastPathProc, NULL, servPtr, 0);
    Ns_RegisterRequest(server, "HEAD", "/", NsFastPathProc, NULL, servPtr, 0);
    Ns_RegisterRequest(server, "POST", "/", NsFastPathProc, NULL, servPtr, 0);

    /*
     * Register the url2file procs.
     */

    Ns_RegisterUrl2FileProc(server, "/", Ns_FastUrl2FileProc, NULL, servPtr, 0);
    Ns_SetUrlToFileProc(server, NsUrlToFileProc);

    /*
     * Initialize ADP.
     */

    path = Ns_ConfigGetPath(server, NULL, "adp", NULL);
    servPtr->adp.errorpage = Ns_ConfigString(path, "errorpage", NULL);
    servPtr->adp.startpage = Ns_ConfigString(path, "startpage", NULL);
    servPtr->adp.enableexpire = Ns_ConfigBool(path, "enableexpire", NS_FALSE);
    servPtr->adp.enabledebug = Ns_ConfigBool(path, "enabledebug", NS_FALSE);
    servPtr->adp.debuginit = Ns_ConfigString(path, "debuginit", "ns_adp_debuginit");
    servPtr->adp.defaultparser = Ns_ConfigString(path, "defaultparser", "adp");
    servPtr->adp.cachesize = Ns_ConfigInt(path, "cachesize", 5000*1024);
    servPtr->adp.tracesize = Ns_ConfigInt(path, "tracesize", 40);
    servPtr->adp.bufsize = Ns_ConfigInt(path, "bufsize", 1 * 1024 * 1000);

    servPtr->adp.flags = 0;
    if (Ns_ConfigGetBool(path, "nocache", &i) && i) {
    	servPtr->adp.flags |= ADP_NOCACHE;
    }
    if (Ns_ConfigGetBool(path, "enableexpire", &i) && i) {
    	servPtr->adp.flags |= ADP_EXPIRE;
    }
    if (Ns_ConfigGetBool(path, "enabledebug", &i) && i) {
    	servPtr->adp.flags |= ADP_DEBUG;
    }
    if (Ns_ConfigGetBool(path, "safeeval", &i) && i) {
    	servPtr->adp.flags |= ADP_SAFE;
    }
    if (Ns_ConfigGetBool(path, "singlescript", &i) && i) {
    	servPtr->adp.flags |= ADP_SINGLE;
    }
    if (Ns_ConfigGetBool(path, "gzip", &i) && i) {
    	servPtr->adp.flags |= ADP_GZIP;
    }
    if (Ns_ConfigGetBool(path, "trace", &i) && i) {
    	servPtr->adp.flags |= ADP_TRACE;
    }
    if (!Ns_ConfigGetBool(path, "detailerror", &i) || i) {
    	servPtr->adp.flags |= ADP_DETAIL;
    }
    if (Ns_ConfigGetBool(path, "stricterror", &i) && i) {
    	servPtr->adp.flags |= ADP_STRICT;
    }
    if (Ns_ConfigGetBool(path, "displayerror", &i) && i) {
    	servPtr->adp.flags |= ADP_DISPLAY;
    }
    if (Ns_ConfigGetBool(path, "trimspace", &i) && i) {
    	servPtr->adp.flags |= ADP_TRIM;
    }
    if (!Ns_ConfigGetBool(path, "autoabort", &i) || i) {
    	servPtr->adp.flags |= ADP_AUTOABORT;
    }

    /*
     * Initialize on-the-fly compression support for ADP.
     */

    path = Ns_ConfigGetPath(server, NULL, "adp", "compress", NULL);
    servPtr->adp.compress.enable = Ns_ConfigBool(path, "enable", NS_FALSE);
    servPtr->adp.compress.level = Ns_ConfigIntRange(path, "level", 4, 1, 9);
    servPtr->adp.compress.minsize = Ns_ConfigInt(path, "minsize", 0);

    /*
     * Initialize the page and tag tables and locks.
     */

    Tcl_InitHashTable(&servPtr->adp.pages, FILE_KEYS);
    Ns_MutexInit(&servPtr->adp.pagelock);
    Ns_CondInit(&servPtr->adp.pagecond);
    Ns_MutexSetName2(&servPtr->adp.pagelock, "nsadp:pages", server);
    Tcl_InitHashTable(&servPtr->adp.tags, TCL_STRING_KEYS);
    Ns_RWLockInit(&servPtr->adp.taglock);

    /*
     * Register ADP for any requested URLs.
     */

    path = Ns_ConfigGetPath(server, NULL, "adp", NULL);
    set = Ns_ConfigGetSection(path);

    /*
     *  If ADP processing is not disabled and no map is configured
     *  setup adp hanlders for all .adp files
     */

    key = Ns_ConfigString(path, "map", NULL);
    if (key == NULL && set != NULL &&
        Ns_ConfigBool(path, "disabled", NS_FALSE) == NS_FALSE) {
        Ns_SetUpdate(set, "map", "/*.adp");
    }

    for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
        key = Ns_SetKey(set, i);
        if (!strcasecmp(key, "map")) {
            map = Ns_SetValue(set, i);
            Ns_RegisterRequest(server, "GET",  map, NsAdpProc, NULL, servPtr, 0);
            Ns_RegisterRequest(server, "HEAD", map, NsAdpProc, NULL, servPtr, 0);
            Ns_RegisterRequest(server, "POST", map, NsAdpProc, NULL, servPtr, 0);
            Ns_Log(Notice, "adp[%s]: mapped %s", server, map);
        }
    }

    /*
     * Call the static server init proc, if any, which may register
     * static modules.
     */

    if (initProc != NULL) {
        (*initProc)(server);
    }

    /*
     * Load modules and initialize Tcl.  The order is significant.
     */

    Ns_MutexSetName2(&servPtr->pools.lock, "nsd:queue:", server);
    CreatePool(servPtr, "");
    path = Ns_ConfigGetPath(server, NULL, "pools", NULL);
    set = Ns_ConfigGetSection(path);
    for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
        CreatePool(servPtr, Ns_SetKey(set, i));
    }
    NsTclInitServer(server);
    NsInitStaticModules(server);
    initServPtr = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * CreatePool --
 *
 *      Create a connection thread pool.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Requests for specified URL's will be handled by given pool.
 *
 *----------------------------------------------------------------------
 */

static void
CreatePool(NsServer *servPtr, char *pool)
{
    ConnPool *poolPtr;
    Conn     *connBufPtr, *connPtr;
    int       i, n, maxconns;
    char     *path;
    Ns_Set   *set;

    poolPtr = ns_calloc(1, sizeof(ConnPool));
    poolPtr->pool = pool;
    poolPtr->servPtr = servPtr;
    if (*pool == '\0') {
        /* NB: Default options from pre-4.0 ns/server/server1 section. */
        path = Ns_ConfigGetPath(servPtr->server, NULL, NULL);
        servPtr->pools.defaultPtr = poolPtr;
    } else {

        /*
         * Map requested method/URL's to this pool.
         */

        path = Ns_ConfigGetPath(servPtr->server, NULL, "pool", pool, NULL);
        set = Ns_ConfigGetSection(path);
        for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
            if (!strcasecmp(Ns_SetKey(set, i), "map")) {
                NsMapPool(poolPtr, Ns_SetValue(set, i));
            }
        }
    }

    poolPtr->nextPtr = servPtr->pools.firstPtr;
    servPtr->pools.firstPtr = poolPtr;

    /*
     * Pre-allocate all available connection structures to avoid having
     * to repeatedly allocate and free them at run time and to ensure there
     * is a per-set maximum number of simultaneous connections to handle
     * before NsQueueConn begins to return NS_ERROR.
     */

    maxconns = Ns_ConfigIntRange(path, "maxconnections", 100, 1, INT_MAX);
    connBufPtr = ns_calloc((size_t) maxconns, sizeof(Conn));
    for (n = 0; n < maxconns - 1; ++n) {
        connPtr = &connBufPtr[n];
        connPtr->nextPtr = &connBufPtr[n+1];
    }
    connBufPtr[n].nextPtr = NULL;
    poolPtr->queue.freePtr = &connBufPtr[0];

    poolPtr->threads.max =
        Ns_ConfigIntRange(path, "maxthreads", 10, 0, maxconns);
    poolPtr->threads.min =
        Ns_ConfigIntRange(path, "minthreads", 0, 0, poolPtr->threads.max);
    poolPtr->threads.timeout =
        Ns_ConfigIntRange(path, "threadtimeout", 120, 0, INT_MAX);
}
