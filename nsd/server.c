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
    char          *path, *spath, *map, *key, *dirf, *p;
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
     
    servPtr->opts.realm = Ns_ConfigGetValue(path, "realm");
    if (servPtr->opts.realm == NULL) {
        servPtr->opts.realm = server;
    }
    if (!Ns_ConfigGetBool(path, "checkmodifiedsince", 
                          &servPtr->opts.modsince)) {
        servPtr->opts.modsince = SERV_MODSINCE_BOOL;
    }
    if (!Ns_ConfigGetBool(path, "flushcontent", 
                          &servPtr->opts.flushcontent)) {
        servPtr->opts.flushcontent = SERV_FLUSHCONTENT_BOOL;
    }
    if (!Ns_ConfigGetBool(path, "noticedetail", 
                          &servPtr->opts.noticedetail)) {
        servPtr->opts.noticedetail = SERV_NOTICEDETAIL_BOOL;
    }
    if (!Ns_ConfigGetInt(path, "errorminsize", 
                         &servPtr->opts.errorminsize)) {
        servPtr->opts.errorminsize = SERV_ERRORMINSIZE_INT;
    }
    p = Ns_ConfigGetValue(path, "headercase");
    if (p != NULL && STRIEQ(p, "tolower")) {
        servPtr->opts.hdrcase = ToLower;
    } else if (p != NULL && STRIEQ(p, "toupper")) {
        servPtr->opts.hdrcase = ToUpper;
    } else {
        servPtr->opts.hdrcase = Preserve;
    }
    
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
    servPtr->tcl.library = Ns_ConfigGetValue(path, "library");
    if (servPtr->tcl.library == NULL) {
        Ns_ModulePath(&ds, server, "tcl", NULL);
        servPtr->tcl.library = Ns_DStringExport(&ds);
    }
    servPtr->tcl.initfile = Ns_ConfigGetValue(path, "initfile");
    if (servPtr->tcl.initfile == NULL) {
        Ns_HomePath(&ds, "bin", "init.tcl", NULL);
        servPtr->tcl.initfile = Ns_DStringExport(&ds);
    }
    servPtr->tcl.modules = Tcl_NewObj();
    Tcl_IncrRefCount(servPtr->tcl.modules);
    Ns_RWLockInit(&servPtr->tcl.lock);
    if (!Ns_ConfigGetInt(path, "nsvbuckets", &n) || n < 1) {
        n = TCL_NSVBUCKETS_INT;
    }
    servPtr->nsv.nbuckets = n;
    servPtr->nsv.buckets = NsTclCreateBuckets(server, n);
    Tcl_InitHashTable(&servPtr->share.inits, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->share.vars, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->share.lock, "nstcl:share", server);
    Tcl_InitHashTable(&servPtr->var.table, TCL_STRING_KEYS);
    Tcl_InitHashTable(&servPtr->sets.table, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->sets.lock, "nstcl:sets", server);

    /*
     * Initialize the Tcl detached channel support.
     */

    Tcl_InitHashTable(&servPtr->chans.table, TCL_STRING_KEYS);
    Ns_MutexSetName2(&servPtr->chans.lock, "nstcl:chans", server);

    /*
     * Initialize the fastpath.
     */
     
    path = Ns_ConfigGetPath(server, NULL, "fastpath", NULL);
    if (!Ns_ConfigGetBool(path, "cache", &i) || i) {
        if (!Ns_ConfigGetInt(path, "cachemaxsize", &n)) {
            n = FASTPATH_CACHESIZE_INT;
        }
        if (!Ns_ConfigGetInt(path, "cachemaxentry", &i) || i < 0) {
            i = FASTPATH_CACHEMAXENTRY_INT;
        }
        servPtr->fastpath.cachemaxentry = i;
        servPtr->fastpath.cache =  NsFastpathCache(server, n);
    }
    if (!Ns_ConfigGetBool(path, "mmap", &servPtr->fastpath.mmap)) {
        servPtr->fastpath.mmap = FASTPATH_MMAP_BOOL;
    }
    dirf = Ns_ConfigGetValue(path, "directoryfile");
    if (dirf == NULL) {
        dirf = Ns_ConfigGetValue(spath, "directoryfile");
    }
    if (dirf != NULL) {
        dirf = ns_strdup(dirf);
        p = dirf;
        n = 1;
        while ((p = (strchr(p, ','))) != NULL) {
            ++n;
            ++p;
        }
        servPtr->fastpath.dirc = n;
        servPtr->fastpath.dirv = ns_malloc(sizeof(char *) * n);
        for (i = 0; i < n; ++i) {
            p = strchr(dirf, ',');
            if (p != NULL) {
                *p++ = '\0';
            }
            servPtr->fastpath.dirv[i] = dirf;
            dirf = p;
        }
    }
    servPtr->fastpath.serverdir = Ns_ConfigGetValue(path, "serverdir");
    if (servPtr->fastpath.serverdir == NULL) {
        Ns_MakePath(&ds, Ns_InfoHomePath(), "servers", server, NULL);
        servPtr->fastpath.serverdir = Ns_DStringExport(&ds);
    } else if (!Ns_PathIsAbsolute(servPtr->fastpath.serverdir)) {
        Ns_MakePath(&ds, Ns_InfoHomePath(), servPtr->fastpath.serverdir, NULL);
        servPtr->fastpath.serverdir = Ns_DStringExport(&ds);
    }
    servPtr->fastpath.pagedir = Ns_ConfigGetValue(path, "pagedir");
    if (servPtr->fastpath.pagedir == NULL) {
        servPtr->fastpath.pagedir = "pages";
    }
    if (Ns_PathIsAbsolute(servPtr->fastpath.pagedir)) {
        servPtr->fastpath.pageroot = servPtr->fastpath.pagedir;
    } else {
        Ns_MakePath(&ds, servPtr->fastpath.serverdir,
                    servPtr->fastpath.pagedir, NULL);
        servPtr->fastpath.pageroot = Ns_DStringExport(&ds);
    }
    p = Ns_ConfigGetValue(path, "directorylisting");
    if (p != NULL && (STREQ(p, "simple") || STREQ(p, "fancy"))) {
        p = "_ns_dirlist";
    }
    servPtr->fastpath.dirproc = Ns_ConfigGetValue(path, "directoryproc");
    if (servPtr->fastpath.dirproc == NULL) {
        servPtr->fastpath.dirproc = p;
    }
    servPtr->fastpath.diradp = Ns_ConfigGetValue(path, "directoryadp");
    
    /*
     * Initialize virtual hosting.
     */

    path = Ns_ConfigGetPath(server, NULL, "vhost", NULL);
    if (!Ns_ConfigGetBool(path, "enabled", &servPtr->vhost.enabled)) {
        servPtr->vhost.enabled = VHOST_ENABLED_BOOL;
    }
    if (servPtr->vhost.enabled
        && Ns_PathIsAbsolute(servPtr->fastpath.pagedir)) {
        Ns_Log(Error, "virtual hosting disabled, pagedir not relative: %s",
               servPtr->fastpath.pagedir);
        servPtr->vhost.enabled = NS_FALSE;
    }
    if (!Ns_ConfigGetBool(path, "stripwww", &i) || i) {
        servPtr->vhost.opts |= NSD_STRIP_WWW;
    }
    if (!Ns_ConfigGetBool(path, "stripport", &i) || i) {
        servPtr->vhost.opts |= NSD_STRIP_PORT;
    }
    servPtr->vhost.hostprefix = Ns_ConfigGetValue(path, "hostprefix");
    Ns_ConfigGetInt(path, "hosthashlevel", &servPtr->vhost.hosthashlevel);
    if (servPtr->vhost.hosthashlevel < 0) {
        servPtr->vhost.hosthashlevel = 0;
    }
    if (servPtr->vhost.hosthashlevel > 5) {
        servPtr->vhost.hosthashlevel = 5;
    }
    if (servPtr->vhost.enabled) {
        NsPageRoot(&ds, servPtr, "www.example.com:80");
        Ns_Log(Notice, "vhost[%s]: www.example.com:80 -> %s",server,ds.string);
        Ns_DStringTrunc(&ds, 0);
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
    
    Ns_RegisterRequest(server, "GET", "/", NsFastGet, NULL, servPtr, 0);
    Ns_RegisterRequest(server, "HEAD", "/", NsFastGet, NULL, servPtr, 0);
    Ns_RegisterRequest(server, "POST", "/", NsFastGet, NULL, servPtr, 0);

    /*
     * Initialize ADP.
     */
    
    path = Ns_ConfigGetPath(server, NULL, "adp", NULL);
    servPtr->adp.errorpage = Ns_ConfigGetValue(path, "errorpage");
    servPtr->adp.startpage = Ns_ConfigGetValue(path, "startpage");
    if (!Ns_ConfigGetBool(path, "enableexpire", 
                          &servPtr->adp.enableexpire)) {
        servPtr->adp.enableexpire = ADP_ENABLEEXPIRE_BOOL;
    }
    if (!Ns_ConfigGetBool(path, "enabledebug", 
                          &servPtr->adp.enabledebug)) {
        servPtr->adp.enabledebug = ADP_ENABLEDEBUG_BOOL;
    }
    servPtr->adp.debuginit = Ns_ConfigGetValue(path, "debuginit");
    if (servPtr->adp.debuginit == NULL) {
        servPtr->adp.debuginit = ADP_DEBUGINIT_STRING;
    }
    servPtr->adp.defaultparser = Ns_ConfigGetValue(path, "defaultparser");
    if (servPtr->adp.defaultparser == NULL) {
        servPtr->adp.defaultparser = ADP_DEFPARSER_STRING;
    }
    if (!Ns_ConfigGetInt(path, "cachesize", &n)) {
        n = ADP_CACHESIZE_INT;
    }
    servPtr->adp.cachesize = n;
    
    /*
     * Initialize on-the-fly compression support for ADP.
     */
    
    path = Ns_ConfigGetPath(server, NULL, "adp", "compress", NULL);
    if (!Ns_ConfigGetBool(path, "enable", &servPtr->adp.compress.enable)) {
        servPtr->adp.compress.enable = ADP_ENABLECOMPRESS_BOOL;
    }
    if (!Ns_ConfigGetInt(path, "level", &n) || n < 1 || n > 9) {
        n = ADP_COMPRESSLEVEL_INT;
    }
    servPtr->adp.compress.level = n;
    if (!Ns_ConfigGetInt(path, "minsize", &n) || n < 0) {
        n = 0;
    }
    servPtr->adp.compress.minsize = n;
    
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
    for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
        key = Ns_SetKey(set, i);
        if (!strcasecmp(key, "map")) {
            map = Ns_SetValue(set, i);
            Ns_RegisterRequest(server, "GET",  map,NsAdpProc, NULL, servPtr, 0);
            Ns_RegisterRequest(server, "HEAD", map,NsAdpProc, NULL, servPtr, 0);
            Ns_RegisterRequest(server, "POST", map,NsAdpProc, NULL, servPtr, 0);
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
    NsLoadModules(server);
    NsTclInitServer(server);
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
    
    if (!Ns_ConfigGetInt(path, "maxconnections", &maxconns)) {
        maxconns = SERV_MAXCONNS_INT;
    }
    connBufPtr = ns_calloc((size_t) maxconns, sizeof(Conn));
    for (n = 0; n < maxconns - 1; ++n) {
        connPtr = &connBufPtr[n];
        connPtr->nextPtr = &connBufPtr[n+1];
    }
    connBufPtr[n].nextPtr = NULL;
    poolPtr->queue.freePtr = &connBufPtr[0];
    
    if (!Ns_ConfigGetInt(path, "minthreads", 
                         &poolPtr->threads.min)) {
        poolPtr->threads.min = SERV_MINTHREADS_INT;
    }
    if (!Ns_ConfigGetInt(path, "maxthreads", 
                         &poolPtr->threads.max)) {
        poolPtr->threads.max = SERV_MAXTHREADS_INT;
    }
    if (!Ns_ConfigGetInt(path, "threadtimeout", 
                         &poolPtr->threads.timeout)) {
        poolPtr->threads.timeout = SERV_THREADTIMEOUT_INT;
    }
    
    /*
     * Determine the minimum and maximum number of threads, adjusting the
     * values as needed.  The threadtimeout value is the maximum number of
     * seconds a thread will wait for a connection before exiting if the
     * current number of threads is above the minimum.
     */
    
    if (poolPtr->threads.max > maxconns) {
        Ns_Log(Warning, "serv: cannot have more maxthreads than maxconns: "
               "%d max threads adjusted down to %d max connections",
               poolPtr->threads.max, maxconns);
        poolPtr->threads.max = maxconns;
    }
    if (poolPtr->threads.min > poolPtr->threads.max) {
        Ns_Log(Warning, "serv: cannot have more minthreads than maxthreads: "
               "%d min threads adjusted down to %d max threads",
               poolPtr->threads.min, poolPtr->threads.max);
        poolPtr->threads.min = poolPtr->threads.max;
    }
}
