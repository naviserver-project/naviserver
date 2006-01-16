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
 * Defines for SockRead return code.
 */

#define SOCK_READY  0
#define SOCK_MORE   1
#define SOCK_SPOOL  2
#define SOCK_ERROR  (-1)

/*
 * Defines for SockRelease reason codes.
 *
 */

typedef enum {
    Reason_Close,
    Reason_CloseTimeout,
    Reason_ReadTimeout,
    Reason_WriteTimeout,
    Reason_ServerReject,
    Reason_SockError,
    Reason_SockShutError
} ReleaseReasons;

/*
 * LoggingFlag mask values
 */
#define LOGGING_READTIMEOUT   0x01
#define LOGGING_SERVERREJECT  0x02
#define LOGGING_SOCKERROR     0x04
#define LOGGING_SOCKSHUTERROR 0x08


/*
 * The following maintains Host header to server mappings.
 */

typedef struct ServerMap {
    NsServer *servPtr;
    char      location[1];
} ServerMap;

static Tcl_HashTable uploadTable; /* Hash table of uploads. */
static Ns_Mutex uploadLock;       /* Lock around upload table. */

/*
 * The following maintains files to be written to the clients
 */

typedef struct WriterSock {
    struct WriterSock *nextPtr;
    Sock              *sockPtr;
    int                fd;
    int                nsend;
    int                flags;
} WriterSock;

/*
 * Static functions defined in this file.
 */

static Ns_ThreadProc DriverThread;
static Ns_ThreadProc SpoolThread;
static Ns_ThreadProc WriterThread;
static int  SetServer(Sock *sockPtr);
static Sock *SockAccept(Driver *drvPtr);
static void SockRelease(Sock *sockPtr, ReleaseReasons reason);
static void SockTrigger(SOCKET sock);
static void SockPoll(Sock *sockPtr, int type, struct pollfd **pfds, unsigned int *nfds,
                     unsigned int *maxfds, Ns_Time *timeoutPtr);
static void SockTimeout(Sock *sockPtr, Ns_Time *nowPtr, int timeout);
static void SockClose(Sock *sockPtr, int keep);
static int SockRead(Sock *sockPtr, int spooler);
static int SockParse(Sock *sockPtr, int spooler);

static int SockSpoolerPush(Sock *sockPtr);
static Sock *SockSpoolerPop(void);

static void SockWriterRelease(WriterSock *sockPtr, ReleaseReasons reason);

/*
 * Static variables defined in this file.
 */

static int nactive;                 /* Active sockets. */

static Tcl_HashTable hosts;         /* Host header to server table. */
static ServerMap *defMapPtr = NULL; /* Default server when not found in table. */

static Ns_Mutex reqLock;            /* Lock around request free list. */
static Request *firstReqPtr = NULL; /* Free list of request structures. */
static Driver *firstDrvPtr;         /* First in list of all drivers. */

static SOCKET drvPipe[2];          /* Trigger to wakeup DriverThread. */
static Ns_Mutex drvLock;            /* Lock around close list and shutdown flag. */
static Ns_Cond drvCond;             /* Cond for stopped flag. */
static Ns_Thread driverThread;      /* Running DriverThread. */
static int drvStopped = 0;          /* Flag to indicate driver thread stopped. */
static int driverShutdown = 0;      /* Flag to indicate shutdown. */
static Sock *firstSockPtr = NULL;   /* Free list of Sock structures. */
static Sock *firstClosePtr;         /* First conn ready for graceful close. */

static SOCKET spoolerPipe[2];       /* Trigger to wakeup SpoolThread. */
static Ns_Mutex spoolerLock;        /* Lock around spooled list. */
static Ns_Cond spoolerCond;         /* Cond for stopped flag. */
static Ns_Thread spoolerThread;     /* Running SpoolThread. */
static int spoolerStopped = 0;      /* Flag to indicate spooler thread stopped. */
static int spoolerShutdown = 0;     /* Flag to indicate shutdown. */
static int spoolerDisabled = 0;     /* Flag to enable/disable the upload spooler. */
static Sock *spoolerSockPtr = NULL; /* List of spooled Sock structures. */

