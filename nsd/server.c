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
 * server.c --
 *
 *      Routines for managing NsServer structures.
 */

#include "nsd.h"

/*
 * The following structure maintains virtual server
 * configuration callbacks.
 */

typedef struct ServerInit {
    struct ServerInit  *nextPtr;
    Ns_ServerInitProc  *proc;
} ServerInit;


/*
 * Local functions defined in this file.
 */

static void CreatePool(NsServer *servPtr, const char *pool)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * Static variables defined in this file.
 */

static NsServer         *initServPtr;  /* Currently initializing server. */
static const ServerInit *firstInitPtr; /* First in list of server config callbacks. */
static ServerInit       *lastInitPtr;  /* Last in list of server config callbacks. */


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
NsGetServerDebug(const char *server, const char *caller)
{
    NsServer *result = NULL;

    if (server != NULL) {
        const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&nsconf.servertable, server);
        fprintf(stderr, "NsGetServer LOOKUP <%s> %s -> %p\n", server, caller, (const void*) hPtr);

        if (hPtr != NULL) {
            result = Tcl_GetHashValue(hPtr);
        }
    } else {
        fprintf(stderr, "NsGetServer called with NULL server from %s =============================\n", caller);
    }

    return result;
}

NsServer *
NsGetServer(const char *server)
{
    NsServer *result = NULL;
    const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&nsconf.servertable, server);

    if (hPtr != NULL) {
        result = Tcl_GetHashValue(hPtr);
    }

    return result;
}

Ns_Server *
Ns_GetServer(const char *server)
{
    return (Ns_Server *)NsGetServer(server);
}

