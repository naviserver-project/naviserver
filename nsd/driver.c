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
#define SOCK_BADREQUEST          (-11)
#define SOCK_ENTITYTOOLARGE      (-12)
#define SOCK_BADHEADER           (-13)
#define SOCK_TOOMANYHEADERS      (-14)

/* WriterSock flags, keep it in upper range not to conflict with Conn flags */

#define WRITER_TIMEOUT           0x10000

/*
 * The following are valid driver state flags.
 */

#define DRIVER_STARTED           1
#define DRIVER_STOPPED           2
#define DRIVER_SHUTDOWN          4
#define DRIVER_FAILED            8
#define DRIVER_QUERY             16
#define DRIVER_DEBUG             32


/*
 * The following maintains Host header to server mappings.
 */

typedef struct ServerMap {
    NsServer *servPtr;
    char      location[1];
} ServerMap;

/*
 * The following structure manages polling.  The PollIn macro is
 * used for the common case of checking for readability.
 */

typedef struct PollData {
    int nfds;                   /* Number of fd's being monitored. */
    int maxfds;                 /* Max fd's (will grow as needed). */
    struct pollfd *pfds;        /* Dynamic array of poll struct's. */
    Ns_Time timeout;            /* Min timeout, if any, for next spin. */
} PollData;

#define PollIn(ppd,i)           ((ppd)->pfds[(i)].revents & POLLIN)
#define PollOut(ppd,i)          ((ppd)->pfds[(i)].revents & POLLOUT)
#define PollHup(ppd,i)          ((ppd)->pfds[(i)].revents & POLLHUP)


/*
 * Static functions defined in this file.
 */

static Ns_ThreadProc DriverThread;
static Ns_ThreadProc SpoolerThread;
static Ns_ThreadProc WriterThread;

static NS_SOCKET DriverListen(Driver *drvPtr);
static NS_DRIVER_ACCEPT_STATUS DriverAccept(Sock *sockPtr);
static ssize_t DriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs);
static int DriverKeep(Sock *sockPtr);
static void DriverClose(Sock *sockPtr);

static int   SockSetServer(Sock *sockPtr);
static int   SockAccept(Driver *drvPtr, Sock **sockPtrPtr);
static int   SockQueue(Sock *sockPtr, Ns_Time *timePtr);
static void  SockPrepare(Sock *sockPtr);
static void  SockRelease(Sock *sockPtr, int reason, int err);
static void  SockError(Sock *sockPtr, int reason, int err);
static void  SockSendResponse(Sock *sockPtr, int code, char *msg);
static void  SockTrigger(NS_SOCKET sock);
static void  SockTimeout(Sock *sockPtr, Ns_Time *nowPtr, int timeout);
static void  SockClose(Sock *sockPtr, int keep);
static int   SockRead(Sock *sockPtr, int spooler);
static int   SockParse(Sock *sockPtr, int spooler);
static void  SockPoll(Sock *sockPtr, int type, PollData *pdata);
static int   SockSpoolerQueue(Driver *drvPtr, Sock *sockPtr);
static void  SockWriterRelease(WriterSock *sockPtr, int reason, int err);
static void  SpoolerQueueStart(SpoolerQueue *queuePtr, Ns_ThreadProc *proc);
static void  SpoolerQueueStop(SpoolerQueue *queuePtr, Ns_Time *timeoutPtr);
static void  PollCreate(PollData *pdata);
static void  PollFree(PollData *pdata);
static void  PollReset(PollData *pdata);
static int   PollSet(PollData *pdata, NS_SOCKET sock, int type, Ns_Time *timeoutPtr);
static int   PollWait(PollData *pdata, int waittime);

/*
 * Static variables defined in this file.
 */

static Ns_LogSeverity DriverDebug;    /* Severity at which to log verbose debugging. */
static Tcl_HashTable hosts;           /* Host header to server table */

static ServerMap *defMapPtr = NULL;   /* Default srv when not found in table */

