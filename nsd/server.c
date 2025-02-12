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
NsGetServer(const char *server)
{
    NsServer *result = NULL;

    if (server != NULL) {
        const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&nsconf.servertable, server);

        if (hPtr != NULL) {
            result = Tcl_GetHashValue(hPtr);
        }
    }

    return result;
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
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;

    hPtr = Tcl_FirstHashEntry(&nsconf.servertable, &search);
    while (hPtr != NULL) {
        const NsServer *servPtr = Tcl_GetHashValue(hPtr);

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
NsStopServers(const Ns_Time *toPtr)
{
    NsServer            *servPtr;
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;

    NS_NONNULL_ASSERT(toPtr != NULL);

    hPtr = Tcl_FirstHashEntry(&nsconf.servertable, &search);
    while (hPtr != NULL) {
        servPtr = Tcl_GetHashValue(hPtr);
        NsStopHttp(servPtr);
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
     * Servers must not be defined twice.
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

    /*
     * Run the library init procs in the order they were registered.
     */

    initPtr = firstInitPtr;
    while (initPtr != NULL) {
        (void) (*initPtr->proc)(server);
        initPtr = initPtr->nextPtr;
    }

    section = Ns_ConfigSectionPath(NULL, server, NULL, (char *)0L);

    /*
     * Set some server options.
     */

    servPtr->opts.realm = ns_strcopy(Ns_ConfigString(section, "realm", server));
    servPtr->opts.modsince = Ns_ConfigBool(section, "checkmodifiedsince", NS_TRUE);

    servPtr->opts.noticedetail = Ns_ConfigBool(section, "noticedetail", NS_TRUE);
    servPtr->opts.stealthmode = Ns_ConfigBool(section, "stealthmode", NS_FALSE);
    servPtr->opts.noticeADP = Ns_ConfigString(section, "noticeadp", "returnnotice.adp");
    servPtr->opts.logDir = Ns_ConfigGetValue(section, "logdir");
    if (servPtr->opts.logDir != NULL) {
        servPtr->opts.logDir = Ns_ConfigFilename(section, "logdir", 4, Ns_InfoLogPath(), servPtr->opts.logDir);
    }

    if (Ns_PathIsAbsolute(servPtr->opts.noticeADP) == NS_FALSE
        && *servPtr->opts.noticeADP != '\0') {
        Tcl_DString  ds;
        const char  *fileName;

        Tcl_DStringInit(&ds);
        fileName = Ns_HomePath(&ds, "conf", "/",
                               servPtr->opts.noticeADP, (char *)0L);
        servPtr->opts.noticeADP = ns_strcopy(fileName);
        Tcl_DStringFree(&ds);
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

    Ns_MutexInit(&servPtr->tcl.synch.lock);
    Ns_MutexSetName2(&servPtr->tcl.synch.lock, "nsd:tcl:synch", server);

    Ns_MutexInit(&servPtr->urlspace.lock);
    Ns_MutexSetName2(&servPtr->urlspace.lock, "nsd:urlspace", server);

    /*
     * Load modules and initialize Tcl.  The order is significant.
     */

    CreatePool(servPtr, NS_EMPTY_STRING);
    set = Ns_ConfigGetSection(Ns_ConfigGetPath(server, NULL, "pools",  (char *)0L));

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
        section = Ns_ConfigSectionPath(NULL, servPtr->server, NULL, (char *)0L);
        servPtr->pools.defaultPtr = poolPtr;
    } else {
        Ns_Set *set;
        size_t  i;
        /*
         * Map requested method/URL's to this pool.
         */
        section = Ns_ConfigGetPath(servPtr->server, NULL, "pool", pool,  (char *)0L);
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
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