static SOCKET writerPipe[2];       /* Trigger to wakeup SpoolThread. */
static Ns_Mutex writerLock;        /* Lock around spooled list. */
static Ns_Cond writerCond;         /* Cond for stopped flag. */
static Ns_Thread writerThread;     /* Running SpoolThread. */
static int writerStopped = 0;      /* Flag to indicate writer thread stopped. */
static int writerShutdown = 0;     /* Flag to indicate shutdown. */
static int writerDisabled = 1;     /* Flag to enable/disable the upload writer. */
static WriterSock *writerSockPtr = NULL; /* List of spooled Sock structures. */

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
    Ns_MutexInit(&uploadLock);
    Ns_MutexSetName(&drvLock, "ns:upload");
    Tcl_InitHashTable(&uploadTable, TCL_STRING_KEYS);
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
    drvPtr->server = server;
    drvPtr->name = init->name;
    drvPtr->proc = init->proc;
    drvPtr->arg = init->arg;
    drvPtr->opts = init->opts;
    drvPtr->servPtr = servPtr;

    drvPtr->maxinput = Ns_ConfigIntRange(path, "maxinput", 1024*1024, 1024, INT_MAX);
    drvPtr->maxline = Ns_ConfigIntRange(path, "maxline", 4096, 256, INT_MAX);
    drvPtr->maxheaders = Ns_ConfigIntRange(path, "maxheaders", 128, 8, INT_MAX);
    drvPtr->bufsize = Ns_ConfigIntRange(path, "bufsize", 16384, 1024, INT_MAX);
    drvPtr->readahead = Ns_ConfigIntRange(path, "readahead", drvPtr->bufsize,
                                          drvPtr->bufsize, drvPtr->maxinput);
    drvPtr->uploadsize = Ns_ConfigIntRange(path, "uploadsize", 2048, 1024, INT_MAX);
    drvPtr->writersize = Ns_ConfigIntRange(path, "writer", 1024*1024, 1024*1024*10, INT_MAX);
    drvPtr->sndbuf = Ns_ConfigIntRange(path, "sndbuf", 0, 0, INT_MAX);
    drvPtr->rcvbuf = Ns_ConfigIntRange(path, "rcvbuf", 0, 0, INT_MAX);
    drvPtr->sendwait = Ns_ConfigIntRange(path, "sendwait", 30, 1, INT_MAX);
    drvPtr->recvwait = Ns_ConfigIntRange(path, "recvwait", 30, 1, INT_MAX);
    drvPtr->closewait = Ns_ConfigIntRange(path, "closewait", 2, 0, INT_MAX);
    drvPtr->keepwait = Ns_ConfigIntRange(path, "keepwait", 30, 0, INT_MAX);
    drvPtr->keepallmethods = Ns_ConfigBool(path, "keepallmethods", NS_FALSE);
    drvPtr->backlog = Ns_ConfigIntRange(path, "backlog", 64, 1, INT_MAX);

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
    drvPtr->address = ns_strdup(address);
    drvPtr->port = Ns_ConfigIntRange(path, "port", defport, 0, 65535);
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
     * Map Host headers for drivers not bound to servers.
     */

    if (server == NULL) {
        if (defserver == NULL) {
            Ns_Fatal("%s: virtual servers configured,"
                     " but %s has no defaultserver defined", module, path);
        }
        defMapPtr = NULL;
        path = Ns_ConfigGetPath(NULL, module, "servers", NULL);
        set = Ns_ConfigGetSection(path);
        for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
            server = Ns_SetKey(set, i);
            host = Ns_SetValue(set, i);
            servPtr = NsGetServer(server);
            if (servPtr == NULL) {
                Ns_Log(Error, "%s: no such server: %s", module, server);
            } else {
                hPtr = Tcl_CreateHashEntry(&hosts, host, &n);
                if (!n) {
                    Ns_Log(Error, "%s: duplicate host map: %s", module, host);
                } else {
                    Ns_DStringVarAppend(&ds, drvPtr->protocol, "://", 
                                        host, NULL);
                    mapPtr = ns_malloc(sizeof(ServerMap) + ds.length);
                    mapPtr->servPtr  = servPtr;
                    strcpy(mapPtr->location, ds.string);
                    Ns_DStringTrunc(&ds, 0);
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

    /*
     * Check if upload spooler has been disabled
     */

    if (!Ns_ConfigBool(path, "spooler", NS_TRUE)) {
        spoolerDisabled = 1;
    }

    if (Ns_ConfigBool(path, "writer", NS_FALSE)) {
        writerDisabled = 0;
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

    /*
     * Listen on all drivers.
     */

    drvPtr = firstDrvPtr;
    while (drvPtr != NULL) {
        if (drvPtr->opts & NS_DRIVER_UDP) {
            drvPtr->sock = Ns_SockListenUdp(drvPtr->bindaddr, drvPtr->port);
        } else if (drvPtr->opts & NS_DRIVER_UNIX) {
            drvPtr->sock = Ns_SockListenUnix(drvPtr->bindaddr, drvPtr->backlog);
        } else {
            drvPtr->sock = Ns_SockListenEx(drvPtr->bindaddr, drvPtr->port,
                                           drvPtr->backlog);
        }
        if (drvPtr->sock == INVALID_SOCKET) {
            Ns_Log(Error, "%s: failed to listen on %s:%d: %s",
                   drvPtr->name, drvPtr->address, drvPtr->port,
                   ns_sockstrerror(ns_sockerrno));
        } else {
            Ns_SockSetNonBlocking(drvPtr->sock);
            Ns_Log(Notice, "%s: listening on %s:%d",
                   drvPtr->name, drvPtr->address, drvPtr->port);
        }
        drvPtr = drvPtr->nextPtr;
    }

    /*
     * Create the socket thread.
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

    /*
     * Create the spooler thread.
     */

    if (spoolerDisabled == 0) {
        if (ns_sockpair(spoolerPipe) != 0) {
            Ns_Fatal("driver: ns_sockpair() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }
        Ns_ThreadCreate(SpoolThread, NULL, 0, &spoolerThread);
    }

    /*
     * Create the writer thread.
     */

    if (writerDisabled == 0) {
        if (ns_sockpair(writerPipe) != 0) {
            Ns_Fatal("driver: ns_sockpair() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }
        Ns_ThreadCreate(WriterThread, NULL, 0, &writerThread);
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
    Ns_MutexLock(&drvLock);
    if (!drvStopped && !driverShutdown) {
        Ns_Log(Notice, "driver: triggering shutdown");
        driverShutdown = 1;
        SockTrigger(drvPipe[1]);
    }
    Ns_MutexUnlock(&drvLock);

    if (!spoolerDisabled) {
        Ns_MutexLock(&spoolerLock);
        if (!spoolerStopped && !spoolerShutdown) {
            Ns_Log(Notice, "spooler: triggering shutdown");
            spoolerShutdown = 1;
            SockTrigger(spoolerPipe[1]);
        }
        Ns_MutexUnlock(&spoolerLock);
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
    int status = NS_OK;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch search;

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

    if (!spoolerDisabled) {
        Ns_MutexLock(&spoolerLock);
        while (!spoolerStopped && status == NS_OK) {
            status = Ns_CondTimedWait(&spoolerCond, &spoolerLock, toPtr);
        }
        Ns_MutexUnlock(&spoolerLock);
        if (status != NS_OK) {
            Ns_Log(Warning, "spooler: timeout waiting for shutdown");
        } else {
            Ns_Log(Notice, "spooler: shutdown complete");
            spoolerThread = NULL;
            ns_sockclose(spoolerPipe[0]);
            ns_sockclose(spoolerPipe[1]);
            Ns_MutexLock(&uploadLock);
            hPtr = Tcl_FirstHashEntry(&uploadTable, &search);
            while (hPtr != NULL) {
                Tcl_DeleteHashEntry(hPtr);
                hPtr = Tcl_NextHashEntry(&search);
            }
            Ns_MutexUnlock(&uploadLock);
        }
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
    /* NB: Sock no longer responsible for freeing request. */
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
        reqPtr->next = reqPtr->content = NULL;
        reqPtr->length = reqPtr->avail = 0;
        reqPtr->coff = reqPtr->woff = reqPtr->roff = 0;
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
    sockPtr->keep = keep;
    sockPtr->nextPtr = firstClosePtr;
    firstClosePtr = sockPtr;
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
    char    c, drain[1024];
    int     n, stopping, pollto;
    Sock   *sockPtr, *closePtr, *nextPtr, *waitPtr, *readPtr;
    Driver *activeDrvPtr;
    Driver *drvPtr, *nextDrvPtr, *idleDrvPtr, *acceptDrvPtr;
    Ns_Time timeout, now, diff;
    unsigned int nfds;           /* Number of Sock to poll(). */
    unsigned int maxfds;         /* Max pollfd's in pfds. */
    struct pollfd *pfds;         /* Array of pollfds to poll(). */

    
    Ns_ThreadSetName("-driver-");
    Ns_Log(Notice, "starting");

    /*
     * Build up the list of active drivers.
     */

    activeDrvPtr = NULL;
    drvPtr = firstDrvPtr;
    firstDrvPtr = NULL;
    while (drvPtr != NULL) {
        nextDrvPtr = drvPtr->nextPtr;
        if (drvPtr->sock != INVALID_SOCKET) {
            Push(drvPtr, activeDrvPtr);
        } else {
            Push(drvPtr, firstDrvPtr);
        }
        drvPtr = nextDrvPtr;
    }

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

    while (!stopping || nactive) {

        /*
         * Set the bits for all active drivers if a connection
         * isn't already pending.
         */
        
        nfds = 1;
        if (waitPtr == NULL) {
            drvPtr = activeDrvPtr;
            while (drvPtr != NULL) {
                pfds[nfds].fd = drvPtr->sock;
                pfds[nfds].events = POLLIN;
                drvPtr->pidx = nfds++;
                drvPtr = drvPtr->nextPtr;
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
            Ns_Fatal("driver: poll() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }
        if ((pfds[0].revents & POLLIN) && recv(drvPipe[0], &c, 1, 0) != 1) {
            Ns_Fatal("driver: trigger recv() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }
        
        /*
         * Update the current time and drain and/or release any
         * closing sockets.
         */
        
        Ns_GetTime(&now);
        if (closePtr != NULL) {
            sockPtr = closePtr;
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
                    SockRelease(sockPtr, Reason_CloseTimeout);
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
                    SockRelease(sockPtr, Reason_ReadTimeout);
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
                    if (!SockSpoolerPush(sockPtr)) {
                        Push(sockPtr, readPtr);
                    }
                    break;
                case SOCK_MORE:
                    SockTimeout(sockPtr, &now, sockPtr->drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;
                case SOCK_READY:
                    if (!SetServer(sockPtr)) {
                        SockRelease(sockPtr, Reason_ServerReject);
                    } else {
                        Push(sockPtr, waitPtr);
                    }
                    break;
                default:
                    SockRelease(sockPtr, Reason_SockError);
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
            drvPtr = activeDrvPtr;
            activeDrvPtr = idleDrvPtr = acceptDrvPtr = NULL;
            while (drvPtr != NULL) {
                nextDrvPtr = drvPtr->nextPtr;
                if (waitPtr != NULL
                    || (!(pfds[drvPtr->pidx].revents & POLLIN))
                    || ((sockPtr = SockAccept(drvPtr)) == NULL)) {
                    
                    /*
                     * Add this driver to the temporary idle list.
                     */
                    
                    Push(drvPtr, idleDrvPtr);
                    

                } else {

                    /*
                     * Add this driver to the temporary accepted list.
                     */
                    
                    Push(drvPtr, acceptDrvPtr);

                    /*
                     * Queue the socket immediately if request is provided
                     */
                    n = (*sockPtr->drvPtr->proc)(DriverAccept, (Ns_Sock*)sockPtr, 0, 0);
                    if (n == NS_OK && sockPtr->reqPtr) {
                        if (!SetServer(sockPtr)) {
                            SockRelease(sockPtr, Reason_ServerReject);
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
                drvPtr = nextDrvPtr;
            }
            
            /*
             * Put the active driver list back together with the idle
             * drivers first but otherwise in the original order.  This
             * should ensure round-robin service of the drivers.
             */
            
            while ((drvPtr = acceptDrvPtr) != NULL) {
                acceptDrvPtr = drvPtr->nextPtr;
                Push(drvPtr, activeDrvPtr);
            }
            while ((drvPtr = idleDrvPtr) != NULL) {
                idleDrvPtr = drvPtr->nextPtr;
                Push(drvPtr, activeDrvPtr);
            }
        }
        
        /*
         * Check for shutdown and get the list of any closing or
         * keepalive sockets.
         */
        
        Ns_MutexLock(&drvLock);
        sockPtr = firstClosePtr;
        firstClosePtr = NULL;
        stopping = driverShutdown;
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
                    SockRelease(sockPtr, Reason_SockShutError);
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
            while ((drvPtr = activeDrvPtr) != NULL) {
                activeDrvPtr = drvPtr->nextPtr;
                if (drvPtr->sock != INVALID_SOCKET) {
                    ns_sockclose(drvPtr->sock);
                    drvPtr->sock = INVALID_SOCKET;
                }
                Push(drvPtr, firstDrvPtr);
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
    
    sockPtr->servPtr = sockPtr->drvPtr->servPtr;
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
            sockPtr->servPtr = mapPtr->servPtr;
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
SockPoll(Sock *sockPtr, int type, struct pollfd **pfds, unsigned int *nfds, unsigned int *maxfds, Ns_Time *timeoutPtr)
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
    
    (*pfds)[*nfds].fd = sockPtr->sock;
    (*pfds)[*nfds].events = type;
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
    Sock *sockPtr;
    int   slen;

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
    sockPtr->drvPtr = drvPtr;
    sockPtr->tfd = 0;
    sockPtr->taddr = 0;
    sockPtr->keep = 0;
    sockPtr->arg = NULL;
    sockPtr->upload.url = NULL;
    sockPtr->upload.size = 0;
    sockPtr->upload.length = 0;

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
    ++nactive;

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
SockRelease(Sock *sockPtr, ReleaseReasons reason)
{
    char *errMsg = NULL;

    switch (reason) {
    case Reason_Close:
    case Reason_CloseTimeout:
        /* This is normal, never log. */
        break;
    case Reason_ReadTimeout:
    case Reason_WriteTimeout:
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
    case Reason_ServerReject:
        if (sockPtr->drvPtr->loggingFlags & LOGGING_SERVERREJECT) {
            errMsg = "No Server found for request";
        }
        break;
    case Reason_SockError:
        if (sockPtr->drvPtr->loggingFlags & LOGGING_SOCKERROR) {
            errMsg = "Unable to read request";
        }
        break;
    case Reason_SockShutError:
        if (sockPtr->drvPtr->loggingFlags & LOGGING_SOCKSHUTERROR) {
            errMsg = "Unable to shutdown socket";
        }
        break;
    }
    if (errMsg != NULL) {
        Ns_Log( Error, "Releasing Socket; %s, Peer =  %s:%d", 
                errMsg, ns_inet_ntoa(sockPtr->sa.sin_addr),
                ntohs(sockPtr->sa.sin_port) );
    }

    SockClose(sockPtr, 0);
    
    --nactive;
    if (sockPtr->sock != INVALID_SOCKET) {
        ns_sockclose(sockPtr->sock);
        sockPtr->sock = INVALID_SOCKET;
    }
    if (sockPtr->reqPtr != NULL) {
        NsFreeRequest(sockPtr->reqPtr);
        sockPtr->reqPtr = NULL;
    }

    Ns_MutexLock(&drvLock);
    sockPtr->nextPtr = firstSockPtr;
    firstSockPtr = sockPtr;
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
        Ns_Fatal("driver: trigger send() failed: %s",
                 ns_sockstrerror(ns_sockerrno));
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
    Tcl_HashEntry *hPtr;

    if (keep && (*sockPtr->drvPtr->proc)(DriverKeep, sock, NULL, 0) != 0) {
        keep = 0;
    }
    if (!keep) {
        (void) (*sockPtr->drvPtr->proc)(DriverClose, sock, NULL, 0);
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
    sockPtr->keep = keep;
#endif

    /*
     * Cleanup upload statistics hash table
     */

    if (sockPtr->upload.url != NULL) {
        Ns_Log(Debug, "upload stats deleted: %s, %lu %lu", sockPtr->upload.url, sockPtr->upload.length, sockPtr->upload.size);
        Ns_MutexLock(&uploadLock);
        hPtr = Tcl_FindHashEntry(&uploadTable, sockPtr->upload.url);
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }
        Ns_MutexUnlock(&uploadLock);
        ns_free(sockPtr->upload.url);
        sockPtr->upload.url = NULL;
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
    Ns_Sock      *sock = (Ns_Sock *) sockPtr;
    struct iovec  buf;
    Request      *reqPtr;
    Tcl_DString  *bufPtr;
    char         tbuf[4096];
    int          len, nread, n;
    
    Ns_DriverSockRequest(sock, 0);

    /*
     * On the first read, attempt to read-ahead bufsize bytes.
     * Otherwise, read only the number of bytes left in the
     * content.
     */
    
    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;
    if (reqPtr->length == 0) {
        nread = sockPtr->drvPtr->bufsize;
    } else {
        nread = reqPtr->length - reqPtr->avail;
    }

    /*
     * Grow the buffer to include space for the next bytes.
     */
    
    len = bufPtr->length;
    n = len + nread;
    if (n > sockPtr->drvPtr->maxinput) {
        n = sockPtr->drvPtr->maxinput;
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
        reqPtr->length > sockPtr->drvPtr->readahead &&
        sockPtr->tfd <= 0) {

        /*
         * In driver mode send this Sock to the spooler thread if
         * it is running
         */

        if (spooler == 0 && spoolerDisabled == 0) {
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
            return SOCK_ERROR;
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
        return SOCK_ERROR;
    }
    if (sockPtr->tfd > 0) {
        if (write(sockPtr->tfd, tbuf, n) != n) {
     	    return SOCK_ERROR;
        }
    } else {
        Tcl_DStringSetLength(bufPtr, len + n);
    }
    reqPtr->woff  += n;
    reqPtr->avail += n;

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

        if ((e - s) > sockPtr->drvPtr->maxline) {
            return SOCK_ERROR;
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
                    
                    return SOCK_ERROR;
                }

            } else if (Ns_ParseHeader(reqPtr->headers, s, Preserve) != NS_OK) {
                
                /*
                 * Invalid header.
                 */
                
                return SOCK_ERROR;
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
            sockPtr->tsize = reqPtr->length + 1;
            sockPtr->taddr = mmap(0, sockPtr->tsize, PROT_READ|PROT_WRITE, MAP_PRIVATE,
                                  sockPtr->tfd, 0);
            if (sockPtr->taddr == MAP_FAILED) {
                sockPtr->taddr = NULL;
                return SOCK_ERROR;
            }
            reqPtr->content = sockPtr->taddr;
            Ns_Log(Debug, "spooling content to file: readahead=%d, filesize=%i",
                   sockPtr->drvPtr->readahead, (int)sockPtr->tsize);
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

    if (sockPtr->upload.url == NULL) {
        if (reqPtr->length > 0 && reqPtr->avail > sockPtr->drvPtr->uploadsize) {
            Tcl_HashEntry *hPtr;
            Ns_Request *req = reqPtr->request;

            sockPtr->upload.url = ns_calloc(1, strlen(req->url) + strlen(req->query ? req->query : "") + 3);
            sprintf(sockPtr->upload.url, "%s?%s", req->url, (req->query ? req->query : ""));
            sockPtr->upload.length = reqPtr->length;
            sockPtr->upload.size = reqPtr->avail;
            Ns_MutexLock(&uploadLock);
            hPtr = Tcl_CreateHashEntry(&uploadTable, sockPtr->upload.url, &cnt);
            Tcl_SetHashValue(hPtr, sockPtr);
            Ns_MutexUnlock(&uploadLock);
            Ns_Log(Debug, "upload stats created for %s", sockPtr->upload.url);
        }
    } else {
        Ns_MutexLock(&uploadLock);
        sockPtr->upload.length = reqPtr->length;
        sockPtr->upload.size = reqPtr->avail;
        Ns_MutexUnlock(&uploadLock);
    }
    
    return SOCK_MORE;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_DriverSockRequest --
 *
 *      Allocates new request struct for the given socket, if data is specified,
 *      it becomes parsed request struct, i.e. should be in the form: METHOD URL PROTO
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

void
Ns_DriverSockRequest(Ns_Sock *sock, char *reqline)
{
    Request      *reqPtr;
    Sock         *sockPtr = (Sock*)sock;

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
            reqPtr->headers = Ns_SetCreate(NULL);
            reqPtr->request = NULL;
            reqPtr->next = reqPtr->content = NULL;
            reqPtr->length = reqPtr->avail = 0;
            reqPtr->coff = reqPtr->woff = reqPtr->roff = 0;
            reqPtr->leadblanks = 0;
        }
        sockPtr->reqPtr = reqPtr;
        reqPtr->port = ntohs(sockPtr->sa.sin_port);
        strcpy(reqPtr->peer, ns_inet_ntoa(sockPtr->sa.sin_addr));

        if (reqline) {
            reqPtr->request = Ns_ParseRequest(reqline);
        }
    }
}

int
NsTclUploadStatsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char buf[64] = "";
    Tcl_HashEntry *hPtr;
    Sock *sockPtr;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "url");
        return TCL_ERROR;
    }

    Ns_MutexLock(&uploadLock);
    hPtr = Tcl_FindHashEntry(&uploadTable, Tcl_GetString(objv[1]));
    if (hPtr != NULL) {
        sockPtr = Tcl_GetHashValue(hPtr);
        sprintf(buf, "%lu %lu", sockPtr->upload.length, sockPtr->upload.size);
    }
    Ns_MutexUnlock(&uploadLock);
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
SpoolThread(void *ignored)
{
    char c;
    int n, stopping, pollto;
    Sock  *sockPtr, *nextPtr, *waitPtr, *readPtr;
    Ns_Time timeout, now, diff;
    unsigned int nfds, maxfds;
    struct pollfd *pfds;
    
    Ns_ThreadSetName("-spooler-");

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "spooler: accepting connections");
    waitPtr = readPtr = NULL;
    Ns_GetTime(&now);
    stopping = 0;
    maxfds = 100;
    pfds = ns_malloc(maxfds * sizeof(struct pollfd));
    pfds[0].fd = spoolerPipe[0];
    pfds[0].events = POLLIN;

    while (!stopping || nactive) {

        /*
         * Set the bits for all active drivers if a connection
         * isn't already pending.
         */
        
        nfds = 1;
        
        /*
         * If there are any read sockets, set the bits
         * and determine the minimum relative timeout.
         */
        
        if (readPtr == NULL) {
            pollto = 60 * 1000;
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
        if ((pfds[0].revents & POLLIN) && recv(spoolerPipe[0], &c, 1, 0) != 1) {
            Ns_Fatal("driver: trigger recv() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
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
                    SockRelease(sockPtr, Reason_ReadTimeout);
                } else {
                    Push(sockPtr, readPtr);
                }
            } else {

                /*
                 * If enabled, perform read-ahead now.
                 */
                
                sockPtr->keep = 0;
                if (sockPtr->drvPtr->opts & NS_DRIVER_ASYNC) {
                    n = SockRead(sockPtr, 1);
                } else {
                    n = SOCK_READY;
                }
                
                /*
                 * Queue for connection processing if ready.
                 */
                
                switch (n) {
                case SOCK_MORE:
                    SockTimeout(sockPtr, &now, sockPtr->drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;
                case SOCK_READY:
                    if (!SetServer(sockPtr)) {
                        SockRelease(sockPtr, Reason_ServerReject);
                    } else {
                        Push(sockPtr, waitPtr);
                    }
                    break;
                default:
                    SockRelease(sockPtr, Reason_SockError);
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
        
        if (waitPtr == NULL && ((sockPtr = SockSpoolerPop()))) {
            SockTimeout(sockPtr, &now, sockPtr->drvPtr->recvwait);
            Push(sockPtr, readPtr);
        }

        /*
         * Check for shutdown 
         */
        
        Ns_MutexLock(&spoolerLock);
        stopping = spoolerShutdown;
        Ns_MutexUnlock(&spoolerLock);
    }
    Ns_Log(Notice, "exiting");
    Ns_MutexLock(&spoolerLock);
    spoolerStopped = 1;
    Ns_CondBroadcast(&spoolerCond);
    Ns_MutexUnlock(&spoolerLock);
}

static int
SockSpoolerPush(Sock *sockPtr)
{
    int trigger = 0;

    Ns_MutexLock(&spoolerLock);
    if (spoolerSockPtr == NULL) {
        trigger = 1;
    }
    Push(sockPtr, spoolerSockPtr);
    Ns_MutexUnlock(&spoolerLock);

    /*
     * Wake up spooler thread
     */

    if (trigger) {
        SockTrigger(spoolerPipe[1]);
    }
    return 1;
}

Sock *SockSpoolerPop(void)
{
    Sock *sockPtr = 0;

    Ns_MutexLock(&spoolerLock);
    sockPtr = spoolerSockPtr;
    if (spoolerSockPtr) {
         spoolerSockPtr = spoolerSockPtr->nextPtr;
    }
    Ns_MutexUnlock(&spoolerLock);
    return sockPtr;
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
WriterThread(void *ignored)
{
    char c;
    Ns_Time now;
    int n, stopping, pollto, toread, nread, status;
    WriterSock *sockPtr, *nextPtr, *writePtr;
    struct pollfd pfds[1];
    char buf[2048];

    Ns_ThreadSetName("-writer-");

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "writer: accepting connections");
    writePtr = NULL;
    Ns_GetTime(&now);
    stopping = 0;
    pfds[0].fd = writerPipe[0];
    pfds[0].events = POLLIN;

    while (!stopping || nactive) {

        /*
         * Select and drain the trigger pipe if necessary.
         */

        if (writePtr == NULL) {
            pfds[0].revents = 0;
            pollto = 30 * 1000;  // Wake up every 30 seconds just in case
            do {
                n = poll(pfds, 1, pollto);
            } while (n < 0  && errno == EINTR);
            if (n < 0) {
                Ns_Fatal("driver: poll() failed: %s", ns_sockstrerror(ns_sockerrno));
            }
            if ((pfds[0].revents & POLLIN) && recv(writerPipe[0], &c, 1, 0) != 1) {
                Ns_Fatal("driver: trigger recv() failed: %s", ns_sockstrerror(ns_sockerrno));
            }
        }

        /*
         * Attempt write to all available sockets
         */
        
        sockPtr = writePtr;
        writePtr = NULL;
        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;

            /*
             * Read block from the file and send it to the socket
             */

            status = NS_OK;
            if (sockPtr->nsend > 0) {
                toread = sockPtr->nsend;
                if (toread > sizeof(buf)) {
                    toread = sizeof(buf);
                }
                nread = read(sockPtr->fd, buf, (size_t)toread);
                if (nread == -1) {
                    status = NS_ERROR;
                } else if (nread == 0) {
                    sockPtr->nsend = 0;  /* NB: Silently ignore a truncated file. */
                } else {
                    n = Ns_SockSend(sockPtr->sockPtr->sock, buf, nread, 0);
                    if (n == nread) {
                        sockPtr->nsend -= n;
                    } else {
                        status = NS_ERROR;
                    }
                }
            }
            if (status != NS_OK) {
                SockWriterRelease(sockPtr, Reason_SockError);
            } else {
                if (sockPtr->nsend > 0) {
                    Push(sockPtr, writePtr);
                } else {
                    SockWriterRelease(sockPtr, 0);
                }
            }
            sockPtr = nextPtr;
        }

        /*
         * Add more sockets to the writer queue
         */

        Ns_MutexLock(&writerLock);
        sockPtr = writerSockPtr;
        writerSockPtr = NULL;
        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            SockTimeout(sockPtr->sockPtr, &now, sockPtr->sockPtr->drvPtr->sendwait);
            Push(sockPtr, writePtr);
            sockPtr = nextPtr;
        }

        /*
         * Check for shutdown
         */
        
        stopping = writerShutdown;
        Ns_MutexUnlock(&writerLock);
    }
    Ns_Log(Notice, "exiting");
    Ns_MutexLock(&writerLock);
    writerStopped = 1;
    Ns_CondBroadcast(&writerCond);
    Ns_MutexUnlock(&writerLock);
}

static void
SockWriterRelease(WriterSock *sockPtr, ReleaseReasons reason)
{
    Ns_Log(Notice, "Writer: stop fd=%d", sockPtr->fd);
    SockRelease(sockPtr->sockPtr, reason);
    close(sockPtr->fd);
    ns_free(sockPtr);
}

int
NsQueueWriter(Ns_Conn *conn, int nsend, Tcl_Channel chan, FILE *fp, int fd)
{
    Conn *connPtr = (Conn*)conn;
    WriterSock *sockPtr;
    int trigger = 0;

    if (writerDisabled ||
        nsend < connPtr->drvPtr->writersize ||
        (conn->flags & NS_CONN_WRITE_CHUNKED)) {
        return NS_ERROR;
    }

    /*
     * Flush the headers
     */

    Ns_WriteConn(conn, NULL, 0);

    sockPtr = (WriterSock*)ns_calloc(1, sizeof(WriterSock));
    sockPtr->sockPtr = connPtr->sockPtr;
    sockPtr->flags = connPtr->flags;
    if (chan != NULL) {
        if (Tcl_GetChannelHandle(chan, TCL_READABLE, (ClientData)&sockPtr->fd) != TCL_OK) {
            ns_free(sockPtr);
            return NS_ERROR;
        }
    } else if (fp != NULL) {
        sockPtr->fd = fileno(fp);
    }
    sockPtr->fd = ns_sockdup(sockPtr->fd);
    sockPtr->nsend = nsend;
    connPtr->sockPtr = NULL;
    // To keep nslog happy about content size sent
    connPtr->nContentSent = nsend;
    Ns_SockSetBlocking(sockPtr->sockPtr->sock);
    Ns_Log(Notice, "Writer: start fd=%d: %d bytes: %s", sockPtr->fd, nsend, connPtr->reqPtr->request->url);

    Ns_MutexLock(&writerLock);
    if (writerSockPtr == NULL) {
        trigger = 1;
    }
    Push(sockPtr, writerSockPtr);
    Ns_MutexUnlock(&writerLock);

    /*
     * Wake up writer thread
     */

    if (trigger) {
        SockTrigger(writerPipe[1]);
    }
    return NS_OK;
}