static Ns_Mutex   reqLock;             /* Lock for request free list */
static Request   *firstReqPtr = NULL;  /* Free list of request structures */
static Driver    *firstDrvPtr = NULL;  /* First in list of all drivers */

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
    DriverDebug = Ns_CreateLogSeverity("Debug(ns:driver)");
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

    if (init->version != NS_DRIVER_VERSION_2) {
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

        if (host == NULL && he != NULL && he->h_name != NULL &&
            strchr(he->h_name, '.') == NULL) {
            he = gethostbyaddr(he->h_addr_list[0],he->h_length,he->h_addrtype);
        }

        /*
         * If the lookup suceeded, use the first address in host entry list.
         */

        if (he == NULL || he->h_name == NULL) {
            Ns_Log(Error, "%s: could not resolve %s", module,
                   host ? host : Ns_InfoHostname());
            return NS_ERROR;
        }
        if (*(he->h_addr_list) == NULL) {
            Ns_Log(Error, "%s: no addresses for %s", module,
                   he->h_name);
            return NS_ERROR;
        }

        memcpy(&ia.s_addr, he->h_addr_list[0], sizeof(ia.s_addr));
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

    Ns_MutexSetName2(&drvPtr->lock, "ns:drv", module);

    if (ns_sockpair(drvPtr->trigger) != 0) {
        Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
    }

    drvPtr->server       = server;
    drvPtr->name         = init->name;
    drvPtr->module       = module;
    drvPtr->listenProc   = init->listenProc;
    drvPtr->acceptProc   = init->acceptProc;
    drvPtr->recvProc     = init->recvProc;
    drvPtr->sendProc     = init->sendProc;
    drvPtr->sendFileProc = init->sendFileProc;
    drvPtr->keepProc     = init->keepProc;
    drvPtr->requestProc  = init->requestProc;
    drvPtr->closeProc    = init->closeProc;
    drvPtr->arg          = init->arg;
    drvPtr->opts         = init->opts;
    drvPtr->servPtr      = servPtr;

    drvPtr->maxinput     = Ns_ConfigWideIntRange(path, "maxinput",
                                             1024*1024,    1024, LLONG_MAX);

    drvPtr->maxupload     = Ns_ConfigWideIntRange(path, "maxupload",
                                             0,            0, drvPtr->maxinput);

    drvPtr->maxline      = Ns_ConfigIntRange(path, "maxline",
                                             8192,         256, INT_MAX);

    drvPtr->maxheaders   = Ns_ConfigIntRange(path, "maxheaders",
                                             128,          8, INT_MAX);

    drvPtr->bufsize      = Ns_ConfigIntRange(path, "bufsize",
                                             16384,        1024, INT_MAX);

    drvPtr->maxqueuesize = Ns_ConfigIntRange(path, "maxqueuesize",
                                             1024,         1, INT_MAX);

    drvPtr->sendwait     = Ns_ConfigIntRange(path, "sendwait",
                                             30,           1, INT_MAX);

    drvPtr->recvwait     = Ns_ConfigIntRange(path, "recvwait",
                                             30,           1, INT_MAX);

    drvPtr->closewait    = Ns_ConfigIntRange(path, "closewait",
                                             2,            0, INT_MAX);

    drvPtr->keepwait     = Ns_ConfigIntRange(path, "keepwait",
                                             5,            0, INT_MAX);

    drvPtr->keepmaxuploadsize = Ns_ConfigIntRange(path, "keepalivemaxuploadsize",
                                             0,            0, INT_MAX);

    drvPtr->keepmaxdownloadsize = Ns_ConfigIntRange(path, "keepalivemaxdownloadsize",
                                             0,            0, INT_MAX);

    drvPtr->backlog      = Ns_ConfigIntRange(path, "backlog",
                                             256,          1, INT_MAX);

    drvPtr->readahead    = Ns_ConfigWideIntRange(path, "readahead",
                                             drvPtr->bufsize,
                                             drvPtr->bufsize, drvPtr->maxinput);

    drvPtr->acceptsize   = Ns_ConfigIntRange(path, "acceptsize",
                                             drvPtr->backlog, 1, INT_MAX);

    /* Apparently not used; should be removed or filled with life
    drvPtr->keepallmethods = Ns_ConfigBool(path, "keepallmethods", NS_FALSE);
    */

    drvPtr->uploadpath = ns_strdup(Ns_ConfigString(path, "uploadpath", P_tmpdir));

    /*
     * Determine the port and then set the HTTP location string either
     * as specified in the config file or constructed from the
     * protocol, hostname and port.
     */

    drvPtr->bindaddr = bindaddr;
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

    if (spPtr->threads > 0) {
        Ns_Log(Notice, "%s: enable %d spooler thread(s) "
               "for uploads >= %ld bytes", module,
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
                                           8192, 512, INT_MAX);
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
    /* int status = NS_OK;*/

    /*
     * Signal and wait for each driver to start.
     */

    drvPtr = firstDrvPtr;
    while (drvPtr != NULL) {
        Ns_Log(Notice, "driver: starting: %s", drvPtr->name);
        Ns_ThreadCreate(DriverThread, drvPtr, 0, &drvPtr->thread);
        Ns_MutexLock(&drvPtr->lock);
        while (!(drvPtr->flags & DRIVER_STARTED)) {
            Ns_CondWait(&drvPtr->cond, &drvPtr->lock);
        }
        /*if ((drvPtr->flags & DRIVER_FAILED)) {
            status = NS_ERROR;
	    }*/
        Ns_MutexUnlock(&drvPtr->lock);
        drvPtr = drvPtr->nextPtr;
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
    Driver *drvPtr = firstDrvPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch search;

    while (drvPtr != NULL) {
        Ns_MutexLock(&drvPtr->lock);
        Ns_Log(Notice, "driver: stopping: %s", drvPtr->name);
        drvPtr->flags |= DRIVER_SHUTDOWN;
        Ns_CondBroadcast(&drvPtr->cond);
        Ns_MutexUnlock(&drvPtr->lock);
        SockTrigger(drvPtr->trigger[1]);
        drvPtr = drvPtr->nextPtr;
    }

    hPtr = Tcl_FirstHashEntry(&hosts, &search);
    while (hPtr != NULL) {
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
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
    Driver *drvPtr = firstDrvPtr;
    int status = NS_OK;

    while (drvPtr != NULL) {
        Ns_MutexLock(&drvPtr->lock);
        while (!(drvPtr->flags & DRIVER_STOPPED) && status == NS_OK) {
            status = Ns_CondTimedWait(&drvPtr->cond, &drvPtr->lock, toPtr);
        }
        Ns_MutexUnlock(&drvPtr->lock);
        if (status != NS_OK) {
            Ns_Log(Warning, "driver: shutdown timeout: %s", drvPtr->module);
        } else {
            Ns_Log(Notice, "driver: stopped: %s", drvPtr->module);
            Ns_ThreadJoin(&drvPtr->thread, NULL);
            drvPtr->thread = NULL;
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

    if (sockPtr->reqPtr == NULL) {
        int status;
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

        reqPtr->expectedLength = 0;
        reqPtr->contentLength  = 0;

        Tcl_DStringFree(&reqPtr->buffer);

        Ns_SetTrunc(reqPtr->headers, 0);

        Ns_ResetRequest(&reqPtr->request);

        Ns_MutexLock(&reqLock);
        reqPtr->nextPtr = firstReqPtr;
        firstReqPtr = reqPtr;
        Ns_MutexUnlock(&reqLock);
    }
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
    Driver *drvPtr = sockPtr->drvPtr;
    int     trigger = 0;

    SockClose(sockPtr, keep);

    Ns_MutexLock(&drvPtr->lock);
    if (drvPtr->closePtr == NULL) {
        trigger = 1;
    }
    sockPtr->nextPtr = drvPtr->closePtr;
    drvPtr->closePtr = sockPtr;
    Ns_MutexUnlock(&drvPtr->lock);

    if (trigger) {
        SockTrigger(drvPtr->trigger[1]);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DriverListen --
 *
 *      Open a listening socket for accepting connections.
 *
 * Results:
 *      File description of socket, or INVALID_SOCKET on error.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
DriverListen(Driver *drvPtr)
{
    NS_SOCKET sock;

    sock = (*drvPtr->listenProc)((Ns_Driver *) drvPtr,
                                 drvPtr->bindaddr,
                                 drvPtr->port,
                                 drvPtr->backlog);
    if (sock == INVALID_SOCKET) {
        Ns_Log(Error, "%s: failed to listen on %s:%d: %s",
               drvPtr->name, drvPtr->address, drvPtr->port,
               ns_sockstrerror(ns_sockerrno));
    } else {
        Ns_Log(Notice, "%s: listening on %s:%d",
               drvPtr->name, drvPtr->address, drvPtr->port);
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverAccept --
 *
 *      Accept a new socket. It will be in non-blocking mode.
 *
 * Results:
 *      _ACCEPT:       a socket was accepted, poll for data
 *      _ACCEPT_DATA:  a socket was accepted, data present, read immediately
 *                     if in async mode, defer reading to connection thread
 *      _ACCEPT_QUEUE: a socket was accepted, queue immediately
 *      _ACCEPT_ERROR: no socket was accepted
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static NS_DRIVER_ACCEPT_STATUS
DriverAccept(Sock *sockPtr)
{
    int n = sizeof(struct sockaddr_in);

    return (*sockPtr->drvPtr->acceptProc)((Ns_Sock *) sockPtr,
                                          sockPtr->drvPtr->sock,
                                          (struct sockaddr *) &sockPtr->sa, &n);
}


/*
 *----------------------------------------------------------------------
 *
 * DriverRecv --
 *
 *      Read data from the socket into the given vector of buffers.
 *
 * Results:
 *      Number of bytes read, or -1 on error.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
DriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs)
{
    Ns_Time timeout;

    timeout.sec = sockPtr->drvPtr->recvwait;
    timeout.usec = 0;

    return (*sockPtr->drvPtr->recvProc)((Ns_Sock *) sockPtr, bufs, nbufs,
                                        &timeout, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * NsDriverSend --
 *
 *      Write a vector of buffers to the socket via the driver callback.
 *
 * Results:
 *      Number of bytes written, or -1 on error.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

ssize_t
NsDriverSend(Sock *sockPtr, struct iovec *bufs, int nbufs, int flags)
{
    Ns_Time timeout;

    timeout.sec = sockPtr->drvPtr->sendwait;
    timeout.usec = 0;

    return (*sockPtr->drvPtr->sendProc)((Ns_Sock *) sockPtr, bufs, nbufs,
                                        &timeout, flags);
}


/*
 *----------------------------------------------------------------------
 *
 * NsDriverSendFile --
 *
 *      Write a vector of file buffers to the socket via the driver
 *      callback. Fallback to default implementation if driver does
 *      not supply one.
 *
 * Results:
 *      Number of bytes written, or -1 on error.
 *
 * Side effects:
 *      May block on disk I/O.
 *
 *----------------------------------------------------------------------
 */

ssize_t
NsDriverSendFile(Sock *sockPtr, Ns_FileVec *bufs, int nbufs, int flags)
{
    Driver  *drvPtr = sockPtr->drvPtr;
    ssize_t  n;
    Ns_Time  timeout;

    timeout.sec = drvPtr->sendwait;
    timeout.usec = 0;

    if (drvPtr->sendFileProc != NULL) {
        n = (*drvPtr->sendFileProc)((Ns_Sock *) sockPtr, bufs, nbufs,
                                    &timeout, flags);
    } else {
        n = NsSockSendFileBufsIndirect((Ns_Sock *) sockPtr, bufs, nbufs,
                                       &timeout, flags,
                                       drvPtr->sendProc);
    }
    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverKeep --
 *
 *      Can the given socket be kept open in the hopes that another
 *      request will arrive before the keepwait timeout expires?
 *
 * Results:
 *      1 if the socket is OK for keepalive, 0 if this is not possible.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static int
DriverKeep(Sock *sockPtr)
{
    return (*sockPtr->drvPtr->keepProc)((Ns_Sock *) sockPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * DriverClose --
 *
 *      Close the given socket.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static void
DriverClose(Sock *sockPtr)
{
    (*sockPtr->drvPtr->closeProc)((Ns_Sock *) sockPtr);
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
DriverThread(void *arg)
{
    Driver        *drvPtr = (Driver*)arg;
    Ns_Time        timeout, now, diff;
    char          *errstr, c, drain[1024];
    int            n, flags, stopping, pollto, accepted;
    Sock          *sockPtr, *closePtr, *nextPtr, *waitPtr, *readPtr;
    PollData       pdata;

    Ns_ThreadSetName("-driver:%s-", drvPtr->name);

    flags = DRIVER_STARTED;
    drvPtr->sock = DriverListen(drvPtr);

    if (drvPtr->sock == INVALID_SOCKET) {
        flags |= (DRIVER_FAILED | DRIVER_SHUTDOWN);
    } else {
        SpoolerQueueStart(drvPtr->spooler.firstPtr, SpoolerThread);
        SpoolerQueueStart(drvPtr->writer.firstPtr, WriterThread);
    }

    Ns_MutexLock(&drvPtr->lock);
    drvPtr->flags |= flags;
    Ns_CondBroadcast(&drvPtr->cond);
    Ns_MutexUnlock(&drvPtr->lock);

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "driver: accepting connections");

    PollCreate(&pdata);
    Ns_GetTime(&now);
    closePtr = waitPtr = readPtr = NULL;
    stopping = (flags & DRIVER_SHUTDOWN);

    while (!stopping) {

        /*
         * Set the bits for all active drivers if a connection
         * isn't already pending.
         */

        PollReset(&pdata);
        PollSet(&pdata, drvPtr->trigger[0], POLLIN, NULL);

        if (waitPtr == NULL) {
            drvPtr->pidx = PollSet(&pdata, drvPtr->sock, POLLIN, NULL);
        }

        /*
         * If there are any closing or read-ahead sockets, set the bits
         * and determine the minimum relative timeout.
	 *
	 * TODO: the various poll time outs should probably be configurable.
         */

        if (readPtr == NULL && closePtr == NULL) {
            pollto = 1 * 1000;
        } else {
            sockPtr = readPtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, POLLIN, &pdata);
                sockPtr = sockPtr->nextPtr;
            }
            sockPtr = closePtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, POLLIN, &pdata);
                sockPtr = sockPtr->nextPtr;
            }
            if (Ns_DiffTime(&pdata.timeout, &now, &diff) > 0)  {
	        pollto = (int)(diff.sec * 1000 + diff.usec / 1000);
            } else {
                pollto = 0;
            }
        }

        n = PollWait(&pdata, pollto);

        if (PollIn(&pdata, 0) && recv(drvPtr->trigger[0], &c, 1, 0) != 1) {
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
		if (PollHup(&pdata, sockPtr->pidx)) {
		  /*
		   * Peer has closed the connection
		   */
		  sockPtr->timeout = now;

		} else if (PollIn(&pdata, sockPtr->pidx)) {
		  /* 
		   * Got some data
		   */
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

	    if (PollHup(&pdata, sockPtr->pidx)) {
	      /*
	       * Peer has closed the connection
	       */
	      SockRelease(sockPtr, SOCK_CLOSE, 0);

	    } else if (PollIn(&pdata, sockPtr->pidx) == 0) {
	      /*
	       * Got no data
	       */
                if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                } else {
                    Push(sockPtr, readPtr);
                }

            } else {

                /*
		 * Got some data.
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
                    if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
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
                if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                    Push(sockPtr, waitPtr);
                }
                sockPtr = nextPtr;
            }
        }

        /*
         * If no connections are waiting, attempt to accept more.
         */

        if (waitPtr == NULL) {
            /*
             * If configured, try to accept more than one request, under heavy load
             * this helps to process more requests
             */

            accepted = 0;
            while (accepted < drvPtr->acceptsize
                   && drvPtr->queuesize < drvPtr->maxqueuesize
                   && PollIn(&pdata, drvPtr->pidx)
                   && (n = SockAccept(drvPtr, &sockPtr)) != SOCK_ERROR) {

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
                    if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                        Push(sockPtr, waitPtr);
                    }
                    break;

                default:
                    Ns_Fatal("driver: SockAccept returned: %d", n);
                }
                accepted++;
            }

	    /*
	     * Check whether we should reanimate some connection
	     * threads. Under normal conditions, requests are dropping
	     * in on a regular basis, and the liveliness of the
	     * connection threads is checked when requests are
	     * queued. However, on bursty loads that suddenly stop, it
	     * is possible that the total number of requests allowed
	     * to be processed by the existing connection threads is
	     * less than the number of queued requests. Therefore,
	     * when the last connection thread terminates, queued
	     * remaining requests might be waiting.  Since the logic
	     * for creating connection threads in on the driver side,
	     * we care here about the already queued requests. The
	     * check is performed only in the timeout case (when n ==
	     * 0)
 	     */
	    if (n == 0 && drvPtr->servPtr) {
	      NsEnsureRunningConnectionThreads(drvPtr->servPtr, NULL);
	  }

        }

        /*
         * Check for shutdown and get the list of any closing
         * or keepalive sockets.
         */

        Ns_MutexLock(&drvPtr->lock);
        sockPtr          = drvPtr->closePtr;
        drvPtr->closePtr = NULL;
        flags            = drvPtr->flags;
        Ns_MutexUnlock(&drvPtr->lock);

        stopping = (flags & DRIVER_SHUTDOWN);

        /*
         * Update the timeout for each closing socket and add to the
         * close list if some data has been read from the socket
         * (i.e., it's not a closing keep-alive connection).
         */

        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            if (sockPtr->keep) {
	        /*
		 * When keep-alive is set and more requests are
		 * already in the request queue, don't timeout but
		 * process the requests immediately.
	         */
	        if (drvPtr->queuesize > 1 || PollIn(&pdata, sockPtr->pidx)) {
		    /*fprintf(stderr, "FIX timeout keepwait %d drvPtr->queuesize %d flags %d %.6x pollin %d\n", 
		      sockPtr->drvPtr->keepwait, drvPtr->queuesize, sockPtr->flags, sockPtr->flags,
		      PollIn(&pdata, sockPtr->pidx)); */
		    sockPtr->timeout = now;
		} else {
		    /*fprintf(stderr, "Update the timeout for each closing socket %d with keepwait %d drvPtr->queuesize %d\n", 
		      PollIn(&pdata, drvPtr->pidx), sockPtr->drvPtr->keepwait, drvPtr->queuesize);*/
		  SockTimeout(sockPtr, &now, sockPtr->drvPtr->keepwait);
		}
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
            ns_sockclose(drvPtr->sock);
            drvPtr->sock = INVALID_SOCKET;
        }
    }

    PollFree(&pdata);

    Ns_Log(Notice, "exiting");
    Ns_MutexLock(&drvPtr->lock);
    drvPtr->flags |= DRIVER_STOPPED;
    Ns_CondBroadcast(&drvPtr->cond);
    Ns_MutexUnlock(&drvPtr->lock);

    /*
     * Shutdown all spooler and writer threads
     */

    Ns_GetTime(&timeout);
    Ns_IncrTime(&timeout, nsconf.shutdowntimeout, 0);

    SpoolerQueueStop(drvPtr->writer.firstPtr, &timeout);
    SpoolerQueueStop(drvPtr->spooler.firstPtr, &timeout);
}

static void
PollCreate(PollData *pdata)
{
    memset((pdata), 0, sizeof(PollData));
}

static void
PollFree(PollData *pdata)
{
    ns_free(pdata->pfds);
    memset((pdata), 0, sizeof(PollData));
}

static void
PollReset(PollData *pdata)
{
    pdata->nfds = 0;
    pdata->timeout.sec = TIME_T_MAX;
    pdata->timeout.usec = 0;
}

static int
PollSet(PollData *pdata, NS_SOCKET sock, int type, Ns_Time *timeoutPtr)
{
    /*
     * Grow the pfds array if necessary.
     */

    if (pdata->nfds >= pdata->maxfds) {
        pdata->maxfds += 100;
        pdata->pfds = ns_realloc(pdata->pfds, pdata->maxfds * sizeof(struct pollfd));
    }

    /*
     * Set the next pollfd struct with this socket.
     */

    pdata->pfds[pdata->nfds].fd = sock;
    pdata->pfds[pdata->nfds].events = type;
    pdata->pfds[pdata->nfds].revents = 0;

    /*
     * Check for new minimum timeout.
     */

    if (timeoutPtr != NULL && Ns_DiffTime(timeoutPtr, &pdata->timeout, NULL) < 0) {
        pdata->timeout = *timeoutPtr;
    }

    return pdata->nfds++;
}

static int
PollWait(PollData *pdata, int waittime)
{
    int n;

    do {
        n = ns_poll(pdata->pfds, pdata->nfds, waittime);
    } while (n < 0  && errno == EINTR);

    if (n < 0) {
        Ns_Fatal("PollWait: ns_poll() failed: %s", ns_sockstrerror(ns_sockerrno));
    }
    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * SockPrepare
 *
 *      Prepares for reading from the socket, allocates new request struct
 *      for the given socket
 *
 * Results:
 *      None
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

static void
SockPrepare(Sock *sockPtr)
{
    Request *reqPtr;

    if (sockPtr->reqPtr != NULL) {
        return;
    }
    Ns_MutexLock(&reqLock);
    reqPtr = firstReqPtr;
    if (reqPtr != NULL) {
        firstReqPtr = reqPtr->nextPtr;
    }
    Ns_MutexUnlock(&reqLock);
    if (reqPtr == NULL) {
        reqPtr = ns_calloc(1, sizeof(Request));
        Tcl_DStringInit(&reqPtr->buffer);
        reqPtr->headers    = Ns_SetCreate(NULL);
        reqPtr->next       = NULL;
        reqPtr->content    = NULL;
        reqPtr->length     = 0;
        reqPtr->avail      = 0;
        reqPtr->coff       = 0;
        reqPtr->woff       = 0;
        reqPtr->roff       = 0;
        reqPtr->leadblanks = 0;
    }
    sockPtr->reqPtr = reqPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * SockQueue --
 *
 *      Puts socket into connection queue
 *
 * Results:
 *      NS_OK if queued,
 *      NS_ERROR if socket closed because of error
 *      NS_TIMEOUT if queue is full
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SockQueue(Sock *sockPtr, Ns_Time *timePtr)
{
    /*
     *  Verify the conditions, Request struct should exists already
     */

    if (!SockSetServer(sockPtr)) {
        SockRelease(sockPtr, SOCK_SERVERREJECT, 0);
        return NS_ERROR;
    }

    /*
     *  Actual queueing, if not ready spool to the waiting list
     */

    if (!NsQueueConn(sockPtr, timePtr)) {
        return NS_TIMEOUT;
    }

    return NS_OK;
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
SockPoll(Sock *sockPtr, int type, PollData *pdata)
{
    sockPtr->pidx = PollSet(pdata, sockPtr->sock, type, &sockPtr->timeout);
}

/*
 *----------------------------------------------------------------------
 *
 * SockTimeout --
 *
 *      Update socket with timeout
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Socket timeout will have nowPtr + timeout value
 *
 *----------------------------------------------------------------------
 */

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
 *      Accept and initialize a new Sock in sockPtrPtr.
 *
 * Results:
 *      SOCK_READY, SOCK_MORE, SOCK_SPOOL,
 *      SOCK_ERROR + NULL sockPtr.
 *
 * Side effects:
 *      Read-ahead may be attempted on new socket.
 *
 *----------------------------------------------------------------------
 */

static int
SockAccept(Driver *drvPtr, Sock **sockPtrPtr)
{
    Sock    *sockPtr;
    int      status;

    /*
     * Allocate and/or initialize a Sock structure.
     */

    Ns_MutexLock(&drvPtr->lock);
    sockPtr = drvPtr->sockPtr;
    if (sockPtr != NULL) {
        drvPtr->sockPtr = sockPtr->nextPtr;
    }
    Ns_MutexUnlock(&drvPtr->lock);

    if (sockPtr == NULL) {
        int sockSize = sizeof(Sock) + (nsconf.nextSlsId * sizeof(Ns_Callback *));
        sockPtr = ns_calloc(1, sockSize);
        sockPtr->drvPtr = drvPtr;
    } else {
        sockPtr->tfd    = 0;
        sockPtr->taddr  = 0;
        sockPtr->keep   = 0;
        sockPtr->arg    = NULL;
    }

    /*
     * Accept the new connection.
     */

    status = DriverAccept(sockPtr);

    if (status == NS_DRIVER_ACCEPT_ERROR) {
        status = SOCK_ERROR;

        Ns_MutexLock(&drvPtr->lock);
        sockPtr->nextPtr = drvPtr->sockPtr;
        drvPtr->sockPtr = sockPtr;
        Ns_MutexUnlock(&drvPtr->lock);
        sockPtr = NULL;

    } else {
        drvPtr->queuesize++;

        if (status == NS_DRIVER_ACCEPT_DATA) {

            /*
             * If there is already data present then read it without
             * polling if we're in async mode.
             */

            if (drvPtr->opts & NS_DRIVER_ASYNC) {
                status = SockRead(sockPtr, 0);
                if (status < 0) {
                    SockRelease(sockPtr, status, errno);
                    status = SOCK_ERROR;
                    sockPtr = NULL;
                }
            } else {

                /*
                 * Queue this socket without reading, NsGetRequest in
                 * the connection thread will perform actual reading of the request
                 */

                status = SOCK_READY;
            }
        } else
        if (status == NS_DRIVER_ACCEPT_QUEUE) {

            /*
             *  We need to call SockPrepare to make sure socket has request structure allocated,
             *  otherwise NsGetRequest will call SockRead which is not what this driver wants
             */

            SockPrepare(sockPtr);
            status = SOCK_READY;
        } else {
            status = SOCK_MORE;
        }
    }

    *sockPtrPtr = sockPtr;

    return status;
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
    Driver *drvPtr = sockPtr->drvPtr;

    SockError(sockPtr, reason, err);
    SockClose(sockPtr, 0);
    NsSlsCleanup(sockPtr);

    drvPtr->queuesize--;

    if (sockPtr->reqPtr != NULL) {
        NsFreeRequest(sockPtr->reqPtr);
        sockPtr->reqPtr = NULL;
    }

    Ns_MutexLock(&drvPtr->lock);
    sockPtr->nextPtr = drvPtr->sockPtr;
    drvPtr->sockPtr  = sockPtr;
    Ns_MutexUnlock(&drvPtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * SockError --
 *
 *      Log error message for given socket
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
SockError(Sock *sockPtr, int reason, int err)
{
    char   *errMsg = NULL;

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
        if (!sockPtr->keep) {
            errMsg = "Timeout during read";
        }
        break;

    case SOCK_SERVERREJECT:
        errMsg = "No Server found for request";
        break;

    case SOCK_READERROR:
        errMsg = "Unable to read request";
        break;

    case SOCK_WRITEERROR:
        errMsg = "Unable to write request";
        break;

    case SOCK_SHUTERROR:
        errMsg = "Unable to shutdown socket";
        break;

    case SOCK_BADREQUEST:
        errMsg = "Bad Request";
        SockSendResponse(sockPtr, 400, errMsg);
        break;

    case SOCK_TOOMANYHEADERS:
        errMsg = "Too Many Request Headers";
        SockSendResponse(sockPtr, 414, errMsg);
        break;

    case SOCK_BADHEADER:
        errMsg = "Invalid Request Header";
        SockSendResponse(sockPtr, 400, errMsg);
        break;

    case SOCK_ENTITYTOOLARGE:
        errMsg = "Request Entity Too Large";
        SockSendResponse(sockPtr, 413, errMsg);
        break;
    }
    if (errMsg != NULL) {
        Ns_Log(DriverDebug, "SockRelease: %s (%d: %s), sock: %d, peer: %s:%d, request: %.99s",
               errMsg,
               err,
               err ? strerror(err) : "",
               sockPtr->sock,
               ns_inet_ntoa(sockPtr->sa.sin_addr),
               ntohs(sockPtr->sa.sin_port),
               sockPtr->reqPtr ? sockPtr->reqPtr->buffer.string : "");
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SockSendResponse --
 *
 *      Send an HTTP response directly to the client using the
 *      driver callback.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      FIXME: This may block the driver thread.
 *
 *----------------------------------------------------------------------
 */

void
SockSendResponse(Sock *sockPtr, int code, char *msg)
{
    struct iovec iov[3];
    char header[32], *response = NULL;

    switch (code) {
    case 413:
        if (response == NULL) {
            response = "Bad Request";
        }
        break;
    case 414:
        if (response == NULL) {
            response = "Request-URI Too Long";
        }
        break;
    case 400:
    default:
        if (response == NULL) {
            response = "Bad Request";
        }
        break;
    }
    snprintf(header, sizeof(header),"HTTP/1.0 %d ", code);
    iov[0].iov_base = header;
    iov[0].iov_len = strlen(header);
    iov[1].iov_base = response;
    iov[1].iov_len = strlen(response);
    iov[2].iov_base = "\r\n\r\n";
    iov[2].iov_len = 4;
    NsDriverSend(sockPtr, iov, 3, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * SockTrigger --
 *
 *      Wakeup DriversThread from blocking ns_poll().
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
SockTrigger(NS_SOCKET fd)
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
    if (keep) {
        keep = DriverKeep(sockPtr);
    }
    if (!keep) {
        DriverClose(sockPtr);
    }
    sockPtr->keep = keep;

    /*
     * Unconditionally remove temporaty file, connection thread
     * should take care about very large uploads
     */

    if (sockPtr->tfile != NULL) {
#ifndef _WIN32
        unlink(sockPtr->tfile);
#else
        DeleteFile(sockPtr->tfile);
#endif
        ns_free(sockPtr->tfile);
        sockPtr->tfile = NULL;
    }

    /*
     * Close and unmmap temp file used for large content
     */

    if (sockPtr->tfd > 0) {
        close(sockPtr->tfd);
    }
    sockPtr->tfd = 0;

#ifndef _WIN32
    if (sockPtr->taddr != NULL) {
        munmap(sockPtr->taddr, (size_t)sockPtr->tsize);
    }
    sockPtr->taddr = 0;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * ChunkedDecode --
 *
 *      Reads the content form the incoming request buffer and tries
 *      to decode chunked encoding parts. The function can be called
 *      repeatetly and with incomplete input and overwrites the buffer
 *      with the decoded data optionally. The decoded data is always
 *      shorter then the encoded one.
 *
 * Results:
 *      1 on success, -1 on incomplete data
 *
 * Side effects:
 *      updates the buffer if update == 1 (and adjusts reqPtr->chunkWriteOff)
 *      updates always reqPtr->chunkStartOff to allow incremental operations
 *
 *----------------------------------------------------------------------
 */
static int 
ChunkedDecode(Request *reqPtr, int update)
{
    Tcl_DString *bufPtr = &reqPtr->buffer;
    long chunk_length;
    char 
      *end = bufPtr->string + bufPtr->length, 
      *chunkStart = bufPtr->string + reqPtr->chunkStartOff;

    while (reqPtr->chunkStartOff <  bufPtr->length) {
      char *p = strstr(chunkStart, "\r\n");
      if (!p) {
        Ns_Log(DriverDebug, "ChunkedDecode: chunk did not find end-of-line");
        return -1;
      }

      *p = '\0';
      chunk_length = strtol(chunkStart, NULL, 16);
      *p = '\r';

      if (p + 2 + chunk_length > end) {
        Ns_Log(DriverDebug,"ChunkedDecode: chunk length past end of buffer");
        return -1;
      }
      if (update) {
        char *writeBuffer = bufPtr->string + reqPtr->chunkWriteOff;
        memmove(writeBuffer, p + 2, chunk_length);
        reqPtr->chunkWriteOff += chunk_length;
        *(writeBuffer + chunk_length) = '\0';
      }
      reqPtr->chunkStartOff += (p - chunkStart) + 4 + chunk_length ;
      chunkStart = bufPtr->string + reqPtr->chunkStartOff;
    }

    return 1;
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
 *      SOCK_SPOOL: Pass input handling to spooler
 *      SOCK_BADREQUEST
 *      SOCK_BADHEADER
 *      SOCK_TOOMANYHEADERS
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
    Driver       *drvPtr = sockPtr->drvPtr;
    DrvSpooler   *spPtr  = &drvPtr->spooler;
    Request      *reqPtr = NULL;
    Tcl_DString  *bufPtr = NULL;

    struct iovec  buf;
    char         tbuf[4096];
    size_t       len, nread;
    ssize_t      n;

    /*
     * Initialize Request structure
     */

    SockPrepare(sockPtr);

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
      n = (size_t)drvPtr->maxinput;
        nread = n - len;
        if (nread == 0) {
            Ns_Log(DriverDebug, "SockRead: maxinput reached %" TCL_LL_MODIFIER "d",
                   drvPtr->maxinput);
            return SOCK_ERROR;
        }
    }

    /*
     * Use temp file for content larger than readahead bytes.
     */

#ifndef _WIN32
    if (reqPtr->coff > 0 &&
        !reqPtr->chunkStartOff /* never spool chunked encoded data since we decode in memory */ &&
        reqPtr->length > drvPtr->readahead && sockPtr->tfd <= 0) {

        /*
         * In driver mode send this Sock to the spooler thread if
         * it is running
         */

        if (spooler == 0 && spPtr->threads > 0) {
            return SOCK_SPOOL;
        }

        /*
         * In spooler mode dump data into temp file, if maxupload is specified
         * we will spool raw uploads into normal temp file (no deleted) in case
         * content size exceeds the configured value.
         */

        if (drvPtr->maxupload > 0 && reqPtr->length > drvPtr->maxupload) {
            sockPtr->tfile = ns_malloc(strlen(drvPtr->uploadpath) + 16);
            sprintf(sockPtr->tfile, "%s%d.XXXXXX", drvPtr->uploadpath, sockPtr->sock);
            mktemp(sockPtr->tfile);
            sockPtr->tfd = open(sockPtr->tfile, O_RDWR|O_CREAT|O_TRUNC|O_EXCL, 0600);
        } else {
            sockPtr->tfd = Ns_GetTemp();
        }

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
        Tcl_DStringSetLength(bufPtr, (int)(len + nread));
        buf.iov_base = bufPtr->string + reqPtr->woff;
        buf.iov_len = nread;
    }

    n = DriverRecv(sockPtr, &buf, 1);

    if (n < 0 || (n == 0 && !reqPtr->request.line)) {
        return SOCK_READERROR;
    }
    
    if (sockPtr->tfd > 0) {
        if (write(sockPtr->tfd, tbuf, n) != n) {
            return SOCK_WRITEERROR;
        }
    } else {
      Tcl_DStringSetLength(bufPtr, (int)(len + n));
    }

    reqPtr->woff  += (off_t)n;
    reqPtr->avail += n;

    /*
     * This driver needs raw buffer, it is binary or non-HTTP request
     */

    if (drvPtr->opts & NS_DRIVER_NOPARSE) {
        return SOCK_READY;
    }

    n = SockParse(sockPtr, spooler);

    return (int)n;
}

/*----------------------------------------------------------------------
 *
 * SockParse --
 *
 *      Construct the given conn by parsing input buffer until end of
 *      headers.  Return NS_SOCK_READY when finnished parsing.
 *
 * Results:
 *      SOCK_READY:  Conn is ready for processing.
 *      SOCK_MORE:   More input is required.
 *      SOCK_ERROR:  Malformed request.
 *      SOCK_BADREQUEST
 *      SOCK_BADHEADER
 *      SOCK_TOOMANYHEADERS
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

    NsUpdateProgress((Ns_Sock *) sockPtr);

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
         * Check for max single line overflows. 
	 *
	 * Previous versions if the driver returned here directly an
         * error code, which was handled via http error message
         * provided via SockError(). However, the SockError() handling
         * closes the connection immediately. This has the
         * consequence, that the http client might never see the error
         * message, since the request was not yet fully transmitted,
         * but it will see a "broken pipe: 13" message instead. We
         * read now the full request and return the message via
         * ConnRunRequest().
         */

        if ((e - s) > drvPtr->maxline) {
	    sockPtr->keep = 0;
            if (reqPtr->request.line == NULL) {
                Ns_Log(DriverDebug, "SockParse: maxline reached of %d bytes",
                       drvPtr->maxline);
		sockPtr->flags = NS_CONN_REQUESTURITOOLONG;
            } else {
	      sockPtr->flags = NS_CONN_LINETOOLONG;
	    }
        }

        /*
         * Update next read pointer to end of this line.
         */

        cnt = (int)(e - s) + 1;
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
            reqPtr->chunkStartOff = 0;

            s = Ns_SetIGet(reqPtr->headers, "content-length");
            if (s == NULL) {
                s = Ns_SetIGet(reqPtr->headers, "Transfer-Encoding");

                if (s != NULL) {
                    /* Lower case is in the standard, capitalized by Mac OS X */
                    if (strcmp(s,"chunked") == 0 || strcmp(s,"Chunked") == 0 ) {
                        Tcl_WideInt expected;
                        
                        reqPtr->chunkStartOff = reqPtr->coff;
                        reqPtr->chunkWriteOff = reqPtr->chunkStartOff;
                        reqPtr->contentLength = 0;

                        /* We need expectedLength for safely terminating read loop */

                        s = Ns_SetIGet(reqPtr->headers, "X-Expected-Entity-Length");

                        if (s && Ns_StrToWideInt(s, &expected) == NS_OK && expected > 0) {
                            reqPtr->expectedLength = expected;
                        }
                        s = NULL;
                    } 
                }
            }

            if (s != NULL) {
                Tcl_WideInt length;

                /*
                 * Honor meaningful remote content-length hints only.
                 */

                if (Ns_StrToWideInt(s, &length) == NS_OK && length > 0) {
		    reqPtr->length = (size_t)length;
		    /*
		     * Handle too large input requests
		     */
                    if (reqPtr->length > (size_t)drvPtr->maxinput) {
                        Ns_Log(DriverDebug, "SockParse: request too large, length=%"
                                            TCL_LL_MODIFIER "d, maxinput=%" TCL_LL_MODIFIER "d",
                               reqPtr->length, drvPtr->maxinput);
			/* 
			 * We have to read the full request (although
			 * it is too large) to drain the
			 * channel. Otherwise, the server might close
			 * the connection *before* it has recevied
			 * full request with its body. Such a
			 * premature close leads to an error message
			 * in clients like firefox. Therefore we do
			 * not return SOCK_ENTITYTOOLARGE here, but
			 * just flag the condition. ...
			 *
			 * Possible future improvements: Currently,
			 * the content is really received and kept. We
			 * might simply drain the input and ignore the
			 * read content, but this requires handling
			 * for the various input modes (spooling,
			 * chunked content, etc.). We should make the
			 * same for the other reply-codes in
			 * SockError().
			 */
			sockPtr->flags = NS_CONN_ENTITYTOOLARGE;
			sockPtr->keep = 0;
			
                    }
                    reqPtr->contentLength = (size_t)length;
                }

            }
        } else {
            save = *e;
            *e = '\0';

            if (reqPtr->request.line == NULL) {
                if (Ns_ParseRequest(&reqPtr->request, s) == NS_ERROR) {

                    /*
                     * Invalid request.
                     */

                    return SOCK_BADREQUEST;
                }

            } else if (Ns_ParseHeader(reqPtr->headers, s, Preserve) != NS_OK) {

                /*
                 * Invalid header.
                 */

                return SOCK_BADHEADER;
            }

            /*
             * Check for max number of headers
             */

            if (Ns_SetSize(reqPtr->headers) > drvPtr->maxheaders) {
                Ns_Log(DriverDebug, "SockParse: maxheaders reached of %d bytes",
                       drvPtr->maxheaders);
                return SOCK_TOOMANYHEADERS;
            }

            *e = save;
            if (reqPtr->request.version <= 0.0) {

                /*
                 * Pre-HTTP/1.0 request.
                 */

                reqPtr->coff = reqPtr->roff;
            }
        }
    }

    /*
     * Set up request length for spooling and further read operations
     */
    if (reqPtr->contentLength) {
      /* 
       * Content-Length was provided, use it 
       */
      reqPtr->length = reqPtr->contentLength;
    }

    /*
     * Check if all content has arrived.
     */

    if (reqPtr->chunkStartOff) {
        /* Chunked encoding was provided */
        int complete;
        Tcl_WideInt currentContentLength;

        complete = ChunkedDecode(reqPtr, 1);
        currentContentLength = reqPtr->chunkWriteOff - reqPtr->coff;

        /* 
         * A chunk might be complete, but it might not be the last
         * chunk from the client. The best thing would be to be able
         * to read until eof here. In cases, where the (optional)
         * expectedLength was provided by the client, we terminate
         * depending on that information
         */
        if (!complete 
            || (reqPtr->expectedLength && currentContentLength < reqPtr->expectedLength)) {
            /* ChunkedDecode wants more data */
            return SOCK_MORE;
        }
        /* ChunkedDecode has enough data */
        reqPtr->length = (size_t)currentContentLength;
    }

    if (reqPtr->coff > 0 && reqPtr->length <= reqPtr->avail) {

        /*
         * With very large uploads we have to put them into regular temporary file
         * and make it available to the connection thread. No parsing of the request
         * will be performed by the server.
         */

        if (sockPtr->tfile != NULL) {
            reqPtr->content = NULL;
            reqPtr->next = NULL;
            reqPtr->avail = 0;
            Ns_Log(Debug, "spooling content to file: size=%" TCL_LL_MODIFIER "d, file=%s",
                   reqPtr->length, sockPtr->tfile);

            /*
             * To make huge uploads easy to handle, we put query into content and change method to GET so
             * Ns_ConnGetQuery will parse it and return as query parameters. If later the conn Tcl page
             * will decide to parse multipart/data file manually it may replace query with new parsed data
             * but in case of batch processing with external tools it is good to know additional info
             * about the uploaded content beforehand.
             */

            if (reqPtr->request.query != NULL) {
                Tcl_DStringSetLength(bufPtr, 0);
                Tcl_DStringAppend(bufPtr, reqPtr->request.query, -1);
                ns_free(reqPtr->request.method);
                reqPtr->request.method = ns_strdup("GET");
                reqPtr->content = bufPtr->string;
            }

            return (reqPtr->request.line != NULL ? SOCK_READY : SOCK_ERROR);
        }

        if (sockPtr->tfd > 0) {
#ifndef _WIN32
            int prot = PROT_READ | PROT_WRITE;
	    /* 
	     * Add a byte to make sure, the \0 assignment below falls
	     * always into the mmapped area. Might lead to crashes
	     * when we hitting page boundaries.
	     */
	    write(sockPtr->tfd, "\0", 1); 
            sockPtr->tsize = reqPtr->length + 1;
            sockPtr->taddr = mmap(0, sockPtr->tsize, prot, MAP_PRIVATE,
                                  sockPtr->tfd, 0);
            if (sockPtr->taddr == MAP_FAILED) {
                sockPtr->taddr = NULL;
                return SOCK_ERROR;
            }
            reqPtr->content = sockPtr->taddr;
            Ns_Log(Debug, "spooling content to file: readahead=%ld, filesize=%i",
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

        return (reqPtr->request.line != NULL ? SOCK_READY : SOCK_ERROR);
    }

    /*
     * Wait for more input.
     */

    return SOCK_MORE;
}

/*
 *----------------------------------------------------------------------
 *
 * SockSetServer --
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
SockSetServer(Sock *sockPtr)
{
    ServerMap     *mapPtr = NULL;
    Tcl_HashEntry *hPtr;
    char          *host = NULL;
    int            status = 1;

    sockPtr->servPtr  = sockPtr->drvPtr->servPtr;
    sockPtr->location = sockPtr->drvPtr->location;

    if (sockPtr->reqPtr != NULL) {
        host = Ns_SetIGet(sockPtr->reqPtr->headers, "Host");
        if (!host && sockPtr->reqPtr->request.version >= 1.1) {
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
        ns_free(sockPtr->reqPtr->request.method);
        sockPtr->reqPtr->request.method = ns_strdup("BAD");
    }

    return 1;
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
    Ns_Time        now, diff;
    Driver        *drvPtr;
    PollData       pdata;

    Ns_ThreadSetName("-spooler%d-", queuePtr->id);

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "spooler%d: accepting connections", queuePtr->id);

    PollCreate(&pdata);
    Ns_GetTime(&now);
    waitPtr = readPtr = NULL;
    stopping = 0;

    while (!stopping) {

        /*
         * If there are any read sockets, set the bits
         * and determine the minimum relative timeout.
         */

        PollReset(&pdata);
        PollSet(&pdata, queuePtr->pipe[0], POLLIN, NULL);

        if (readPtr == NULL) {
            pollto = 30 * 1000;
        } else {
            sockPtr = readPtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, POLLIN, &pdata);
                sockPtr = sockPtr->nextPtr;
            }
	    pollto = -1;
        }

        /*
         * Select and drain the trigger pipe if necessary.
         */

        n = PollWait(&pdata, pollto);

        if (PollIn(&pdata, 0) && recv(queuePtr->pipe[0], &c, 1, 0) != 1) {
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
            if (PollHup(&pdata, sockPtr->pidx)) {
		/*
		 * Peer has closed the connection
		 */
		SockRelease(sockPtr, SOCK_CLOSE, 0);

            } else if (PollIn(&pdata, sockPtr->pidx) == 0) {
	        /*
	         * Got no data
	         */
                if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                    queuePtr->queuesize--;
                } else {
                    Push(sockPtr, readPtr);
                }
            } else {
	        /*
		 * Got some data
		 */
                n = SockRead(sockPtr, 1);
                switch (n) {
                case SOCK_MORE:
                    SockTimeout(sockPtr, &now, drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;

                case SOCK_READY:
                    if (SockSetServer(sockPtr) == 0) {
                        SockRelease(sockPtr, SOCK_SERVERREJECT, 0);
                        queuePtr->queuesize--;
                    } else {
                        Push(sockPtr, waitPtr);
                    }
                    break;

                default:
                    SockRelease(sockPtr, n, errno);
                    queuePtr->queuesize--;
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
                if (NsQueueConn(sockPtr, &now) == 0) {
                    Push(sockPtr, waitPtr);
                } else {
                    queuePtr->queuesize--;
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
                queuePtr->queuesize++;
                sockPtr = nextPtr;
            }
        }

        /*
         * Check for shutdown
         */

        stopping = queuePtr->shutdown;
        Ns_MutexUnlock(&queuePtr->lock);
    }
    PollFree(&pdata);

    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = 1;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

static void
SpoolerQueueStart(SpoolerQueue *queuePtr, Ns_ThreadProc *proc)
{
    while (queuePtr != NULL) {
        if (ns_sockpair(queuePtr->pipe)) {
            Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
        }
        Ns_ThreadCreate(proc, queuePtr, 0, &queuePtr->thread);
        queuePtr = queuePtr->nextPtr;
    }
}

static void
SpoolerQueueStop(SpoolerQueue *queuePtr, Ns_Time *timeoutPtr)
{
    int status;

    while (queuePtr != NULL) {
        Ns_MutexLock(&queuePtr->lock);
        if (!queuePtr->stopped && !queuePtr->shutdown) {
            Ns_Log(Notice, "%d: triggering shutdown", queuePtr->id);
            queuePtr->shutdown = 1;
            SockTrigger(queuePtr->pipe[1]);
        }
        status = NS_OK;
        while (!queuePtr->stopped && status == NS_OK) {
            status = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, timeoutPtr);
        }
        if (status != NS_OK) {
            Ns_Log(Warning, "%d: timeout waiting for shutdown", queuePtr->id);
        } else {
            Ns_Log(Notice, "%d: shutdown complete", queuePtr->id);
            Ns_ThreadJoin(&queuePtr->thread, NULL);
            queuePtr->thread = NULL;
            ns_sockclose(queuePtr->pipe[0]);
            ns_sockclose(queuePtr->pipe[1]);
        }
        Ns_MutexUnlock(&queuePtr->lock);
        queuePtr = queuePtr->nextPtr;
    }
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

    Ns_Log(Debug, "Spooler: %d: started fd=%d: %" TCL_LL_MODIFIER "d bytes",
           queuePtr->id, sockPtr->sock, sockPtr->reqPtr->length);

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = 1;
    }
    Push(sockPtr, queuePtr->sockPtr);
    Ns_MutexUnlock(&queuePtr->lock);

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
    SpoolerQueue   *queuePtr = (SpoolerQueue*)arg;
    unsigned char   c, *bufPtr;
    int             n, err, stopping, pollto, status;
    Tcl_WideInt     toread, maxsize;
    Ns_Time         now;
    Sock           *sockPtr;
    Driver         *drvPtr;
    DrvWriter      *wrPtr;
    WriterSock     *curPtr, *nextPtr, *writePtr;
    PollData        pdata;
    struct iovec    vbuf;


    Ns_ThreadSetName("-writer%d-", queuePtr->id);

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    Ns_Log(Notice, "writer%d: accepting connections", queuePtr->id);

    PollCreate(&pdata);
    Ns_GetTime(&now);
    writePtr = NULL;
    stopping = 0;

    while (!stopping) {

        /*
         * If there are any read sockets, set the bits
         * and determine the minimum relative timeout.
         */

        PollReset(&pdata);
        PollSet(&pdata, queuePtr->pipe[0], POLLIN, NULL);

        if (writePtr == NULL) {
            pollto = 30 * 1000;
        } else {
            curPtr = writePtr;
            while (curPtr != NULL) {
                if (curPtr->size > 0) {
                    SockPoll(curPtr->sockPtr, POLLOUT, &pdata);
                }
                curPtr = curPtr->nextPtr;
            }
	    pollto = -1;
        }

        /*
         * Select and drain the trigger pipe if necessary.
         */
        n = PollWait(&pdata, pollto);

        if (PollIn(&pdata, 0) && recv(queuePtr->pipe[0], &c, 1, 0) != 1) {
            Ns_Fatal("writer: trigger recv() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }

        /*
         * Write to all available sockets
         */

        Ns_GetTime(&now);
        curPtr = writePtr;
        writePtr = NULL;

        while (curPtr != NULL) {

            nextPtr = curPtr->nextPtr;

            sockPtr = curPtr->sockPtr;
            drvPtr  = sockPtr->drvPtr;
            wrPtr   = &drvPtr->writer;
            err = status = NS_OK;

            if (PollOut(&pdata, sockPtr->pidx) && curPtr->size > 0 ) {

                /*
                 * Read block from the file and send it to the socket
                 */

                if (curPtr->fd > -1) {
                    maxsize = wrPtr->bufsize;
                    toread = curPtr->nread;
                    bufPtr = curPtr->buf;

                    /*
                     *  Case when bufsize > 0 means that we have leftover
                     *  from previous send, fill up the rest of the buffer
                     *  and retransmit it with new portion from the file
                     */

                    if (curPtr->bufsize > 0) {
                        bufPtr = curPtr->buf + (sizeof(curPtr->buf) - curPtr->bufsize);
                        memmove(curPtr->buf, bufPtr, curPtr->bufsize);
                        bufPtr = curPtr->buf + curPtr->bufsize;
                        maxsize -= curPtr->bufsize;
                    }
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
                    vbuf.iov_len = curPtr->bufsize;
                    vbuf.iov_base = (void *) curPtr->buf;
                    n = (int)NsDriverSend(curPtr->sockPtr, &vbuf, 1, 0);
                    if (n < 0) {
                        err = errno;
                        status = NS_ERROR;
                    } else {
                        curPtr->size -= n;
                        curPtr->nsent += n;
                        curPtr->bufsize -= n;
                        sockPtr->timeout.sec = 0;
                        if (curPtr->data) {
                            curPtr->buf += n;
                        }
                    }
                }
            } else {

                /*
                 *  Mark when first timeout occured or check if it is already
                 *  for too long and we need to stop this socket
                 */

                if (sockPtr->timeout.sec == 0) {
                    SockTimeout(sockPtr, &now, curPtr->sockPtr->drvPtr->sendwait);
                } else {
                    if (Ns_DiffTime(&sockPtr->timeout, &now, NULL) <= 0) {
                        err = ETIMEDOUT;
                        status = NS_ERROR;
                    }
                }
            }

            /*
             * Check result status and close the socket in case of
             * timeout or completion
             */

            if (status != NS_OK) {
                SockWriterRelease(curPtr, SOCK_WRITEERROR, err);
                queuePtr->queuesize--;
            } else {
                if (curPtr->size > 0) {
                    Push(curPtr, writePtr);
                } else {
                    SockWriterRelease(curPtr, 0, 0);
                    queuePtr->queuesize--;
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
            queuePtr->queuesize++;
            curPtr = nextPtr;
        }
        queuePtr->curPtr = writePtr;

        /*
         * Check for shutdown
         */

        stopping = queuePtr->shutdown;
        Ns_MutexUnlock(&queuePtr->lock);
    }
    PollFree(&pdata);

    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = 1;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

static void
SockWriterRelease(WriterSock *wrSockPtr, int reason, int err)
{
    Ns_Log(Debug, "Writer: closed sock=%d, fd=%d, error=%d/%d, "
           "sent=%" TCL_LL_MODIFIER "d, flags=%X",
           wrSockPtr->sockPtr->sock, wrSockPtr->fd, reason, err,
           wrSockPtr->nsent, wrSockPtr->flags);

    if (err || reason) {
        SockRelease(wrSockPtr->sockPtr, reason, err);
    } else {
        NsSockClose(wrSockPtr->sockPtr, wrSockPtr->keep);
    }

    if (wrSockPtr->fd > -1) {
        close(wrSockPtr->fd);
        ns_free(wrSockPtr->buf);
    }
    ns_free(wrSockPtr->data);
    ns_free(wrSockPtr);
}

int
NsWriterQueue(Ns_Conn *conn, size_t nsend, Tcl_Channel chan, FILE *fp, int fd,
              const char *data)
{
    Conn          *connPtr = (Conn*)conn;
    WriterSock    *wrSockPtr;
    SpoolerQueue  *queuePtr;
    Driver        *drvPtr;
    DrvWriter     *wrPtr;
    int            trigger = 0;

    if (conn == NULL) {
        return NS_ERROR;
    }

    drvPtr = connPtr->sockPtr->drvPtr;
    wrPtr  = &drvPtr->writer;

    if (wrPtr->threads == 0) {
        Ns_Log(DriverDebug, "NsWriterQueue: no writer threads configured");
        return NS_ERROR;
    }

    if (nsend < (size_t)wrPtr->maxsize) {
        Ns_Log(DriverDebug, "NsWriterQueue: file is too small(%"
                            TCL_LL_MODIFIER "d < %d)",
               nsend, wrPtr->maxsize);
        return NS_ERROR;
    }


    wrSockPtr = (WriterSock*)ns_calloc(1, sizeof(WriterSock));
    wrSockPtr->sockPtr = connPtr->sockPtr;
    wrSockPtr->sockPtr->timeout.sec = 0;
    wrSockPtr->flags = connPtr->flags;
    wrSockPtr->fd = -1;

    if (chan != NULL) {
        if (Tcl_GetChannelHandle(chan, TCL_READABLE,
                                 (ClientData)&wrSockPtr->fd) != TCL_OK) {
            ns_free(wrSockPtr);
            return NS_ERROR;
        }
    } else if (fp != NULL) {
        wrSockPtr->fd = fileno(fp);
    } else if (fd != -1) {
        wrSockPtr->fd = fd;
    } else if (data != NULL ) {
        wrSockPtr->data = ns_malloc(nsend + 1);
        memcpy(wrSockPtr->data, data, nsend);
    } else {
        ns_free(wrSockPtr);
        return NS_ERROR;
    }
    if (wrSockPtr->fd > -1) {
        wrSockPtr->fd  = ns_sockdup(wrSockPtr->fd);
        wrSockPtr->buf = ns_malloc(wrPtr->bufsize);
    }
    if (wrSockPtr->data) {
        wrSockPtr->buf = (unsigned char*)wrSockPtr->data;
        wrSockPtr->bufsize = nsend;
    }

    /*
     * Make sure we have proper content length header for keep-alives
     */

    connPtr->responseLength = nsend;

    /*
     * Flush the headers
     */

    Ns_ConnSetLengthHeader(conn, nsend);
    Ns_ConnWriteData(conn, NULL, 0, 0);

    wrSockPtr->keep = connPtr->keep > 0 ? 1 : 0;
    wrSockPtr->size = nsend;
    wrSockPtr->nread = nsend;
    connPtr->sockPtr = NULL;

    /* To keep ns_log happy about content size returned */
    connPtr->nContentSent = nsend;
    connPtr->flags |= NS_CONN_SENT_VIA_WRITER;

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

    Ns_Log(Debug, "Writer: %d: started sock=%d, fd=%d: "
           "size=%" TCL_LL_MODIFIER "d, flags=%X: keep=%d, %s",
           queuePtr->id, wrSockPtr->sockPtr->sock, wrSockPtr->fd,
           nsend, wrSockPtr->flags, wrSockPtr->keep, connPtr->reqPtr->request.url);

    /*
     * Now add new writer socket to the writer thread's queue
     */

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = 1;
    }
    Push(wrSockPtr, queuePtr->sockPtr);
    Ns_MutexUnlock(&queuePtr->lock);

    /*
     * Wake up writer thread
     */

    if (trigger) {
        SockTrigger(queuePtr->pipe[1]);
    }

    return NS_OK;
}

int
NsTclWriterObjCmd(ClientData arg, Tcl_Interp *interp, int objc,
                  Tcl_Obj *CONST objv[])
{
    int           fd, opt, rc;
    Tcl_DString   ds;
    Ns_Conn      *conn;
    Driver       *drvPtr;
    DrvWriter    *wrPtr;
    WriterSock   *wrSockPtr;
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
    conn = Ns_GetConn();

    switch (opt) {
    case cmdSubmitIdx: {
        int size;
        char *data;

        if (objc < 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "data");
            return TCL_ERROR;
        }
        if (conn == NULL) {
            Tcl_AppendResult(interp, "no connection", NULL);
            return TCL_ERROR;
        }
        data = (char*)Tcl_GetByteArrayFromObj(objv[2], &size);
        if (data) {
            rc = NsWriterQueue(conn, size, NULL, NULL, -1, data);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
        }
        break;
    }

    case cmdSubmitFileIdx: {
        char *name;
        struct stat st;
        Tcl_Obj *file = NULL;
        Tcl_WideInt headers = 0, offset = 0, size = 0;

        Ns_ObjvSpec opts[] = {
            {"-headers",  Ns_ObjvBool,    &headers, (void *) NS_TRUE},
            {"-offset",   Ns_ObjvWideInt, &offset,  NULL},
            {"-size",     Ns_ObjvWideInt, &size,    NULL},
            {NULL, NULL, NULL, NULL}
        };
        Ns_ObjvSpec args[] = {
            {"file",      Ns_ObjvObj,     &file,    NULL},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }

        if (conn == NULL) {
            Tcl_AppendResult(interp, "no connection", NULL);
            return TCL_ERROR;
        }

        name = Tcl_GetString(file);

        if (size <= 0) {
            rc = stat(name, &st);
            if (rc != NS_OK) {
                Tcl_AppendResult(interp, "stat failed for ", name, NULL);
                return TCL_ERROR;
            }
            size = st.st_size;
        }

        fd = open(name, O_RDONLY);
        if (fd == -1) {
            return TCL_ERROR;
        }

        if (offset > 0) {
	  lseek(fd, (off_t)offset, SEEK_SET);
        }

        /*
         *  The caller requested that we build required headers
         */

        if (headers) {
            Ns_ConnSetTypeHeader(conn, Ns_GetMimeType(name));
        }

        rc = NsWriterQueue(conn, (size_t)size, NULL, NULL, fd, NULL);

        Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
        close(fd);

        break;
    }

    case cmdListIdx:
        Tcl_DStringInit(&ds);
        drvPtr = firstDrvPtr;
        while (drvPtr != NULL) {
            wrPtr = &drvPtr->writer;
            queuePtr = wrPtr->firstPtr;
            while (queuePtr != NULL) {
                Ns_MutexLock(&queuePtr->lock);
                wrSockPtr = queuePtr->curPtr;
                while (wrSockPtr != NULL) {
                    Ns_DStringPrintf(&ds, "%s %s %d "
                                     "%" TCL_LL_MODIFIER "d %" TCL_LL_MODIFIER "d ",
                                     drvPtr->name,
                                     ns_inet_ntoa(wrSockPtr->sockPtr->sa.sin_addr),
                                     wrSockPtr->fd, wrSockPtr->size, wrSockPtr->nsent);
                    wrSockPtr = wrSockPtr->nextPtr;
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