const char *
Ns_ServerName(const Ns_Server *servPtr)
{
    return ((const NsServer *)servPtr)->server;
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
 * StartServerCB, StopServerCB, WaitServerCB --
 *
 *      Callback functions for NsForeachHashValue().
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      See NsStartServer(), NsStopHttp(), NsStopServer(),
 *      and NsWaitServer().
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
StartServerCB(void *hashValue, const void *UNUSED(ctx))
{
    NsStartServer(hashValue);
    return NS_OK;
}

static Ns_ReturnCode
StopServerCB(void *hashValue, const void *UNUSED(ctx))
{
    NsStopHttp(hashValue);
    NsStopServer(hashValue);
    return NS_OK;
}

static Ns_ReturnCode
WaitServerCB(void *hashValue, const void *ctx)
{
    const Ns_Time *toPtr = ctx;

    NsWaitServer(hashValue, toPtr);
    return NS_OK;
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
    NsForeachHashValue(&nsconf.servertable, StartServerCB, NULL);
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
NsStopServers(const Ns_Time *toPtr)
{
    NS_NONNULL_ASSERT(toPtr != NULL);

    NsForeachHashValue(&nsconf.servertable, StopServerCB, NULL);
    NsForeachHashValue(&nsconf.servertable, WaitServerCB, (const void*)toPtr);
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
NsInitServer(const char *server, Ns_ServerInitProc *initProc)
{
    Tcl_HashEntry     *hPtr;
    NsServer          *servPtr;
    const ServerInit  *initPtr;
    const char        *section, *p;
    const Ns_Set      *set = NULL;
    size_t             i;
    int                n;

    NS_NONNULL_ASSERT(server != NULL);

    Ns_Log(Debug, "NsInitServer called for <%s>", server);

#if 0
    {
        bool         found = NS_FALSE;
        Ns_Set      *set = NULL, **sections;
        Tcl_DString  ds, *dsPtr = &ds;

        /*
         * Before adding a hash entry, double-check, if the specified server was
         * properly defined.
         */
        Tcl_DStringInit(dsPtr);
        Tcl_DStringAppend(dsPtr, "ns/server/", 10);
        Tcl_DStringAppend(dsPtr, server, TCL_INDEX_NONE);

        sections = Ns_ConfigGetSections();

        for (i = 0; sections[i] != NULL; i++) {
            if (strncmp(dsPtr->string, sections[i]->name, (size_t)dsPtr->length) == 0) {
                found = NS_TRUE;
                break;
            }
        }
        Tcl_DStringFree(dsPtr);

        if (!found) {
            Ns_Log(Error, "no section 'ns/server/%s' in configuration file", server);
            return;
        }
    }
#endif

    /*
     * Servers must not be defined twice. Use hash table to avoid duplicates.
     */
    hPtr = Tcl_CreateHashEntry(&nsconf.servertable, server, &n);
    if (n == 0) {
        Ns_Log(Error, "duplicate server: %s", server);
        return;
    }

    /*
     * Create a new NsServer.
     */

    servPtr = ns_calloc(1u, sizeof(NsServer));

    servPtr->server = server;

    Tcl_SetHashValue(hPtr, servPtr);
    Tcl_DStringAppendElement(&nsconf.servers, server);
    initServPtr = servPtr;

    section = Ns_ConfigSectionPath(NULL, server, NULL, NS_SENTINEL);

    /*
     * Set some server options.
     */

    servPtr->opts.realm = ns_strcopy(Ns_ConfigString(section, "realm", server));
    servPtr->opts.modsince = Ns_ConfigBool(section, "checkmodifiedsince", NS_TRUE);

    servPtr->opts.noticedetail = Ns_ConfigBool(section, "noticedetail", NS_TRUE);
    servPtr->opts.stealthmode = Ns_ConfigBool(section, "stealthmode", NS_FALSE);

    /*
     * Resolve "noticeadp" against HOME/conf (the default directory is
     * currently hard-wired, user can provide an absolute directory).
     */
    {
        Tcl_DString  ds;

        Tcl_DStringInit(&ds);
        Tcl_DStringAppend(&ds, nsconf.home, TCL_INDEX_NONE);
        Tcl_DStringAppend(&ds, "/conf", 5);
        servPtr->opts.noticeADP = Ns_ConfigFilename(section, "noticeadp", 9,
                                                    ds.string,
                                                    "returnnotice.adp", NS_FALSE, NS_FALSE);
        Tcl_DStringFree(&ds);
    }

#ifdef NS_WITH_DEPRECATED_5_0
    if (Ns_ConfigGetValue(section, "serverdir") == NULL && servPtr->opts.serverdir != NULL) {
        /*
         * We have a "serverdir" from the deprecated location but none in the
         * server section. Be friendly and use the value from the deprecated
         * location, but provide a warning.
         */
        Ns_Log(Warning, "using the 'serverdir' from the deprecated fastpath section");
    } else {
        if (servPtr->opts.serverdir != NULL) {
            Ns_Log(Notice, "overriding 'serverdir' setting from fastpath section with value from %s", section);
            ns_free_const(servPtr->opts.serverdir);
        }
        servPtr->opts.serverdir = Ns_ConfigFilename(section, "serverdir", 9,
                                                        nsconf.home, NS_EMPTY_STRING,
                                                        NS_TRUE, NS_FALSE);
    }
#else
    servPtr->opts.serverdir = Ns_ConfigFilename(section, "serverdir", 9,
                                                    nsconf.home, NS_EMPTY_STRING,
                                                    NS_TRUE, NS_FALSE);
#endif
    Ns_Log(Notice,  "NsInitServer servPtr->opts.serverdir set to <%s>", servPtr->opts.serverdir);


    /*
     * Resolve and update the server log directory configuration.
     *
     *      This block determines the appropriate log directory for the
     *      server.  If the server-specific log directory is not set, it uses
     *      the global "ns/parameters" section; otherwise, it uses the current
     *      configuration section. The code then completes a relative log
     *      directory path by combining the server's root path with the
     *      configured log directory value.  The "update" flag is set to
     *      NS_FALSE to prevent storing the computed absolute path back into
     *      the configuration database.
     */
    servPtr->opts.logDir = Ns_ConfigGetValue(section, "logdir");
    //Ns_Log(Notice, "??? raw serverlogdir section '%s' <%s>", section, servPtr->opts.logDir);

    {
        const char *fromSection = servPtr->opts.logDir == NULL ? "ns/parameters" : section;
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        servPtr->opts.logDir = Ns_ConfigFilename(fromSection, "logdir", 6,
                                                 Ns_ServerPath(&ds, server, NS_SENTINEL),
                                                 nsconf.logDir, NS_FALSE, NS_FALSE);
        Tcl_DStringFree(&ds);
    }

    /*
     * Optional Server Root Processing Callback
     *
     *      This code block checks if a "serverrootproc" value is defined in
     *      the server configuration section. If present, it creates a Tcl
     *      callback object from the string and allocates a temporary
     *      interpreter for processing. The callback is then registered using
     *      Ns_SetServerRootProc, which sets up the server root processing
     *      routine (NsTclServerRoot) to dynamically complete server root
     *      paths. If registration fails, a warning is logged. Finally, the
     *      temporary interpreter is deallocated.
     */
    {
        const char *rootProcString = Ns_ConfigGetValue(section, "serverrootproc");
        if (rootProcString != NULL) {
            Ns_TclCallback *cbPtr;
            Tcl_Obj        *callbackObj = Tcl_NewStringObj(rootProcString, TCL_INDEX_NONE);
            Tcl_Interp     *interp;

            interp = NsTclAllocateInterp( servPtr);
            Tcl_IncrRefCount(callbackObj);
            cbPtr = Ns_TclNewCallback(interp, (ns_funcptr_t)NsTclServerRoot, callbackObj,
                                      0, NULL);
            Tcl_IncrRefCount(callbackObj);

            if (unlikely(Ns_SetServerRootProc(NsTclServerRoot, cbPtr) != NS_OK)) {
                Ns_Log(Warning, "server init: cannot register serverrootproc");
            }
            Ns_TclDeAllocateInterp(interp);
            //Ns_Log(Notice, "??? serverlogdir NULL, path <%s>", servPtr->opts.logDir);
        }
    }

    servPtr->opts.errorminsize = (int)Ns_ConfigMemUnitRange(section, "errorminsize", NULL, 514, 0, INT_MAX);
    servPtr->filter.rwlocks = Ns_ConfigBool(section, "filterrwlocks", NS_TRUE);

    servPtr->opts.hdrcase = Preserve;
    p = Ns_ConfigString(section, "headercase", "preserve");
    if (STRIEQ(p, "tolower")) {
        servPtr->opts.hdrcase = ToLower;
    } else if (STRIEQ(p, "toupper")) {
        servPtr->opts.hdrcase = ToUpper;
    }

    /*
     * Add server specific extra headers.
     */
    servPtr->opts.extraHeaders = Ns_ConfigSet(section, "extraheaders", NULL);

    /*
     * Initialize on-the-fly compression support.
     */
    servPtr->compress.enable = Ns_ConfigBool(section, "compressenable", NS_FALSE);
#ifndef HAVE_ZLIB_H
    Ns_Log(Warning, "init server %s: compress is enabled, but no zlib support built in",
           server);
#else
    Ns_Log(Notice, "init server %s: using zlib version %s", server, ZLIB_VERSION);
#endif
    servPtr->compress.level = Ns_ConfigIntRange(section, "compresslevel", 4, 1, 9);
    servPtr->compress.minsize = (int)Ns_ConfigMemUnitRange(section, "compressminsize", NULL, 512, 0, INT_MAX);
    servPtr->compress.preinit = Ns_ConfigBool(section, "compresspreinit", NS_FALSE);

    /*
     * Run the library init procs in the order they were registered.
     */
    initPtr = firstInitPtr;
    while (initPtr != NULL) {
        (void) (*initPtr->proc)(server);
        initPtr = initPtr->nextPtr;
    }

    /*
     * Call the static server init proc, if any, which may register
     * static modules.
     */

    if (initProc != NULL) {
        (void) (*initProc)(server);
    }

    /*
     * Initialize and name the mutexes.
     */

    Ns_MutexInit(&servPtr->pools.lock);
    Ns_MutexSetName2(&servPtr->pools.lock, "nsd:pools", server);

    if (servPtr->filter.rwlocks) {
        Ns_RWLockInit(&servPtr->filter.lock.rwlock);
        Ns_RWLockSetName2(&servPtr->filter.lock.rwlock, "nsd:filter", server);
    } else {
        Ns_MutexInit(&servPtr->filter.lock.mlock);
        Ns_MutexSetName2(&servPtr->filter.lock.mlock, "nsd:filter", server);
    }
    Ns_RWLockInit(&servPtr->request.rwlock);
    Ns_RWLockSetName2(&servPtr->request.rwlock, "nsd:auth", server);

    Ns_RWLockInit(&servPtr->opts.rwlock);
    Ns_RWLockSetName2(&servPtr->opts.rwlock, "nsd:opts", server);

    Ns_MutexInit(&servPtr->tcl.synch.lock);
    Ns_MutexSetName2(&servPtr->tcl.synch.lock, "nsd:tcl:synch", server);

    Ns_MutexInit(&servPtr->urlspace.lock);
    Ns_MutexSetName2(&servPtr->urlspace.lock, "nsd:urlspace", server);

    /*
     * Load modules and initialize Tcl.  The order is significant.
     */

    CreatePool(servPtr, NS_EMPTY_STRING);
    set = Ns_ConfigGetSection(Ns_ConfigGetPath(server, NULL, "pools",  NS_SENTINEL));

    for (i = 0u; set != NULL && i < Ns_SetSize(set); ++i) {
        CreatePool(servPtr, Ns_SetKey(set, i));
    }

    /*
     * Initialize infrastructure of ns_http before Tcl init to make it usable
     * from startup scripts.
     */
    NsInitHttp(servPtr);
    NsTclInitServer(server);

    NsInitStaticModules(server);
    initServPtr = NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * NsRegisterServerInit --
 *
 *      Add a libnsd Ns_ServerInitProc to the end of the virtual server
 *      initialization list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Proc will be called when virtual server is initialized.
 *
 *----------------------------------------------------------------------
 */

void
NsRegisterServerInit(Ns_ServerInitProc *proc)
{
    ServerInit *initPtr;

    NS_NONNULL_ASSERT(proc != NULL);

    initPtr = ns_malloc(sizeof(ServerInit));
    initPtr->proc = proc;
    initPtr->nextPtr = NULL;

    if (firstInitPtr == NULL) {
        firstInitPtr = lastInitPtr = initPtr;
    } else {
        lastInitPtr->nextPtr = initPtr;
        lastInitPtr = initPtr;
    }
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
CreatePool(NsServer *servPtr, const char *pool)
{
    ConnPool   *poolPtr;
    Conn       *connBufPtr, *connPtr;
    int         n, maxconns, lowwatermark, highwatermark, queueLength;
    const char *section;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(pool != NULL);

    poolPtr = ns_calloc(1u, sizeof(ConnPool));
    poolPtr->pool = pool;
    poolPtr->servPtr = servPtr;

    if (*pool == '\0') {
        /* NB: Default options from pre-4.0 ns/server/server1 section. */
        section = Ns_ConfigSectionPath(NULL, servPtr->server, NULL, NS_SENTINEL);
        servPtr->pools.defaultPtr = poolPtr;
    } else {
        Ns_Set *set;
        size_t  i;
        /*
         * Map requested method/URL's to this pool.
         */
        section = Ns_ConfigGetPath(servPtr->server, NULL, "pool", pool,  NS_SENTINEL);
        set = Ns_ConfigGetSection2(section, NS_FALSE);
        for (i = 0u; set != NULL && i < Ns_SetSize(set); ++i) {
            const char *key = Ns_SetKey(set, i);

            if ( STREQ(key, "map")
                || STREQ(key, "map-inherit")) {
                NsConfigMarkAsRead(section, i);
                NsMapPool(poolPtr, Ns_SetValue(set, i), 0u);
            }
            if (STREQ(key, "map-noinherit")) {
                NsConfigMarkAsRead(section, i);
                NsMapPool(poolPtr, Ns_SetValue(set, i), NS_OP_NOINHERIT);
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
     *
     * If compression is enabled for this server and the "compresspreinit"
     * parameter is set for this pool, also initialize the compression
     * stream buffers.  This allocates a fair chunk of memory per connection,
     * so skip it if not needed.  The streams will be initialized later
     * if necessary.
     */

    maxconns = Ns_ConfigIntRange(section, "maxconnections", 100, 1, INT_MAX);
    poolPtr->wqueue.maxconns = maxconns;
    connBufPtr = ns_calloc((size_t) maxconns, sizeof(Conn));

    /*
     * Setting connsperthread to > 0 will cause the thread to graceously exit,
     * after processing that many requests, thus initiating kind-of Tcl-level
     * garbage collection.
     */
    poolPtr->threads.connsperthread =
        Ns_ConfigIntRange(section, "connsperthread", 10000, 0, INT_MAX);

    poolPtr->threads.max =
        Ns_ConfigIntRange(section, "maxthreads", 10, 0, maxconns);
    poolPtr->threads.min =
        Ns_ConfigIntRange(section, "minthreads", 1, 1, poolPtr->threads.max);

    Ns_ConfigTimeUnitRange(section, "threadtimeout", "2m", 0, 0, INT_MAX, 0,
                           &poolPtr->threads.timeout);

    poolPtr->wqueue.rejectoverrun = Ns_ConfigBool(section, "rejectoverrun", NS_FALSE);
    Ns_ConfigTimeUnitRange(section, "retryafter", "5s", 0, 0, INT_MAX, 0,
                           &poolPtr->wqueue.retryafter);

    poolPtr->rate.defaultConnectionLimit =
        Ns_ConfigIntRange(section, "connectionratelimit", -1, -1, INT_MAX);
    poolPtr->rate.poolLimit =
        Ns_ConfigIntRange(section, "poolratelimit", -1, -1, INT_MAX);

    if (poolPtr->rate.poolLimit != -1) {
        NsWriterBandwidthManagement = NS_TRUE;
    }
    for (n = 0; n < maxconns - 1; ++n) {
        connPtr = &connBufPtr[n];
        connPtr->nextPtr = &connBufPtr[n+1];
        if (servPtr->compress.enable
            && servPtr->compress.preinit) {
            (void) Ns_CompressInit(&connPtr->cStream);
        }
        connPtr->rateLimit = poolPtr->rate.defaultConnectionLimit;
    }

    connBufPtr[n].nextPtr = NULL;
    poolPtr->wqueue.freePtr = &connBufPtr[0];

    queueLength = maxconns - poolPtr->threads.max;

    highwatermark = Ns_ConfigIntRange(section, "highwatermark", 80, 0, 100);
    lowwatermark =  Ns_ConfigIntRange(section, "lowwatermark", 10, 0, 100);
    poolPtr->wqueue.highwatermark = (queueLength * highwatermark) / 100;
    poolPtr->wqueue.lowwatermark  = (queueLength * lowwatermark) / 100;

    Ns_Log(Notice, "pool %s: queueLength %d low water %d high water %d",
           NsPoolName(pool),
           queueLength, poolPtr->wqueue.lowwatermark,
           poolPtr->wqueue.highwatermark);

    /*
     * To allow one to vary maxthreads at run time, allow potentially
     * maxconns threads to be created. Otherwise, maxthreads would be
     * sufficient.
     */
    poolPtr->tqueue.args = ns_calloc((size_t)maxconns, sizeof(ConnThreadArg));

    Ns_DListInit(&(poolPtr->rate.writerRates));

    /*
     * The Pools are never freed before exit, so there is apparently no need
     * to free connBufPtr, threadQueue.args explicitly, or the connPtr in the
     * pool.
     */
    {
        Tcl_DString ds;
        int         j;

        Tcl_DStringInit(&ds);
        Tcl_DStringAppend(&ds, "nsd:", 4);
        Tcl_DStringAppend(&ds, servPtr->server, TCL_INDEX_NONE);
        Tcl_DStringAppend(&ds, ":", 1);
        Tcl_DStringAppend(&ds, NsPoolName(pool), TCL_INDEX_NONE);

        for (j = 0; j < maxconns; j++) {
            char suffix[64];

            snprintf(suffix, 64u, "connthread:%d", j);
            Ns_MutexInit(&poolPtr->tqueue.args[j].lock);
            Ns_MutexSetName2(&poolPtr->tqueue.args[j].lock, ds.string, suffix);
            Ns_CondInit(&poolPtr->tqueue.args[j].cond);
        }
        Ns_MutexInit(&poolPtr->tqueue.lock);
        Ns_MutexSetName2(&poolPtr->tqueue.lock, ds.string, "tqueue");

        Ns_MutexInit(&poolPtr->wqueue.lock);
        Ns_MutexSetName2(&poolPtr->wqueue.lock, ds.string, "wqueue");
        Ns_CondInit(&poolPtr->wqueue.cond);

        Ns_MutexInit(&poolPtr->threads.lock);
        Ns_MutexSetName2(&poolPtr->threads.lock, ds.string, "threads");

        Ns_MutexInit(&poolPtr->rate.lock);
        Ns_MutexSetName2(&poolPtr->rate.lock, ds.string, "ratelimit");

        Tcl_DStringFree(&ds);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ServerLogDir --
 *
 *      Returns the directory path where the server’s log files are stored.
 *
 *      If the provided NsServer pointer is NULL or its logDir field is not set,
 *      this function returns the default log path obtained from Ns_InfoLogPath().
 *      Otherwise, it returns the logDir value from the server structure.
 *
 * Parameters:
 *      servPtr - Pointer to the NsServer structure representing the server.
 *
 * Results:
 *      A pointer to a null-terminated string containing the log directory path.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_ServerLogDir(const char *server)
{
    const char *result;
    NsServer *servPtr = NsGetServer(server);

    if (servPtr == NULL || servPtr->opts.logDir == NULL) {
        result = Ns_InfoLogPath();
    } else {
        result = servPtr->opts.logDir;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ServerRootProcEnabled --
 *
 *      Determines whether server root processing is enabled for the specified
 *      server.  This function checks if the server can be resolved and if its
 *      virtual host structure has a non-null serverRootProc callback. When
 *      enabled, this callback is used to process relative paths (e.g., for
 *      log directories) relative to the server's root.
 *
 * Parameters:
 *      server - A pointer to a null-terminated string representing the server's name.
 *
 * Results:
 *      Returns true if the server exists and its serverRootProc callback is set; otherwise,
 *      returns false.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
NsServerRootProcEnabled(const NsServer *servPtr)
{
    bool      result;

    if (servPtr == NULL) {
        result = NS_FALSE;
    } else {
        result = (servPtr->vhost.serverRootProc != NULL);
    }
    return result;
}

bool
Ns_ServerRootProcEnabled(const char *server)
{
    return NsServerRootProcEnabled(NsGetServer(server));
}


typedef struct LogfileCtxData {
    const void *handle;
    int fd;
} LogfileCtxData;

typedef struct LogfileCtx {
    const char *filename;
    LogfileCtxData *dataPtr;
} LogfileCtx;


/*
 *----------------------------------------------------------------------
 *
 * LogFileOpen --
 *
 *      Opens the log file specified in the LogfileCtx structure. The file is
 *      opened in append mode. If the file does not exist, it is created with
 *      permission mode 0644.
 *
 * Parameters:
 *      arg - Pointer to a LogfileCtx structure containing the log filename.
 *
 * Results:
 *      Returns the file descriptor of the opened log file, or NS_INVALID_FD
 *      on failure.
 *
 * Side Effects:
 *      Logs an error if the file cannot be opened.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
LogFileOpen(void *arg)
{
    LogfileCtx   *ctx = arg;
    Ns_ReturnCode result = NS_OK;

    ctx->dataPtr->fd = ns_open(ctx->filename, O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (ctx->dataPtr->fd == NS_INVALID_FD) {
        Ns_Log(Error, "logfile open: error '%s' opening '%s'",
               strerror(errno), ctx->filename);
        result = NS_ERROR;

    } else {
        Ns_Log(Notice, "logfile open: opened '%s' fd %d", ctx->filename, ctx->dataPtr->fd);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * LogFileClose --
 *
 *      Closes the log file associated with the file descriptor in the given
 *      LogfileCtx structure.
 *
 * Parameters:
 *      arg - Pointer to a LogfileCtx structure containing the file descriptor
 *            to close.
 *
 * Results:
 *      Returns the result of ns_close() for the file descriptor.
 *
 * Side Effects:
 *      Closes the file.
 *
 *----------------------------------------------------------------------
 */
static int
LogFileClose(void *arg)
{
    LogfileCtx *ctx = arg;

    Ns_Log(Notice, "logfile close: fd %d fn %s", ctx->dataPtr->fd, ctx->filename);

    return ns_close(ctx->dataPtr->fd);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ServerLogGetFd --
 *
 *      Retrieves the file descriptor for a log file associated with a given
 *      server.  The function first obtains the server structure via
 *      NsGetServer(). It then checks a hash table (logfileTable) in the
 *      server's virtual host structure to determine if a file descriptor for
 *      the specified filename is already cached.  If so, the cached file
 *      descriptor is returned; otherwise, the log file is opened, cached (if
 *      successful), and its file descriptor returned.
 *
 * Parameters:
 *      server   - The server name (as a null-terminated string).
 *      filename - The log filename.
 *
 * Results:
 *      The file descriptor for the log file, or NS_INVALID_FD if the server
 *      is not available or the file cannot be opened.
 *
 * Side Effects:
 *      Caches a newly opened file descriptor in the server's logfileTable.
 *
 *----------------------------------------------------------------------
 */
int
Ns_ServerLogGetFd(const char *server, const void *handle, const char *filename)
{
    int       fd;
    NsServer *servPtr = NsGetServer(server);

    NS_NONNULL_ASSERT(filename != NULL);

    if (servPtr == NULL) {
        fd = NS_INVALID_FD;
    } else {
        int            isNew;
        Tcl_HashEntry *hPtr;

        Ns_Log(Notice, "logfile getfd: filename '%s'", filename);

        Ns_MutexLock(&servPtr->vhost.logMutex);
        hPtr = Tcl_CreateHashEntry(&servPtr->vhost.logfileTable, filename, &isNew);
        if (isNew == 0) {
            const LogfileCtxData *dataPtr = Tcl_GetHashValue(hPtr);
            fd = dataPtr->fd;
            Ns_Log(Notice, "logfile getfd: return cached fd %d for '%s'", fd, filename);
        } else {
            LogfileCtx ctx = {filename, NULL};

            ctx.dataPtr = ns_calloc(1u, sizeof(LogfileCtxData));
            ctx.dataPtr->handle = handle;
            LogFileOpen(&ctx);
            fd = ctx.dataPtr->fd;
            /*
             * Remember just valid fds. Don't keep hash entries, when open
             * fails.
             */
            if (fd != NS_INVALID_FD) {
                Tcl_SetHashValue(hPtr, ctx.dataPtr);
            } else {
                ns_free(ctx.dataPtr);
                Tcl_DeleteHashEntry(hPtr);
            }
        }
        Ns_MutexUnlock(&servPtr->vhost.logMutex);
    }

    return fd;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ServerLogCloseAll --
 *
 *      Closes all log file descriptors for a given server that match a
 *      specific handle.
 *
 *      For each log file, it compares the associated handle with the provided
 *      handle.  If a match is found, the function closes the log file using
 *      LogFileClose(), frees the associated data, and removes the entry from
 *      the logfile table. Log messages are issued to indicate which log files
 *      are closed or skipped.
 *
 * Parameters:
 *      server - A pointer to a null-terminated string representing the server name.
 *      handle - A pointer to the handle used to identify the log files to close.
 *
 * Results:
 *      Returns NS_OK if the operation is successful, or NS_ERROR if the server could not be
 *      found.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_ServerLogCloseAll(const char *server, const void *handle)
{
    Ns_ReturnCode result = NS_OK;
    NsServer     *servPtr = NsGetServer(server);

    Ns_Log(Notice, "logfile closeall server '%s' %s", server, (const char*)handle);

    if (servPtr != NULL) {
        Tcl_HashSearch   search;
        Tcl_HashEntry   *hPtr;

        Ns_MutexLock(&servPtr->vhost.logMutex);
        hPtr = Tcl_FirstHashEntry(&servPtr->vhost.logfileTable, &search);

        while (hPtr != NULL) {
            LogfileCtx ctx = {Tcl_GetHashKey(&servPtr->vhost.logfileTable, hPtr),
                              Tcl_GetHashValue(hPtr)
            };
            if (handle == ctx.dataPtr->handle) {
                Ns_Log(Notice, "... closeall %s is  for me: %s", (const char*)ctx.dataPtr->handle, ctx.filename);
                LogFileClose(&ctx);
                ns_free(ctx.dataPtr);
                Tcl_DeleteHashEntry(hPtr);
            } else {
                Ns_Log(Notice, "... closeall %s not for me: %s", (const char*)ctx.dataPtr->handle, ctx.filename);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&servPtr->vhost.logMutex);
    } else {
        result = NS_ERROR;
    }

    return result;
}

/*
 * maybe, we can reuse this.
 */
static inline const char *
Ns_TclGetHashKeyConst(const Tcl_HashTable *table, const Tcl_HashEntry *entry)
{
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wcast-qual"
#endif
    const char *key = (const char *)Tcl_GetHashKey(table, entry);
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
    return key;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ServerLogRollAll --
 *
 *      Performs log file rollover for all log files associated with the
 *      specified server.  The function iterates over the server's logfile
 *      table and for each log file entry, compares its associated handle with
 *      the provided handle. If the handles match, it invokes
 *      Ns_RollFileCondFmt() to perform rollover using the specified roll
 *      format (rollfmt) and maximum backup count (maxbackup).  Diagnostic
 *      messages are logged to indicate which log files are processed and
 *      which are skipped.
 *
 * Parameters:
 *      server    - A null-terminated string specifying the server name.
 *      handle    - A pointer to a handle that identifies the log files to be rolled.
 *      rollfmt   - A format string used to generate the names of rolled log files.
 *      maxbackup - The maximum number of backup log files to keep.
 *
 * Results:
 *      Returns NS_OK if the rollover operation is successful for the matching log files;
 *      otherwise, returns NS_ERROR.
 *
 * Side Effects:
 *      - May trigger rollover (renaming/moving) of log files.
 *      - Logs messages for each log file processed.
 */

Ns_ReturnCode
Ns_ServerLogRollAll(const char *server, const void *handle, const char *rollfmt, TCL_SIZE_T maxbackup)
{
    Ns_ReturnCode result = NS_OK;
    NsServer     *servPtr = NsGetServer(server);

    Ns_Log(Notice, "logfile rollall server '%s' %s", server, (const char *)handle);

    if (servPtr != NULL) {
        Tcl_HashSearch       search;
        const Tcl_HashEntry *hPtr;

        Ns_MutexLock(&servPtr->vhost.logMutex);

#ifdef PRINT_FULL_TABLE
        hPtr = Tcl_FirstHashEntry(&servPtr->vhost.logfileTable, &search);
        while (hPtr != NULL) {
            LogfileCtx ctx = {Ns_TclGetHashKeyConst(&servPtr->vhost.logfileTable, hPtr),
                              Tcl_GetHashValue(hPtr)
            };
            Ns_Log(Notice, "... fd %d '%s'", ctx.fd, ctx.filename);
            hPtr = Tcl_NextHashEntry(&search);
        }
#endif

        hPtr = Tcl_FirstHashEntry(&servPtr->vhost.logfileTable, &search);
        while (hPtr != NULL) {
            LogfileCtx ctx = {Ns_TclGetHashKeyConst(&servPtr->vhost.logfileTable, hPtr),
                              Tcl_GetHashValue(hPtr)
            };

            if (handle == ctx.dataPtr->handle) {
                Ns_Log(Notice, "... rollall %s is  for me: %s", (const char*)ctx.dataPtr->handle, ctx.filename);
                result = Ns_RollFileCondFmt(LogFileOpen, LogFileClose, &ctx,
                                            ctx.filename, rollfmt, maxbackup);
            } else {
                Ns_Log(Notice, "... rollall %s not for me: %s", (const char*)ctx.dataPtr->handle, ctx.filename);

            }

            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&servPtr->vhost.logMutex);

    } else {
        result = NS_ERROR;
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
