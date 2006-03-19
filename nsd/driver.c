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
 * driver.c --
 *
 *      Connection I/O for loadable socket drivers.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


/*
 * Defines for SockRead return and reason codes.
 */

#define SOCK_READY                  0
#define SOCK_MORE                   1
#define SOCK_SPOOL                  2
#define SOCK_ERROR                (-1)
#define SOCK_CLOSE                (-2)
#define SOCK_CLOSETIMEOUT         (-3)
#define SOCK_READTIMEOUT          (-4)
#define SOCK_WRITETIMEOUT         (-5)
#define SOCK_SERVERREJECT         (-6)
#define SOCK_READERROR            (-7)
#define SOCK_WRITEERROR           (-8)
#define SOCK_SHUTERROR            (-9)
#define SOCK_REQUESTURITOOLONG   (-10)
#define SOCK_BADREQUEST          (-11)
#define SOCK_ENTITYTOOLARGE      (-12)

/*
 * LoggingFlag mask values
 */
#define LOGGING_READTIMEOUT      (1<<0)
#define LOGGING_SERVERREJECT     (1<<1)
#define LOGGING_SOCKERROR        (1<<2)
#define LOGGING_SOCKSHUTERROR    (1<<3)


/*
 * The following maintains Host header to server mappings.
 */

typedef struct ServerMap {
    NsServer *servPtr;
    char      location[1];
} ServerMap;

/*
 * Static functions defined in this file.
 */

static Ns_ThreadProc DriverThread;
static Ns_ThreadProc SpoolerThread;
static Ns_ThreadProc WriterThread;

static int   SetServer(Sock *sockPtr);
static Sock *SockAccept(Driver *drvPtr);
static void  SockRelease(Sock *sockPtr, int reason, int err);
static void  SockTrigger(SOCKET sock);
static void  SockTimeout(Sock *sockPtr, Ns_Time *nowPtr, int timeout);
static void  SockClose(Sock *sockPtr, int keep);
static int   SockRead(Sock *sockPtr, int spooler);
static int   SockParse(Sock *sockPtr, int spooler);
static void  SockPoll(Sock *sockPtr, int type, struct pollfd **pfds, 
                      unsigned int *nfds, unsigned int *maxfds,
                      Ns_Time *timeoutPtr);

static int   SockSpoolerQueue(Driver *drvPtr, Sock *sockPtr);
static void  SockWriterRelease(WriterSock *sockPtr, int reason, int err);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable hosts;           /* Host header to server table */

static ServerMap *defMapPtr = NULL;   /* Default srv when not found in table */

static Ns_Mutex   reqLock;             /* Lock for request free list */
static Request   *firstReqPtr = NULL;  /* Free list of request structures */
static Driver    *firstDrvPtr = NULL;  /* First in list of all drivers */

static SOCKET     drvPipe[2];          /* Trigger to wakeup DriverThread */
static Ns_Mutex   drvLock;             /* Lock for close list and shutdown flg*/
static Ns_Cond    drvCond;             /* Cond for stopped flag */

static Ns_Thread  driverThread;        /* Running DriverThread */
static int        drvStopped = 0;      /* Flag: driver thread stopped */
static int        driverShutdown = 0;  /* Flag: shutdown */

static Sock      *firstSockPtr  = NULL;/* Free list of Sock structures */
static Sock      *firstClosePtr = NULL;/* First conn ready for graceful close */

#define Push(x, xs) ((x)->nextPtr = (xs), (xs) = (x))


/*
 *----------------------------------------------------------------------
 *
 * NsInitDrivers --
 *
 *      Init drivers system.
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
NsInitDrivers(void)
{
    Tcl_InitHashTable(&hosts, TCL_STRING_KEYS);
    Ns_MutexSetName(&drvLock, "ns:upload");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DriverInit --
 *
 *      Initialize a driver.
 *
 * Results:
 *      NS_OK if initialized, NS_ERROR if config or other error.
 *
 * Side effects:
 *      Listen socket will be opened later in NsStartDrivers.
 *
 *----------------------------------------------------------------------
 */

int
Ns_DriverInit(char *server, char *module, Ns_DriverInitData *init)
{
    char           *path,*address, *host, *bindaddr, *defproto, *defserver;
    int             i, n, defport;
    ServerMap      *mapPtr;
    Tcl_HashEntry  *hPtr;
    Ns_DString      ds;
    Ns_Set         *set;
    struct in_addr  ia;
    struct hostent *he;
    Driver         *drvPtr;
    DrvWriter      *wrPtr;
    DrvSpooler     *spPtr;
    NsServer       *servPtr = NULL;

    if (server != NULL && (servPtr = NsGetServer(server)) == NULL) {
        return NS_ERROR;
    }
    
    if (init->version != NS_DRIVER_VERSION_1) {
        Ns_Log(Error, "%s: version field of init argument is invalid: %d", 
               module, init->version);
        return NS_ERROR;
    }
    
    path = (init->path ? init->path : Ns_ConfigGetPath(server, module, NULL));
    
    /*
     * Determine the hostname used for the local address to bind
     * to and/or the HTTP location string.
     */

    host = Ns_ConfigGetValue(path, "hostname");
    bindaddr = address = Ns_ConfigGetValue(path, "address");
    defserver = Ns_ConfigGetValue(path, "defaultserver");

    /*
     * If the listen address was not specified, attempt to determine it
     * through a DNS lookup of the specified hostname or the server's
     * primary hostname.
     */

    if (address == NULL) {
        he = gethostbyname(host ? host : Ns_InfoHostname());
        
        /*
         * If the lookup suceeded but the resulting hostname does not
         * appear to be fully qualified, attempt a reverse lookup on the
         * address which often returns the fully qualified name.
         *
         * NB: This is a common but sloppy configuration for a Unix
         * network.
         */
        
        if (he != NULL && he->h_name != NULL &&
            strchr(he->h_name, '.') == NULL) {
            he = gethostbyaddr(he->h_addr, he->h_length, he->h_addrtype);
        }
        
        /*
         * If the lookup suceeded, use the first address in host entry list.
         */
        
        if (he == NULL || he->h_name == NULL) {
            Ns_Log(Error, "%s: could not resolve %s: %s", module,
                   host ? host : Ns_InfoHostname(), strerror(errno));
            return NS_ERROR;
        }
        if (*(he->h_addr_list) == NULL) {
            Ns_Log(Error, "%s: no addresses for %s", module, he->h_name);
            return NS_ERROR;
        }

        memcpy(&ia.s_addr, *(he->h_addr_list), sizeof(ia.s_addr));
        address = ns_inet_ntoa(ia);
        
        /*
         * Finally, if no hostname was specified, set it to the hostname
         * derived from the lookup(s) above.
         */ 
        
        if (host == NULL) {
            host = he->h_name;
        }
    }
    
    /*
     * If the hostname was not specified and not determined by the lookup
     * above, set it to the specified or derived IP address string.
     */
    
    if (host == NULL) {
        host = address;
    }

    /*
     * Set the protocol and port defaults.
     */

    if (init->opts & NS_DRIVER_SSL) {
        defproto = "https";
        defport = 443;
    } else {
        defproto = "http";
        defport = 80;
    }

    /*
     * Allocate a new driver instance and set configurable parameters.
     */

    drvPtr = ns_calloc(1, sizeof(Driver));

    drvPtr->server       = server;
    drvPtr->name         = init->name;
    drvPtr->proc         = init->proc;
    drvPtr->arg          = init->arg;
    drvPtr->opts         = init->opts;
    drvPtr->servPtr      = servPtr;

    drvPtr->maxinput     = Ns_ConfigIntRange(path, "maxinput", 
                                             1024*1024, 1024, INT_MAX);

    drvPtr->maxline      = Ns_ConfigIntRange(path, "maxline", 
                                             4096,       256, INT_MAX);
    
    drvPtr->maxheaders   = Ns_ConfigIntRange(path, "maxheaders",
                                             128,          8, INT_MAX);
    
    drvPtr->bufsize      = Ns_ConfigIntRange(path, "bufsize",
                                             16384,     1024, INT_MAX);

    drvPtr->sndbuf       = Ns_ConfigIntRange(path, "sndbuf",
                                             0,            0, INT_MAX);

    drvPtr->maxqueuesize = Ns_ConfigIntRange(path, "maxqueuesize",
                                             256,          1, INT_MAX);

    drvPtr->rcvbuf       = Ns_ConfigIntRange(path, "rcvbuf",
                                             0,            0, INT_MAX);

    drvPtr->sendwait     = Ns_ConfigIntRange(path, "sendwait",
                                             30,           1, INT_MAX);

    drvPtr->recvwait     = Ns_ConfigIntRange(path, "recvwait",
                                             30,           1, INT_MAX);

    drvPtr->closewait    = Ns_ConfigIntRange(path, "closewait",
                                             2,            0, INT_MAX);

    drvPtr->keepwait     = Ns_ConfigIntRange(path, "keepwait", 
                                             30,           0, INT_MAX);

    drvPtr->backlog      = Ns_ConfigIntRange(path, "backlog",
                                             64,           1, INT_MAX);

    drvPtr->readahead    = Ns_ConfigIntRange(path, "readahead",
                                             drvPtr->bufsize,
                                             drvPtr->bufsize, drvPtr->maxinput);

    drvPtr->keepallmethods = Ns_ConfigBool(path, "keepallmethods", NS_FALSE);

    /*
     * Allow specification of logging or not of various deep
     * socket handling errors.  These all default to Off.
     */

    drvPtr->loggingFlags = 0;
    
    if (Ns_ConfigBool(path, "readtimeoutlogging", NS_FALSE)) {
        drvPtr->loggingFlags |= LOGGING_READTIMEOUT;
    }
    if (Ns_ConfigBool(path, "serverrejectlogging", NS_FALSE)) {
        drvPtr->loggingFlags |= LOGGING_SERVERREJECT;
    }
    if (Ns_ConfigBool(path, "sockerrorlogging", NS_FALSE)) {
        drvPtr->loggingFlags |= LOGGING_SOCKERROR;
    }
    if (Ns_ConfigBool(path, "sockshuterrorlogging", NS_FALSE)) {
        drvPtr->loggingFlags |= LOGGING_SOCKSHUTERROR;
    }

    /*
     * Check if bind address represent valid pathname and if so
     * switch driver to Unix domain sockets mode
     */

    drvPtr->bindaddr = bindaddr;
    if (drvPtr->bindaddr && Ns_PathIsAbsolute(drvPtr->bindaddr)) {
        drvPtr->opts |= NS_DRIVER_UNIX;
    }

    /*
     * Determine the port and then set the HTTP location string either
     * as specified in the config file or constructed from the
     * protocol, hostname and port.
     */

    drvPtr->protocol = ns_strdup(defproto);
    drvPtr->address  = ns_strdup(address);
    drvPtr->port     = Ns_ConfigIntRange(path, "port", defport, 0, 65535);
    drvPtr->location = Ns_ConfigGetValue(path, "location");
    
    if (drvPtr->location != NULL && strstr(drvPtr->location, "://")) {
        drvPtr->location = ns_strdup(drvPtr->location);
    } else {
        Ns_DStringInit(&ds);
        Ns_DStringVarAppend(&ds, drvPtr->protocol, "://", host, NULL);
        if (drvPtr->port != defport) {
            Ns_DStringPrintf(&ds, ":%d", drvPtr->port);
        }
        drvPtr->location = Ns_DStringExport(&ds);
    }

    drvPtr->nextPtr = firstDrvPtr;
    firstDrvPtr = drvPtr;

    /*
     * Check if upload spooler has been disabled
     */

    spPtr = &drvPtr->spooler;
    spPtr->threads = Ns_ConfigIntRange(path, "spoolerthreads", 0, 0, 32);
    
    if (spPtr->threads > 0) {;
        spPtr->uploadsize = Ns_ConfigIntRange(path, "uploadsize", 
                                              2048, 1024, INT_MAX);
        Tcl_InitHashTable(&spPtr->table, TCL_STRING_KEYS);
        Ns_Log(Notice, "%s: enable %d spooler thread(s) "
               "for uploads >= %d bytes", module,
               spPtr->threads, drvPtr->readahead);
        for (i = 0; i < spPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1, sizeof(SpoolerQueue));
            queuePtr->id = i;
            Push(queuePtr, spPtr->firstPtr);
        }
    }

    /*
     * Number of writer threads
     */

    wrPtr = &drvPtr->writer;
    wrPtr->threads = Ns_ConfigIntRange(path, "writerthreads", 0, 0, 32);
    
    if (wrPtr->threads > 0) {
        wrPtr->maxsize = Ns_ConfigIntRange(path, "writersize",
                                           1024*1024, 1024, INT_MAX);
        wrPtr->bufsize = Ns_ConfigIntRange(path, "writerbufsize",
                                           2048, 512, INT_MAX);
        Ns_Log(Notice, "%s: enable %d writer thread(s) "
               "for downloads >= %d bytes, bufsize=%d bytes", 
               module, wrPtr->threads, wrPtr->maxsize, wrPtr->bufsize);
        for (i = 0; i < wrPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1, sizeof(SpoolerQueue));
            queuePtr->id = i;
            Push(queuePtr, wrPtr->firstPtr);
        }
    }

    /*
     * Map Host headers for drivers not bound to servers.
     */

    if (server == NULL) {
        if (defserver == NULL) {
            Ns_Fatal("%s: virtual servers configured,"
                     " but %s has no defaultserver defined", module, path);
        }
        defMapPtr = NULL;
        path = Ns_ConfigGetPath(NULL, module, "servers", NULL);
        set  = Ns_ConfigGetSection(path);
        for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
            server  = Ns_SetKey(set, i);
            host    = Ns_SetValue(set, i);
            servPtr = NsGetServer(server);
            if (servPtr == NULL) {
                Ns_Log(Error, "%s: no such server: %s", module, server);
            } else {
                hPtr = Tcl_CreateHashEntry(&hosts, host, &n);
                if (!n) {
                    Ns_Log(Error, "%s: duplicate host map: %s", module, host);
                } else {
                    Ns_DStringVarAppend(&ds, drvPtr->protocol, "://",host,NULL);
                    mapPtr = ns_malloc(sizeof(ServerMap) + ds.length);
                    mapPtr->servPtr  = servPtr;
                    strcpy(mapPtr->location, ds.string);
                    Ns_DStringSetLength(&ds, 0);
                    if (defMapPtr == NULL && STREQ(defserver, server)) {
                        defMapPtr = mapPtr;
                    }
                    Tcl_SetHashValue(hPtr, mapPtr);
                }
            }
        }
        if (defMapPtr == NULL) {
            Ns_Fatal("%s: default server %s not defined in %s",
                     module, server, path);
        }
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartDrivers --
 *
 *      Listen on all driver address/ports and start the DriverThread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See DriverThread.
 *
 *----------------------------------------------------------------------
 */

void
NsStartDrivers(void)
{
    Driver *drvPtr;
    SpoolerQueue *queuePtr;

    /*
     * Listen on all drivers.
     */

    drvPtr = firstDrvPtr;

    while (drvPtr != NULL) {
        if (drvPtr->opts & NS_DRIVER_UDP) {
            drvPtr->sock = Ns_SockListenUdp(drvPtr->bindaddr,
                                            drvPtr->port);

        } else if (drvPtr->opts & NS_DRIVER_UNIX) {
            drvPtr->sock = Ns_SockListenUnix(drvPtr->bindaddr,
                                             drvPtr->backlog);
        } else {
            drvPtr->sock = Ns_SockListenEx(drvPtr->bindaddr,
                                           drvPtr->port,drvPtr->backlog);
        }
        if (drvPtr->sock == INVALID_SOCKET) {
            Ns_Log(Error, "%s: failed to listen on %s:%d: %s",
                   drvPtr->name, drvPtr->address, drvPtr->port,
                   ns_sockstrerror(ns_sockerrno));
        } else {

            Ns_SockSetNonBlocking(drvPtr->sock);
            Ns_Log(Notice, "%s: listening on %s:%d",
                   drvPtr->name, drvPtr->address, drvPtr->port);

            /*
             * Create the spooler thread(s).
             */
            
            queuePtr = drvPtr->spooler.firstPtr;
            while (queuePtr) {
                if (ns_sockpair(queuePtr->pipe) != 0) {
                    Ns_Fatal("driver: ns_sockpair() failed: %s", 
                             ns_sockstrerror(ns_sockerrno));
                }
                Ns_ThreadCreate(SpoolerThread, queuePtr, 0,
                                &queuePtr->thread);
                queuePtr = queuePtr->nextPtr;
            }

            /*
             * Create the writer thread(s)
             */

            queuePtr = drvPtr->writer.firstPtr;
            while (queuePtr) {
                if (ns_sockpair(queuePtr->pipe) != 0) {
                    Ns_Fatal("driver: ns_sockpair() failed: %s",
                             ns_sockstrerror(ns_sockerrno));
                }
                Ns_ThreadCreate(WriterThread, queuePtr, 0, 
                                &queuePtr->thread);
                queuePtr = queuePtr->nextPtr;
            }
        }
        drvPtr = drvPtr->nextPtr;
    }

    /*
     * Create the driver thread.
     */

    if (firstDrvPtr != NULL) {
        if (ns_sockpair(drvPipe) != 0) {
            Ns_Fatal("driver: ns_sockpair() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }
        Ns_ThreadCreate(DriverThread, NULL, 0, &driverThread);
    } else {
        Ns_Log(Warning, "no communication drivers configured");
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsStopDrivers --
 *
 *      Trigger the DriverThread to begin shutdown.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      DriverThread will close listen sockets and then exit after all
 *      outstanding connections are complete and closed.
 *
 *----------------------------------------------------------------------
 */

void
NsStopDrivers(void)
{
    int i;
    Driver *drvPtr;
    SpoolerQueue *queuePtr, *queueList[2];

    Ns_MutexLock(&drvLock);
    if (!drvStopped && !driverShutdown) {
        Ns_Log(Notice, "driver: triggering shutdown");
        driverShutdown = 1;
        SockTrigger(drvPipe[1]);
    }
    Ns_MutexUnlock(&drvLock);

    /*
     * Shutdown all spooler and writer threads
     */

    drvPtr = firstDrvPtr;
    while (drvPtr != NULL) {
        queueList[0] = drvPtr->writer.firstPtr;
        queueList[1] = drvPtr->spooler.firstPtr;
        for (i = 0; i < 2; i++) {
            queuePtr = queueList[i];
            while (queuePtr) {
                Ns_MutexLock(&queuePtr->lock);
                if (!queuePtr->stopped && !queuePtr->shutdown) {
                    Ns_Log(Notice, "%s%d: triggering shutdown", 
                           (i ? "spooler" : "writer"), queuePtr->id);
                    queuePtr->shutdown = 1;
                    SockTrigger(queuePtr->pipe[1]);
                }
                Ns_MutexUnlock(&queuePtr->lock);
                queuePtr = queuePtr->nextPtr;
            }
        }
        drvPtr = drvPtr->nextPtr;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsWaitDriversShutdown --
 *
 *      Wait for exit of DriverThread.  This callback is invoke later by
 *      the timed shutdown thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Driver thread is joined and trigger pipe closed.
 *
 *----------------------------------------------------------------------
 */

void
NsWaitDriversShutdown(Ns_Time *toPtr)
{
    int i, status = NS_OK;
    Driver *drvPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch search;
    SpoolerQueue *queuePtr, *queueList[2];

    Ns_MutexLock(&drvLock);
    while (!drvStopped && status == NS_OK) {
        status = Ns_CondTimedWait(&drvCond, &drvLock, toPtr);
    }
    Ns_MutexUnlock(&drvLock);
    if (status != NS_OK) {
        Ns_Log(Warning, "driver: timeout waiting for shutdown");
    } else {
        Ns_Log(Notice, "driver: shutdown complete");
        driverThread = NULL;
        ns_sockclose(drvPipe[0]);
        ns_sockclose(drvPipe[1]);
        hPtr = Tcl_FirstHashEntry(&hosts, &search);
        while (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
            hPtr = Tcl_NextHashEntry(&search);
        }
    }

    /*
     * Wait for shutdown of all spooler and writer threads
     */

    drvPtr = firstDrvPtr;

    while (drvPtr != NULL) {
        queueList[0] = drvPtr->writer.firstPtr;
        queueList[1] = drvPtr->spooler.firstPtr;
        for (i = 0; i < 2; i++) {
            queuePtr = queueList[i];
            while (queuePtr) {
                status = NS_OK;
                Ns_MutexLock(&queuePtr->lock);
                while (!queuePtr->stopped && status == NS_OK) {
                    status = Ns_CondTimedWait(&queuePtr->cond,
                                              &queuePtr->lock, toPtr);
                }
                Ns_MutexUnlock(&queuePtr->lock);
                if (status != NS_OK) {
                    Ns_Log(Warning, "%s%d: timeout waiting for shutdown",
                           (i ? "spooler" : "writer"), queuePtr->id);
                } else {
                    Ns_Log(Notice, "%s%d: shutdown complete",
                           (i ? "spooler" : "writer"), queuePtr->id);
                    queuePtr->thread = NULL;
                    ns_sockclose(queuePtr->pipe[0]);
                    ns_sockclose(queuePtr->pipe[1]);
                }
                queuePtr = queuePtr->nextPtr;
            }
        }
        drvPtr = drvPtr->nextPtr;
    }
}


/* 
 *----------------------------------------------------------------------
 *
 * NsGetRequest --
 *
 *      Return the request buffer, reading it if necessary (i.e., if
 *      not an async read-ahead connection).  This function is called
 *      at the start of connection processing.
 *
 * Results:
 *      Pointer to Request structure or NULL on error.
 *
 * Side effects:
 *      May wait for content to arrive if necessary.
 *
 *----------------------------------------------------------------------
 */

Request *
NsGetRequest(Sock *sockPtr)
{
    Request *reqPtr;
    int status;

    if (sockPtr->reqPtr == NULL) {
        do {
            status = SockRead(sockPtr, 0);
        } while (status == SOCK_MORE);
        if (status != SOCK_READY) {
            if (sockPtr->reqPtr != NULL) {
                NsFreeRequest(sockPtr->reqPtr);
            }
            sockPtr->reqPtr = NULL;
        }
    }
    reqPtr = sockPtr->reqPtr;
    /* NB: Sock is no longer responsible for freeing request. */
    sockPtr->reqPtr = NULL;

    return reqPtr;
}


/* 
 *----------------------------------------------------------------------
 *
 * NsFreeRequest --
 *
 *      Free a connection request structure.  This routine is called
 *      at the end of connection processing or on a socket which
 *      times out during async read-ahead.
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
NsFreeRequest(Request *reqPtr)
{
    if (reqPtr != NULL) {

        reqPtr->next       = NULL;
        reqPtr->content    = NULL;
        reqPtr->length     = 0;
        reqPtr->avail      = 0;
        reqPtr->coff       = 0;
        reqPtr->woff       = 0;
        reqPtr->roff       = 0;
        reqPtr->leadblanks = 0;

        Tcl_DStringFree(&reqPtr->buffer);
        
        Ns_SetTrunc(reqPtr->headers, 0);

        Ns_FreeRequest(reqPtr->request);
        reqPtr->request = NULL;
        
        Ns_MutexLock(&reqLock);
        reqPtr->nextPtr = firstReqPtr;
        firstReqPtr = reqPtr;
        Ns_MutexUnlock(&reqLock);
    }
}


/* 
 *----------------------------------------------------------------------
 *
 * NsSockSend --
 *
 *      Send buffers via the socket's driver callback.
 *
 * Results:
 *      # of bytes sent or -1 on error.
 *
 * Side effects:
 *      Depends on driver proc.
 *
 *----------------------------------------------------------------------
 */

int
NsSockSend(Sock *sockPtr, struct iovec *bufs, int nbufs)
{
    Ns_Sock *sock = (Ns_Sock *) sockPtr;

    return (*sockPtr->drvPtr->proc)(DriverSend, sock, bufs, nbufs);
}


/* 
 *----------------------------------------------------------------------
 *
 * NsSockClose --
 *
 *      Return a connction to the DriverThread for closing or keepalive.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Socket may be reused by a keepalive connection.
 *
 *----------------------------------------------------------------------
 */

void
NsSockClose(Sock *sockPtr, int keep)
{
    int trigger = 0;

    SockClose(sockPtr, keep);

    Ns_MutexLock(&drvLock);
    if (firstClosePtr == NULL) {
        trigger = 1;
    }
    sockPtr->keep    = keep;
    sockPtr->nextPtr = firstClosePtr;
    firstClosePtr    = sockPtr;
    Ns_MutexUnlock(&drvLock);

    if (trigger) {
        SockTrigger(drvPipe[1]);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DriverThread --
 *
 *      Main listening socket driver thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are accepted on the configured listen sockets,
 *      placed on the run queue to be serviced, and gracefully
 *      closed when done.  Async sockets have the entire request read
 *      here before queuing as well.
 *
 *----------------------------------------------------------------------
 */

static void
DriverThread(void *ignored)
{
    char          *errstr, c, drain[1024];
    int            n, stopping, pollto;
    Sock          *sockPtr, *closePtr, *nextPtr, *waitPtr, *readPtr;
    Ns_Time        timeout, now, diff;
    Driver        *drvPtr;
    unsigned int   nfds;   /* Number of Sock to poll(). */
    unsigned int   maxfds; /* Max pollfd's in pfds. */
    struct pollfd *pfds;   /* Array of pollfds to poll(). */

    Ns_ThreadSetName("-driver-");
    Ns_Log(Notice, "starting");

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "driver: accepting connections");
    closePtr = waitPtr = readPtr = NULL;
    Ns_GetTime(&now);
    stopping = 0;
    maxfds = 100;
    pfds = ns_malloc(sizeof(struct pollfd) * maxfds);
    pfds[0].fd = drvPipe[0];
    pfds[0].events = POLLIN;

    while (!stopping) {

        /*
         * Set the bits for all active drivers if a connection
         * isn't already pending.
         */
        
        nfds = 1;
        if (waitPtr == NULL) {
            drvPtr = firstDrvPtr;
            while (drvPtr != NULL) {
                if (drvPtr->sock != INVALID_SOCKET) {
                    pfds[nfds].fd = drvPtr->sock;
                    pfds[nfds].events = POLLIN;
                    drvPtr->pidx = nfds++;
                    drvPtr = drvPtr->nextPtr;
                }
            }
        }
        
        /*
         * If there are any closing or read-ahead sockets, set the bits
         * and determine the minimum relative timeout.
         */
        
        if (readPtr == NULL && closePtr == NULL) {
            pollto = 60 * 1000;
        } else {
            timeout.sec = INT_MAX;
            timeout.usec = LONG_MAX;
            sockPtr = readPtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, POLLIN, &pfds, &nfds, &maxfds, &timeout);
                sockPtr = sockPtr->nextPtr;
            }
            sockPtr = closePtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, POLLIN, &pfds, &nfds, &maxfds, &timeout);
                sockPtr = sockPtr->nextPtr;
            }
            if (Ns_DiffTime(&timeout, &now, &diff) > 0)  {
                pollto = diff.sec * 1000 + diff.usec / 1000;
            } else {
                pollto = 0;
            }
        }
        
        /*
         * Select and drain the trigger pipe if necessary.
         */

        pfds[0].revents = 0;

        do {
            n = poll(pfds, nfds, pollto);
        } while (n < 0  && errno == EINTR);

        if (n < 0) {
            errstr = ns_sockstrerror(ns_sockerrno);
            Ns_Fatal("driver: poll() failed: %s", errstr);
        }

        if ((pfds[0].revents & POLLIN) 
            && recv(drvPipe[0], &c, 1, 0) != 1) {
            errstr = ns_sockstrerror(ns_sockerrno);
            Ns_Fatal("driver: trigger recv() failed: %s", errstr);
        }
        
        /*
         * Update the current time and drain and/or release any
         * closing sockets.
         */

        Ns_GetTime(&now);

        if (closePtr != NULL) {
            sockPtr  = closePtr;
            closePtr = NULL;
            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                if (pfds[sockPtr->pidx].revents & POLLIN) {
                    n = recv(sockPtr->sock, drain, sizeof(drain), 0);
                    if (n <= 0) {
                        sockPtr->timeout = now;
                    }
                }
                if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    SockRelease(sockPtr, SOCK_CLOSETIMEOUT, 0);
                } else {
                    Push(sockPtr, closePtr);
                }
                sockPtr = nextPtr;
            }
        }
        
        /*
         * Attempt read-ahead of any new connections.
         */
        
        sockPtr = readPtr;
        readPtr = NULL;

        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            if (!(pfds[sockPtr->pidx].revents & POLLIN)) {
                if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                } else {
                    Push(sockPtr, readPtr);
                }

            } else {

                /*
                 * If enabled, perform read-ahead now.
                 */
                
                sockPtr->keep = 0;
                if (sockPtr->drvPtr->opts & NS_DRIVER_ASYNC) {
                    n = SockRead(sockPtr, 0);
                } else {
                    n = SOCK_READY;
                }
                
                /*
                 * Queue for connection processing if ready.
                 */
                
                switch (n) {
                case SOCK_SPOOL:
                    if (!SockSpoolerQueue(sockPtr->drvPtr, sockPtr)) {
                        Push(sockPtr, readPtr);
                    }
                    break;

                case SOCK_MORE:
                    SockTimeout(sockPtr, &now, sockPtr->drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;

                case SOCK_READY:
                    if (!SetServer(sockPtr)) {
                        SockRelease(sockPtr, SOCK_SERVERREJECT, 0);
                    } else {
                        Push(sockPtr, waitPtr);
                    }
                    break;

                default:
                    SockRelease(sockPtr, n, errno);
                    break;
                }
            }
            sockPtr = nextPtr;
        }
        
        /*
         * Attempt to queue any pending connection
         * after reversing the list to ensure oldest
         * connections are tried first.
         */

        if (waitPtr != NULL) {
            sockPtr = NULL;
            while ((nextPtr = waitPtr) != NULL) {
                waitPtr = nextPtr->nextPtr;
                Push(nextPtr, sockPtr);
            }

            /*
             * Hint: NsQueueConn may fail to queue a certain
             * socket to the designated connection queue. 
             * In such case, ALL ready sockets will be put on
             * the waiting list until the next interation, 
             * regardless of which connection queue they are 
             * to be queued.
             */

            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                if (waitPtr != NULL || !NsQueueConn(sockPtr, &now)) {
                    Push(sockPtr, waitPtr);
                }
                sockPtr = nextPtr;
            }
        }
        
        /*
         * If no connections are waiting, attempt to accept more.
         */
        
        if (waitPtr == NULL) {
            drvPtr = firstDrvPtr;
            while (drvPtr != NULL) {
                if (drvPtr->queuesize < drvPtr->maxqueuesize
                    && (pfds[drvPtr->pidx].revents & POLLIN)
                    && (sockPtr = SockAccept(drvPtr)) != NULL) {

                    /*
                     * Queue the socket immediately if request is provided
                     */

                    n = (*sockPtr->drvPtr->proc)(DriverAccept,
                                                 (Ns_Sock*)sockPtr, 0, 0);
                    if (n == NS_OK) {
                        if (sockPtr->reqPtr == NULL || !SetServer(sockPtr)) {
                            SockRelease(sockPtr, SOCK_SERVERREJECT, 0);
                        } else {
                            if (!NsQueueConn(sockPtr, &now)) {
                                Push(sockPtr, waitPtr);
                            }
                        }
                    } else {

                       /*
                        * Put the socket on the read-ahead list.
                        */

                        SockTimeout(sockPtr, &now, sockPtr->drvPtr->recvwait);
                        Push(sockPtr, readPtr);
                    }
                }
                drvPtr = drvPtr->nextPtr;
            }
        }
        
        /*
         * Check for shutdown and get the list of any closing
         * or keepalive sockets.
         */
        
        Ns_MutexLock(&drvLock);
        sockPtr       = firstClosePtr;
        firstClosePtr = NULL;
        stopping      = driverShutdown;
        Ns_MutexUnlock(&drvLock);
        
        /*
         * Update the timeout for each closing socket and add to the
         * close list if some data has been read from the socket
         * (i.e., it's not a closing keep-alive connection).
         */
        
        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            if (sockPtr->keep) {
                SockTimeout(sockPtr, &now, sockPtr->drvPtr->keepwait);
                Push(sockPtr, readPtr);
            } else {
                if (shutdown(sockPtr->sock, 1) != 0) {
                    SockRelease(sockPtr, SOCK_SHUTERROR, errno);
                } else {
                    SockTimeout(sockPtr, &now, sockPtr->drvPtr->closewait);
                    Push(sockPtr, closePtr);
                }
            }
            sockPtr = nextPtr;
        }
        
        /*
         * Close the active drivers if shutdown is pending.
         */

        if (stopping) {
            drvPtr = firstDrvPtr;
            while (drvPtr != NULL) {
                if (drvPtr->sock != INVALID_SOCKET) {
                    ns_sockclose(drvPtr->sock);
                    drvPtr->sock = INVALID_SOCKET;
                }
                drvPtr = drvPtr->nextPtr;
            }
        }
    }
    
    Ns_Log(Notice, "exiting");
    Ns_MutexLock(&drvLock);
    drvStopped = 1;
    Ns_CondBroadcast(&drvCond);
    Ns_MutexUnlock(&drvLock);
}


/*
 *----------------------------------------------------------------------
 *
 * SetServer --
 *
 *      Set virtual server from driver context or Host header.
 *
 * Results:
 *      1 if valid server set, 0 otherwise.
 *
 * Side effects:
 *      Will update sockPtr->servPtr.
 *
 *----------------------------------------------------------------------
 */

static int
SetServer(Sock *sockPtr)
{
    ServerMap     *mapPtr = NULL;
    Tcl_HashEntry *hPtr;
    char          *host = NULL;
    int            status = 1;
    
    sockPtr->servPtr  = sockPtr->drvPtr->servPtr;
    sockPtr->location = sockPtr->drvPtr->location;

    if (sockPtr->reqPtr) {
        host = Ns_SetIGet(sockPtr->reqPtr->headers, "Host");
        if (!host && sockPtr->reqPtr->request->version >= 1.1) {
            status = 0;
        }
    }
    if (sockPtr->servPtr == NULL) {
        if (host) {
            hPtr = Tcl_FindHashEntry(&hosts, host);
            if (hPtr != NULL) {
                mapPtr = Tcl_GetHashValue(hPtr);
            }
        }
        if (!mapPtr) {
            mapPtr = defMapPtr;
        }
        if (mapPtr) {
            sockPtr->servPtr  = mapPtr->servPtr;
            sockPtr->location = mapPtr->location;
        }
        if (sockPtr->servPtr == NULL) {
            status = 0;
        }
    }
    
    if (!status && sockPtr->reqPtr) {
        ns_free(sockPtr->reqPtr->request->method);
        sockPtr->reqPtr->request->method = ns_strdup("BAD");
    }

    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * SockPoll --
 *
 *      Arrange for given Sock to be monitored. 
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sock fd will be monitored for readability on next spin of
 *      DriverThread.
 *
 *----------------------------------------------------------------------
 */

static void
SockPoll(Sock *sockPtr, int type, struct pollfd **pfds, unsigned int *nfds,
         unsigned int *maxfds, Ns_Time *timeoutPtr)
{
    /*
     * Grow the pfds array if necessary.
     */

    if (*nfds >= *maxfds) {
        *maxfds += 100;
        *pfds = ns_realloc(*pfds, *maxfds * sizeof(struct pollfd));
    }
    
    /*
     * Set the next pollfd struct with this socket.
     */
    
    (*pfds)[*nfds].fd      = sockPtr->sock;
    (*pfds)[*nfds].events  = type;
    (*pfds)[*nfds].revents = 0;

    sockPtr->pidx = (*nfds)++;

    /* 
     * Check for new minimum timeout.
     */

    if (Ns_DiffTime(&sockPtr->timeout, timeoutPtr, NULL) < 0) {
        *timeoutPtr = sockPtr->timeout;
    }
}

static void
SockTimeout(Sock *sockPtr, Ns_Time *nowPtr, int timeout)
{
    sockPtr->timeout = *nowPtr;
    Ns_IncrTime(&sockPtr->timeout, timeout, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * SockAccept --
 *
 *      Accept and initialize a new Sock.
 *
 * Results:
 *      Pointer to Sock or NULL on error.
 *
 * Side effects:
 *      Socket buffer sizes are set as configured.
 *
 *----------------------------------------------------------------------
 */

static Sock *
SockAccept(Driver *drvPtr)
{
    Sock        *sockPtr;
    UploadStats *statsPtr;
    int          slen;

    /*
     * Allocate and/or initialize a connection structure.
     */
    
    sockPtr = firstSockPtr;
    if (sockPtr != NULL) {
        firstSockPtr = sockPtr->nextPtr;
    } else {
        sockPtr = ns_malloc(sizeof(Sock));
        sockPtr->reqPtr = NULL;
    }
    
    /*
     * Accept the new connection.
     */
    
    slen = sizeof(struct sockaddr_in);

    sockPtr->drvPtr  = drvPtr;
    sockPtr->tfd     = 0;
    sockPtr->taddr   = 0;
    sockPtr->keep    = 0;
    sockPtr->arg     = NULL;

    statsPtr = &sockPtr->upload;

    statsPtr->url    = NULL;
    statsPtr->size   = 0;
    statsPtr->length = 0;
    
    if (drvPtr->opts & NS_DRIVER_UDP) {
        sockPtr->sock = drvPtr->sock;
    } else {
        sockPtr->sock = Ns_SockAccept(drvPtr->sock,
                                      (struct sockaddr *) &sockPtr->sa, &slen);
    }
    if (sockPtr->sock == INVALID_SOCKET) {

        /* 
         * Accept failed - return the Sock to the free list.
         */
        
        sockPtr->nextPtr = firstSockPtr;
        firstSockPtr = sockPtr;
        return NULL;
    }

    /*
     * Even though the socket should have inherited
     * non-blocking from the accept socket, set again
     * just to be sure.
     */

    Ns_SockSetNonBlocking(sockPtr->sock);

    /*
     * Set the send/recv socket bufsizes if required.
     */

    if (drvPtr->sndbuf > 0) {
        setsockopt(sockPtr->sock, SOL_SOCKET, SO_SNDBUF,
                   (char *) &drvPtr->sndbuf, sizeof(drvPtr->sndbuf));
    }
    if (drvPtr->rcvbuf > 0) {
        setsockopt(sockPtr->sock, SOL_SOCKET, SO_RCVBUF,
                   (char *) &drvPtr->rcvbuf, sizeof(drvPtr->rcvbuf));
    }

    drvPtr->queuesize++;

    return sockPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * SockRelease --
 *
 *      Close a socket and release the connection structure for
 *      re-use.
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
SockRelease(Sock *sockPtr, int reason, int err)
{
    char         *errMsg = NULL;
    struct iovec  iov;

    switch (reason) {
    case SOCK_CLOSE:
    case SOCK_CLOSETIMEOUT:
        /* This is normal, never log. */
        break;

    case SOCK_READTIMEOUT:
    case SOCK_WRITETIMEOUT:
        /*
         * For this case, whether this is acceptable or not
         * depends upon whether this sock was a keep-alive
         * that we were allowing to 'linger'.
         */
        if (!sockPtr->keep
            && (sockPtr->drvPtr->loggingFlags & LOGGING_READTIMEOUT) ) {
            errMsg = "Timeout during read";
        }
        break;

    case SOCK_SERVERREJECT:
        if (sockPtr->drvPtr->loggingFlags & LOGGING_SERVERREJECT) {
            errMsg = "No Server found for request";
        }
        break;

    case SOCK_READERROR:
        if (sockPtr->drvPtr->loggingFlags & LOGGING_SOCKERROR) {
            errMsg = "Unable to read request";
        }
        break;

    case SOCK_WRITEERROR:
        if (sockPtr->drvPtr->loggingFlags & LOGGING_SOCKERROR) {
            errMsg = "Unable to write request";
        }
        break;

    case SOCK_SHUTERROR:
        if (sockPtr->drvPtr->loggingFlags & LOGGING_SOCKSHUTERROR) {
            errMsg = "Unable to shutdown socket";
        }
        break;

    case SOCK_REQUESTURITOOLONG:
        errMsg = "Request-URI Too Long";
        iov.iov_base = "HTTP/1.0 414 Request-URI Too Long\r\n\r\n";
        iov.iov_len  = strlen(iov.iov_base);
        (*sockPtr->drvPtr->proc)(DriverSend, (Ns_Sock*)sockPtr, &iov, 1);
        break;

    case SOCK_BADREQUEST:
        errMsg = "Bad Request";
        iov.iov_base = "HTTP/1.0 400 Bad Request\r\n\r\n";
        iov.iov_len  = strlen(iov.iov_base);
        (*sockPtr->drvPtr->proc)(DriverSend, (Ns_Sock*)sockPtr, &iov, 1);
        break;

    case SOCK_ENTITYTOOLARGE:
        errMsg = "Request Entity Too Large";
        iov.iov_base = "HTTP/1.0 413 Bad Request\r\n\r\n";
        iov.iov_len  = strlen(iov.iov_base);
        (*sockPtr->drvPtr->proc)(DriverSend, (Ns_Sock*)sockPtr, &iov, 1);
        break;
    }
    if (errMsg != NULL) {
        Ns_Log( Error, "Releasing Socket; %s %s(%d/%d), FD = %d, Peer = %s:%d",
                errMsg, (err ? strerror(err) : ""), reason, err, sockPtr->sock,
                ns_inet_ntoa(sockPtr->sa.sin_addr),ntohs(sockPtr->sa.sin_port));
    }

    SockClose(sockPtr, 0);
    
    sockPtr->drvPtr->queuesize--;
    if (sockPtr->sock != INVALID_SOCKET) {
        if (!(sockPtr->drvPtr->opts & NS_DRIVER_UDP)) {
            ns_sockclose(sockPtr->sock);
        }
        sockPtr->sock = INVALID_SOCKET;
    }
    if (sockPtr->reqPtr != NULL) {
        NsFreeRequest(sockPtr->reqPtr);
        sockPtr->reqPtr = NULL;
    }

    Ns_MutexLock(&drvLock);
    sockPtr->nextPtr = firstSockPtr;
    firstSockPtr     = sockPtr;
    Ns_MutexUnlock(&drvLock);
}


/*
 *----------------------------------------------------------------------
 *
 * SockTrigger --
 *
 *      Wakeup DriversThread from blocking poll().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      DriversThread will wakeup.
 *
 *----------------------------------------------------------------------
 */

static void
SockTrigger(SOCKET fd)
{
    if (send(fd, "", 1, 0) != 1) {
        char * errstr = ns_sockstrerror(ns_sockerrno);
        Ns_Fatal("driver: trigger send() failed: %s", errstr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SockClose --
 *
 *      Closes connection socket, does all cleanups
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static void
SockClose(Sock *sockPtr, int keep)
{
    Ns_Sock *sock = (Ns_Sock *) sockPtr;
    Driver  *drvPtr = sockPtr->drvPtr;
    UploadStats *statsPtr = &sockPtr->upload;

    if (keep && (*drvPtr->proc)(DriverKeep, sock, NULL, 0) != 0) {
        keep = 0;
    }
    if (keep == 0) {
        (void) (*drvPtr->proc)(DriverClose, sock, NULL, 0);
    }

#ifndef _WIN32
    
    /*
     * Close and unmmap temp file used for large content
     */
    
    if (sockPtr->tfd > 0) {
        close(sockPtr->tfd);
    }
    sockPtr->tfd = 0;
    if (sockPtr->taddr != NULL) {
        munmap(sockPtr->taddr, (size_t)sockPtr->tsize);
    }
    sockPtr->taddr = 0;
    sockPtr->keep  = keep;
#endif

    /*
     * Cleanup upload statistics hash table
     */

    if (statsPtr->url != NULL) {
        Tcl_HashEntry *hPtr;
        DrvSpooler *spoolPtr = &drvPtr->spooler;

        Ns_MutexLock(&spoolPtr->lock);
        hPtr = Tcl_FindHashEntry(&spoolPtr->table, statsPtr->url);
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&spoolPtr->lock);
        
        Ns_Log(Debug, "upload stats deleted: %s, %lu %lu",
               statsPtr->url, statsPtr->length, statsPtr->size);
        
        ns_free(statsPtr->url);
        statsPtr->url = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * SockRead --
 *
 *      Read content from the given Sock, processing the input as
 *      necessary.  This is the core callback routine designed to
 *      either be called repeatedly within the DriverThread during
 *      an async read-ahead or in a blocking loop in NsGetRequest
 *      at the start of connection processing.
 *
 * Results:
 *      SOCK_READY: Request is ready for processing.
 *      SOCK_MORE:  More input is required.
 *      SOCK_ERROR: Client drop or timeout.
 *
 * Side effects:
 *      The Request structure will be built up for use by the
 *      connection thread.  Also, before returning SOCK_READY,
 *      the next byte to read mark and bytes available are set
 *      to the beginning of the content, just beyond the headers.
 *
 *      Contents may be spooled into temp file and mmap-ed
 *
 *----------------------------------------------------------------------
 */

static int
SockRead(Sock *sockPtr, int spooler)
{
    Ns_Sock      *sock   = (Ns_Sock *) sockPtr;
    Driver       *drvPtr = sockPtr->drvPtr;
    DrvSpooler   *spPtr  = &drvPtr->spooler;
    Request      *reqPtr = NULL;
    Tcl_DString  *bufPtr = NULL;

    struct iovec  buf;
    char         tbuf[4096];
    int          len, nread, n;

    /*
     * Initialize Request structure
     */

    if (Ns_DriverSockRequest(sock, 0) != NS_OK) {
        return SOCK_ERROR;
    }

    /*
     * On the first read, attempt to read-ahead bufsize bytes.
     * Otherwise, read only the number of bytes left in the
     * content.
     */
    
    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;
    if (reqPtr->length == 0) {
        nread = drvPtr->bufsize;
    } else {
        nread = reqPtr->length - reqPtr->avail;
    }

    /*
     * Grow the buffer to include space for the next bytes.
     */
    
    len = bufPtr->length;
    n = len + nread;
    if (n > drvPtr->maxinput) {
        n = drvPtr->maxinput;
        nread = n - len;
        if (nread == 0) {
            return SOCK_ERROR;
        }
    }

    /*
     * Use temp file for content larger than readahead bytes.
     */

#ifndef _WIN32
    if (reqPtr->coff > 0 &&
        reqPtr->length > drvPtr->readahead && sockPtr->tfd <= 0) {

        /*
         * In driver mode send this Sock to the spooler thread if
         * it is running
         */
        
        if (spooler == 0 && spPtr->threads > 0) {
            return SOCK_SPOOL;
        }
        
        /*
         * In spooler mode dump data into temp file
         */
        
        sockPtr->tfd = Ns_GetTemp();
        if (sockPtr->tfd < 0) {
            return SOCK_ERROR;
        }
        n = bufPtr->length - reqPtr->coff;
        if (write(sockPtr->tfd, bufPtr->string + reqPtr->coff, n) != n) {
            return SOCK_WRITEERROR;
        }
        Tcl_DStringSetLength(bufPtr, 0);
    }
#endif
    if (sockPtr->tfd > 0) {
        buf.iov_base = tbuf;
        buf.iov_len = MIN(nread, sizeof(tbuf));
    } else {
        Tcl_DStringSetLength(bufPtr, len + nread);
        buf.iov_base = bufPtr->string + reqPtr->woff;
        buf.iov_len = nread;
    }

    n = (*sockPtr->drvPtr->proc)(DriverRecv, sock, &buf, 1);

    if (n <= 0) {
        return SOCK_READERROR;
    }
    if (sockPtr->tfd > 0) {
        if (write(sockPtr->tfd, tbuf, n) != n) {
     	    return SOCK_WRITEERROR;
        }
    } else {
        Tcl_DStringSetLength(bufPtr, len + n);
    }

    reqPtr->woff  += n;
    reqPtr->avail += n;

    /*
     * Check the hard limit for max uploaded content size
     */

    if (reqPtr->avail > sockPtr->drvPtr->maxinput) {
        return SOCK_ENTITYTOOLARGE;
    }

    return SockParse(sockPtr, spooler);
}

/*----------------------------------------------------------------------
 *
 * SockParse --
 *
 *      Construct the given conn by parsing input buffer until end of
 *      headers.  Return NS_SOCK_READY when finnished parsing.
 *
 * Results:
 *      NS_SOCK_READY:  Conn is ready for processing.
 *      NS_SOCK_MORE:   More input is required.
 *      NS_SOCK_ERROR:  Malformed request.
 *
 * Side effects:
 *      An Ns_Request and/or Ns_Set may be allocated.
 *      Ns_Conn buffer management offsets updated.
 *
 *----------------------------------------------------------------------
 */

static int
SockParse(Sock *sockPtr, int spooler)
{
    Request      *reqPtr;
    Tcl_DString  *bufPtr;
    char         *s, *e, save;
    int           cnt;

    Driver       *drvPtr = sockPtr->drvPtr;
    DrvSpooler   *spPtr  = &drvPtr->spooler;

    UploadStats  *statsPtr = &sockPtr->upload;
    
    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;

    /*
     * Scan lines until start of content.
     */
    
    while (reqPtr->coff == 0) {
     
        /*
         * Find the next line.
         */
        
        s = bufPtr->string + reqPtr->roff;
        e = strchr(s, '\n');
        if (e == NULL) {
            
            /*
             * Input not yet null terminated - request more.
             */
            
            return SOCK_MORE;
        }

        /*
         * Check for max single line overflow.
         */

        if ((e - s) > drvPtr->maxline) {
            if (reqPtr->request == NULL) {
                return SOCK_REQUESTURITOOLONG;
            }
            return SOCK_BADREQUEST;
        }

        /*
         * Update next read pointer to end of this line.
         */
        
        cnt = e - s + 1;
        reqPtr->roff  += cnt;
        reqPtr->avail -= cnt;
        if (e > s && e[-1] == '\r') {
            --e;
        }
        
        /*
         * Check for end of headers.
         */
        
        if (e == s) {
            
            /*
             * Look for a blank line on its own prior to any "real" 
             * data. We eat up to 2 of these before closing the
             * connection.
             */
            
            if (bufPtr->length == 0) {
                if (++reqPtr->leadblanks > 2) {
                    return SOCK_ERROR;
                }
                reqPtr->woff = reqPtr->roff = 0;
                Tcl_DStringSetLength(bufPtr, 0);
                return SOCK_MORE;
            }
            reqPtr->coff = reqPtr->roff;
            s = Ns_SetIGet(reqPtr->headers, "content-length");
            if (s != NULL) {
                int length;

                /*
                 * Honour meaningfull remote 
                 * content-length hints only.
                 */

                length = atoi(s);
                if (length >= 0) {
                    reqPtr->length = length;
                    if (reqPtr->length > drvPtr->maxinput) {
                        return SOCK_ENTITYTOOLARGE;
                    }
                }

            }
        } else {
            save = *e;
            *e = '\0';
            if (reqPtr->request == NULL) {
                reqPtr->request = Ns_ParseRequest(s);
                if (reqPtr->request == NULL) {
                    
                    /*
                     * Invalid request.
                     */
                    
                    return SOCK_BADREQUEST;
                }

            } else if (Ns_ParseHeader(reqPtr->headers, s, Preserve) != NS_OK) {
                
                /*
                 * Invalid header.
                 */
                
                return SOCK_BADREQUEST;
            }

            /*
             * Check for max number of headers
             */

            if (Ns_SetSize(reqPtr->headers) > drvPtr->maxheaders) {
                return SOCK_BADREQUEST;
            }

            *e = save;
            if (reqPtr->request->version <= 0.0) {
             
                /*
                 * Pre-HTTP/1.0 request.
                 */
                
                reqPtr->coff = reqPtr->roff;
            }
        }
    }
    
    /*
     * Check if all content has arrived.
     */
    
    if (reqPtr->coff > 0 && reqPtr->length <= reqPtr->avail) {
        if (sockPtr->tfd > 0) {
#ifndef _WIN32
            int prot = PROT_READ | PROT_WRITE;
            sockPtr->tsize = reqPtr->length + 1;
            sockPtr->taddr = mmap(0, sockPtr->tsize, prot, MAP_PRIVATE,
                                  sockPtr->tfd, 0);
            if (sockPtr->taddr == MAP_FAILED) {
                sockPtr->taddr = NULL;
                return SOCK_ERROR;
            }
            reqPtr->content = sockPtr->taddr;
            Ns_Log(Debug, "spooling content to file: readahead=%d, filesize=%i",
                   drvPtr->readahead, (int)sockPtr->tsize);
#endif
        } else {
            reqPtr->content = bufPtr->string + reqPtr->coff;
        }
        reqPtr->next = reqPtr->content;
        reqPtr->avail = reqPtr->length;
        
        /*
         * Ensure that there are no 'bonus' crlf chars left visible
         * in the buffer beyond the specified content-length.
         * This happens from some browsers on POST requests.
         */
        
        if (reqPtr->length > 0) {
            reqPtr->content[reqPtr->length] = '\0';

        }
        return (reqPtr->request ? SOCK_READY : SOCK_ERROR);
    }

    /*
     * Wait for more input.
     */

    if (spooler == 0) {
        return SOCK_MORE;
    }

    /*
     * Create/update upload stats hash entry
     */

    if (statsPtr->url == NULL) {
        if (reqPtr->length > 0 && reqPtr->avail > spPtr->uploadsize) {
            Ns_Request *req = reqPtr->request;
            Tcl_HashEntry *hPtr;
            size_t len = strlen(req->url) + 1;

            if (req->query == NULL) {
                statsPtr->url = ns_calloc(1, len);
                strcpy(statsPtr->url, req->url);
            } else {
                len += strlen(req->query) + 1; /* for '?' delimiter */
                statsPtr->url = ns_calloc(1, len);
                sprintf(statsPtr->url, "%s?%s", req->url, req->query);
            }

            statsPtr->length = reqPtr->length;
            statsPtr->size = reqPtr->avail;

            Ns_MutexLock(&spPtr->lock);
            hPtr = Tcl_CreateHashEntry(&spPtr->table, statsPtr->url, &cnt);
            Tcl_SetHashValue(hPtr, sockPtr);
            Ns_MutexUnlock(&spPtr->lock);
            Ns_Log(Debug, "upload stats created for %s", statsPtr->url);
        }
    } else {
        Ns_MutexLock(&spPtr->lock);
        statsPtr->length = reqPtr->length;
        statsPtr->size = reqPtr->avail;
        Ns_MutexUnlock(&spPtr->lock);
    }
    
    return SOCK_MORE;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DriverSockRequest --
 *
 *      Allocates new request struct for the given socket, if data is
 *      specified, it becomes parsed request struct, i.e. should be 
 *      in the form: METHOD URL PROTO
 *
 * Results:
 *      NS_ERROR if request cannot be parsed.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

int
Ns_DriverSockRequest(Ns_Sock *sock, char *reqline)
{
    Request *reqPtr;
    Sock     *sockPtr = (Sock*)sock;

    reqPtr = sockPtr->reqPtr;
    if (reqPtr == NULL) {
        Ns_MutexLock(&reqLock);
        reqPtr = firstReqPtr;
        if (reqPtr != NULL) {
            firstReqPtr = reqPtr->nextPtr;
        }
        Ns_MutexUnlock(&reqLock);
        if (reqPtr == NULL) {
            reqPtr = ns_malloc(sizeof(Request));
            Tcl_DStringInit(&reqPtr->buffer);
            reqPtr->headers    = Ns_SetCreate(NULL);
            reqPtr->request    = NULL;
            reqPtr->next       = NULL;
            reqPtr->content    = NULL;
            reqPtr->length     = 0;
            reqPtr->avail      = 0;
            reqPtr->coff       = 0;
            reqPtr->woff       = 0;
            reqPtr->roff       = 0;
            reqPtr->leadblanks = 0;
        }
        reqPtr->port = ntohs(sockPtr->sa.sin_port);
        strcpy(reqPtr->peer, ns_inet_ntoa(sockPtr->sa.sin_addr));
        sockPtr->reqPtr = reqPtr;
    }

    if (reqline) {
        reqPtr->request = Ns_ParseRequest(reqline);
        if (reqPtr->request == NULL) {
            NsFreeRequest(reqPtr);
            sockPtr->reqPtr = NULL;
            return NS_ERROR;
        }
    }

    return NS_OK;
}

int
NsTclUploadStatsObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                       Tcl_Obj *CONST objv[])
{
    char        buf[64] = "";
    Driver     *drvPtr;
    DrvSpooler *spPtr = &drvPtr->spooler;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "url");
        return TCL_ERROR;
    }

    drvPtr = firstDrvPtr;

    while (drvPtr != NULL) {
        Tcl_HashEntry *hPtr;
        Ns_MutexLock(&spPtr->lock);
        hPtr = Tcl_FindHashEntry(&spPtr->table, Tcl_GetString(objv[1]));
        if (hPtr != NULL) {
            Sock *sockPtr = (Sock *)Tcl_GetHashValue(hPtr);
            UploadStats *statsPtr = &sockPtr->upload;
            sprintf(buf, "%lu %lu", statsPtr->length, statsPtr->size);
            break;
        }
        Ns_MutexUnlock(&spPtr->lock);
        drvPtr = drvPtr->nextPtr;
    }

    Tcl_AppendResult(interp, buf, NULL);

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * SpoolerThread --
 *
 *      Spooling socket driver thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are accepted on the configured listen sockets,
 *      placed on the run queue to be serviced, and gracefully
 *      closed when done.  Async sockets have the entire request read
 *      here before queuing as well.
 *
 *----------------------------------------------------------------------
 */

static void
SpoolerThread(void *arg)
{
    SpoolerQueue  *queuePtr = (SpoolerQueue*)arg;
    char           c;
    int            n, stopping, pollto;
    Sock          *sockPtr, *nextPtr, *waitPtr, *readPtr;
    Ns_Time        timeout, now, diff;
    Driver        *drvPtr;
    unsigned int   nfds, maxfds;
    struct pollfd *pfds;
    
    Ns_ThreadSetName("-spooler%d-", queuePtr->id);

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "spooler%d: accepting connections", queuePtr->id);
    waitPtr = readPtr = NULL;
    Ns_GetTime(&now);
    stopping = 0;
    maxfds = 100;
    pfds = ns_malloc(maxfds * sizeof(struct pollfd));
    pfds[0].fd = queuePtr->pipe[0];
    pfds[0].events = POLLIN;

    while (!stopping) {

        /*
         * If there are any read sockets, set the bits
         * and determine the minimum relative timeout.
         */
        
        nfds = 1;
        if (readPtr == NULL) {
            pollto = 30 * 1000;
        } else {
            timeout.sec = INT_MAX;
            timeout.usec = LONG_MAX;
            sockPtr = readPtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, POLLIN, &pfds, &nfds, &maxfds, &timeout);
                sockPtr = sockPtr->nextPtr;
            }
            if (Ns_DiffTime(&timeout, &now, &diff) > 0)  {
                pollto = diff.sec * 1000 + diff.usec / 1000;
            } else {
                pollto = 0;
            }
        }

        /*
         * Select and drain the trigger pipe if necessary.
         */

        pfds[0].revents = 0;

        do {
            n = poll(pfds, nfds, pollto);
        } while (n < 0  && errno == EINTR);
        if (n < 0) {
            Ns_Fatal("driver: poll() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }
        if ((pfds[0].revents & POLLIN) 
            && recv(queuePtr->pipe[0], &c, 1, 0) != 1) {
            Ns_Fatal("spooler: trigger recv() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }

        /*
         * Attempt read-ahead of any new connections.
         */

        Ns_GetTime(&now);
        sockPtr = readPtr;
        readPtr = NULL;

        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            drvPtr  = sockPtr->drvPtr;
            if (!(pfds[sockPtr->pidx].revents & POLLIN)) {
                if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                } else {
                    Push(sockPtr, readPtr);
                }
            } else {
                n = SockRead(sockPtr, 1);
                switch (n) {
                case SOCK_MORE:
                    SockTimeout(sockPtr, &now, drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;

                case SOCK_READY:
                    if (!SetServer(sockPtr)) {
                        SockRelease(sockPtr, SOCK_SERVERREJECT, 0);
                    } else {
                        Push(sockPtr, waitPtr);
                    }
                    break;

                default:
                    SockRelease(sockPtr, n, errno);
                    break;
                }
            }
            sockPtr = nextPtr;
        }
        
        /*
         * Attempt to queue any pending connection
         * after reversing the list to ensure oldest
         * connections are tried first.
         */
        
        if (waitPtr != NULL) {
            sockPtr = NULL;
            while ((nextPtr = waitPtr) != NULL) {
                waitPtr = nextPtr->nextPtr;
                Push(nextPtr, sockPtr);
            }
            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                if (waitPtr != NULL || !NsQueueConn(sockPtr, &now)) {
                    Push(sockPtr, waitPtr);
                }
                sockPtr = nextPtr;
            }
        }
        
        /*
         * Add more connections from the spooler queue
         */
        
        Ns_MutexLock(&queuePtr->lock);
        if (waitPtr == NULL) {
            sockPtr = (Sock*)queuePtr->sockPtr;
            queuePtr->sockPtr = NULL;
            while (sockPtr != NULL) {
                nextPtr = sockPtr->nextPtr;
                drvPtr  = sockPtr->drvPtr;
                SockTimeout(sockPtr, &now, drvPtr->recvwait);
                Push(sockPtr, readPtr);
                sockPtr = nextPtr;
            }
        }

        /*
         * Check for shutdown 
         */
        
        stopping = queuePtr->shutdown;
        Ns_MutexUnlock(&queuePtr->lock);
    }

    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = 1;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

static int
SockSpoolerQueue(Driver *drvPtr, Sock *sockPtr)
{
    int trigger = 0;
    SpoolerQueue *queuePtr;

    /*
     * Get the next spooler thread from the list, all spooler requests are
     * rotated between all spooler threads
     */

    Ns_MutexLock(&drvPtr->spooler.lock);
    if (drvPtr->spooler.curPtr == NULL) {
        drvPtr->spooler.curPtr = drvPtr->spooler.firstPtr;
    }
    queuePtr = drvPtr->spooler.curPtr;
    drvPtr->spooler.curPtr = drvPtr->spooler.curPtr->nextPtr;
    Ns_MutexUnlock(&drvPtr->spooler.lock);

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = 1;
    }
    Push(sockPtr, queuePtr->sockPtr);
    Ns_MutexUnlock(&queuePtr->lock);
    Ns_Log(Notice, "Spooler: %d: started fd=%d: %u bytes",
           queuePtr->id, sockPtr->sock, sockPtr->reqPtr->length);

    /*
     * Wake up spooler thread
     */
    
    if (trigger) {
        SockTrigger(queuePtr->pipe[1]);
    }

    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterThread --
 *
 *      Thread that writes files to clients
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Connections are accepted and their SockPtr is set to NULL
 *      so closing actual connection does not close the socket
 *
 *----------------------------------------------------------------------
 */

static void
WriterThread(void *arg)
{    

    unsigned char   c, *bufPtr;
    int             n, err, stopping, pollto, toread, maxsize, status;

    SpoolerQueue   *queuePtr = (SpoolerQueue*)arg;
    Ns_Time         now;

    Sock           *sockPtr;
    Driver         *drvPtr;
    DrvWriter      *wrPtr;
    WriterSock     *curPtr, *nextPtr, *writePtr;

    struct pollfd   pfds[1];
    struct iovec    vbuf;

    Ns_ThreadSetName("-writer%d-", queuePtr->id);

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "writer%d: accepting connections", queuePtr->id);
    Ns_GetTime(&now);
    stopping = 0;
    writePtr = NULL;
    pfds[0].fd = queuePtr->pipe[0];
    pfds[0].events = POLLIN;

    while (!stopping) {

        /*
         * Select and drain the trigger pipe if necessary.
         */

        if (writePtr == NULL) {
            pfds[0].revents = 0;
            pollto = 30 * 1000;  /* Wake up every 30 seconds just in case */
            do {
                n = poll(pfds, 1, pollto);
            } while (n < 0  && errno == EINTR);
            if (n < 0) {
                Ns_Fatal("driver: poll() failed: %s", 
                         ns_sockstrerror(ns_sockerrno));
            }
            if ((pfds[0].revents & POLLIN) 
                && recv(queuePtr->pipe[0], &c, 1, 0) != 1) {
                Ns_Fatal("driver: trigger recv() failed: %s",
                         ns_sockstrerror(ns_sockerrno));
            }
        }

        /*
         * Attempt write to all available sockets
         */

        Ns_GetTime(&now);
        curPtr = writePtr;
        writePtr = NULL;

        while (curPtr != NULL) {

            nextPtr = curPtr->nextPtr;

            sockPtr = curPtr->sockPtr;
            drvPtr  = sockPtr->drvPtr;
            wrPtr   = &drvPtr->writer;

            /*
             * Read block from the file and send it to the socket
             */

            n = err = status = NS_OK;
            if (curPtr->size > 0) {
                if (curPtr->fd > -1) {
                    maxsize = wrPtr->bufsize;

                    /*
                     *  Case when bufsize > 0 means that we have leftover
                     *  from previous send, fill up the rest of the buffer
                     *  and retransmit it with new portion from the file
                     */

                    if (curPtr->bufsize > 0) {
                        bufPtr = curPtr->buf + (sizeof(curPtr->buf) 
                                                - curPtr->bufsize);
                        memmove(curPtr->buf, bufPtr, curPtr->bufsize);
                        bufPtr = curPtr->buf + curPtr->bufsize;
                        maxsize -= curPtr->bufsize;
                    } else {
                        bufPtr = curPtr->buf;
                    }
                    toread = curPtr->nread;
                    if (toread > maxsize) {
                        toread = maxsize;
                    }

                    /*
                     * Read whatever we have left in the file
                     */

                    if (toread > 0) {
                         n = read(curPtr->fd, bufPtr, (size_t)toread);
                         if (n <= 0) {
                             status = NS_ERROR;
                         } else {
                             curPtr->nread -= n;
                             curPtr->bufsize += n;
                         }
                    }
                }

                /*
                 * If actual amount sent is less than requested,
                 * keep that data for the next iteration
                 */

                if (status == NS_OK) {
                    vbuf.iov_base = (void *) curPtr->buf;
                    vbuf.iov_len = curPtr->bufsize;
                    n = (*drvPtr->proc)
                        (DriverSend, (Ns_Sock*)curPtr->sockPtr, &vbuf, 1);
                    if (n > 0) {
                        curPtr->size -= n;
                        curPtr->nsent += n;
                        curPtr->bufsize -= n;
                        if (curPtr->data) {
                            curPtr->buf += n;
                        }
                    } else {
                        err = errno;
                        status = NS_ERROR;
                    }
                }
            }
            if (status != NS_OK) {
                SockWriterRelease(curPtr, SOCK_WRITEERROR, err);
            } else {
                if (curPtr->size > 0) {
                    Push(curPtr, writePtr);
                } else {
                    SockWriterRelease(curPtr, 0, 0);
                }
            }
            curPtr = nextPtr;
        }

        /*
         * Add more sockets to the writer queue
         */

        Ns_MutexLock(&queuePtr->lock);
        curPtr = queuePtr->sockPtr;
        queuePtr->sockPtr = NULL;
        while (curPtr != NULL) {
            nextPtr = curPtr->nextPtr;
            sockPtr = curPtr->sockPtr;
            drvPtr  = sockPtr->drvPtr;
            SockTimeout(sockPtr, &now, drvPtr->sendwait);
            Push(curPtr, writePtr);
            curPtr = nextPtr;
        }
        queuePtr->curPtr = writePtr;

        /*
         * Check for shutdown
         */
        
        stopping = queuePtr->shutdown;
        Ns_MutexUnlock(&queuePtr->lock);
    }

    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = 1;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

static void
SockWriterRelease(WriterSock *sockPtr, int reason, int err)
{
    Ns_Log(Notice, "Writer: closed fd=%d, error=%d/%d, sent=%u", 
           sockPtr->fd, reason, err, sockPtr->nsent);
    SockRelease(sockPtr->sockPtr, reason, err);
    if (sockPtr->fd > -1) {
        close(sockPtr->fd);
        ns_free(sockPtr->buf);
    }
    ns_free(sockPtr->data);
    ns_free(sockPtr);
}

int
NsWriterQueue(Ns_Conn *conn, int nsend, Tcl_Channel chan, FILE *fp, int fd, 
              const char *data)
{
    Conn          *connPtr = (Conn*)conn;
    WriterSock    *sockPtr;
    SpoolerQueue  *queuePtr;
    Driver        *drvPtr;
    DrvWriter     *wrPtr;
    int            trigger = 0;

    if (conn == NULL) {
        return NS_ERROR;
    }
    
    drvPtr = connPtr->sockPtr->drvPtr;
    wrPtr  = &drvPtr->writer;

    if (wrPtr->threads == 0 || nsend < wrPtr->maxsize
        || (conn->flags & NS_CONN_WRITE_CHUNKED)) {
        return NS_ERROR;
    }

    /*
     * Flush the headers
     */

    Ns_WriteConn(conn, NULL, 0);

    sockPtr = (WriterSock*)ns_calloc(1, sizeof(WriterSock));
    sockPtr->sockPtr = connPtr->sockPtr;
    sockPtr->flags = connPtr->flags;
    sockPtr->fd = -1;

    if (chan != NULL) {
        if (Tcl_GetChannelHandle(chan, TCL_READABLE, 
                                 (ClientData)&sockPtr->fd) != TCL_OK) {
            ns_free(sockPtr);
            return NS_ERROR;
        }
    } else if (fp != NULL) {
        sockPtr->fd = fileno(fp);
    } else if (fd != -1) {
        sockPtr->fd = fd;
    } else if (data != NULL ) {
        sockPtr->data = ns_malloc(nsend + 1);
        memcpy(sockPtr->data, data, nsend);
    } else {
        ns_free(sockPtr);
        return NS_ERROR;
    }
    if (sockPtr->fd > -1) {
        sockPtr->fd  = ns_sockdup(sockPtr->fd);
        sockPtr->buf = ns_malloc(wrPtr->bufsize);
    }
    if (sockPtr->data) {
        sockPtr->buf = (unsigned char*)sockPtr->data;
        sockPtr->bufsize = nsend;
    }
    sockPtr->size = nsend;
    sockPtr->nread = nsend;
    connPtr->sockPtr = NULL;

    /* To keep nslog happy about content size returned */
    connPtr->nContentSent = nsend;
    Ns_SockSetBlocking(sockPtr->sockPtr->sock);
    
    /*
     * Get the next writer thread from the list, all writer requests are
     * rotated between all writer threads
     */

    Ns_MutexLock(&wrPtr->lock);
    if (wrPtr->curPtr == NULL) {
        wrPtr->curPtr = wrPtr->firstPtr;
    }
    queuePtr = wrPtr->curPtr;
    wrPtr->curPtr = wrPtr->curPtr->nextPtr;
    Ns_MutexUnlock(&wrPtr->lock);

    /*
     * Now add new writer socket to the writer thread's queue
     */

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = 1;
    }
    Push(sockPtr, queuePtr->sockPtr);
    Ns_MutexUnlock(&queuePtr->lock);

    /*
     * Wake up writer thread
     */

    if (trigger) {
        SockTrigger(queuePtr->pipe[1]);
    }

    Ns_Log(Notice, "Writer: %d: started fd=%d: %u bytes: %s", 
           queuePtr->id, sockPtr->fd, nsend, connPtr->reqPtr->request->url);
    
    return NS_OK;
}

int
NsTclWriterObjCmd(ClientData arg, Tcl_Interp *interp, int objc, 
                  Tcl_Obj *CONST objv[])
{
    char         *data;
    int           fd, opt, size;
    Tcl_DString   ds;
    Driver       *drvPtr;
    DrvWriter    *wrPtr;
    WriterSock   *sockPtr;
    SpoolerQueue *queuePtr;

    static CONST char *opts[] = {
        "submit", "submitfile", "list", NULL
    };

    enum {
        cmdSubmitIdx, cmdSubmitFileIdx, cmdListIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, 
                            "option", 0, &opt) != TCL_OK) {
        return TCL_ERROR;
    }
    
    switch (opt) {
    case cmdSubmitIdx:
        if (objc < 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "data");
            return TCL_ERROR;
        }
        data = (char*)Tcl_GetByteArrayFromObj(objv[2], &size);
        if (data) {
            int nwq = NsWriterQueue(Ns_GetConn(), size, NULL, NULL, -1, data);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(nwq));
        }
        break;
        
    case cmdSubmitFileIdx:
        if (objc < 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "filename");
            return TCL_ERROR;
        }
        /* FIXME: TclVFS */
        fd = open(Tcl_GetString(objv[2]), O_RDONLY);
        if (fd == -1) {
            Tcl_AppendResult(interp, "error opening file: ", 
                             Tcl_GetString(objv[2]), ": ", strerror(errno), 0);
            return TCL_ERROR;
        } else {
            int nwq = NsWriterQueue(Ns_GetConn(), 0, NULL, NULL, fd, NULL);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(nwq));
            close(fd);
        }
        break;
        
    case cmdListIdx:
        Tcl_DStringInit(&ds);
        drvPtr = firstDrvPtr;
        while (drvPtr != NULL) {
            wrPtr = &drvPtr->writer;
            queuePtr = wrPtr->firstPtr;
            while (queuePtr != NULL) {
                Ns_MutexLock(&queuePtr->lock);
                sockPtr = queuePtr->curPtr;
                while (sockPtr != NULL) {
                    Ns_DStringPrintf(&ds, "%s %s %d %u %u ", drvPtr->name,
                                     ns_inet_ntoa(sockPtr->sockPtr->sa.sin_addr),
                                     sockPtr->fd, sockPtr->size, sockPtr->nsent);
                    sockPtr = sockPtr->nextPtr;
                }
                Ns_MutexUnlock(&queuePtr->lock);
                queuePtr = queuePtr->nextPtr;
            }
            drvPtr = drvPtr->nextPtr;
        }
        Tcl_AppendResult(interp, ds.string, 0);
        Tcl_DStringFree(&ds);
        break;
    }

    return NS_OK;
}
