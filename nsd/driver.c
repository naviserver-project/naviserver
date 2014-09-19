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
 * Managing streaming output via writer
 */
#define NS_WRITER_STREAM_NONE         0
#define NS_WRITER_STREAM_ACTIVE       1
#define NS_WRITER_STREAM_FINISH       2

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
 * Async writer definitons
 */

typedef struct AsyncWriter {
    Ns_Mutex lock;             /* Lock around writer queues */
    SpoolerQueue *firstPtr;    /* List of writer threads */
} AsyncWriter;

/* AsyncWriteData is similar to WriterSock */
typedef struct AsyncWriteData {
    struct AsyncWriteData *nextPtr;
    char              *data;
    int                fd;
    int                pidx;
    Tcl_WideInt        nsent;
    size_t             size;
    size_t             bufsize;
    char              *buf;
} AsyncWriteData;

static AsyncWriter *asyncWriter = NULL;


/*
 * Static functions defined in this file.
 */

static Ns_ThreadProc DriverThread;
static Ns_ThreadProc SpoolerThread;
static Ns_ThreadProc WriterThread;
static Ns_ThreadProc AsyncWriterThread;

static NS_SOCKET DriverListen(Driver *drvPtr);
static NS_DRIVER_ACCEPT_STATUS DriverAccept(Sock *sockPtr);
static ssize_t DriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs);
static int     DriverKeep(Sock *sockPtr);
static void    DriverClose(Sock *sockPtr);

static int   SockSetServer(Sock *sockPtr);
static int   SockAccept(Driver *drvPtr, Sock **sockPtrPtr, Ns_Time *nowPtr);
static int   SockQueue(Sock *sockPtr, Ns_Time *timePtr);
static void  SockPrepare(Sock *sockPtr);
static void  SockRelease(Sock *sockPtr, int reason, int err);
static void  SockError(Sock *sockPtr, int reason, int err);
static void  SockSendResponse(Sock *sockPtr, int code, char *msg);
static void  SockTrigger(NS_SOCKET sock);
static void  SockTimeout(Sock *sockPtr, Ns_Time *nowPtr, int timeout);
static void  SockClose(Sock *sockPtr, int keep);
static int   SockRead(Sock *sockPtr, int spooler, Ns_Time *timePtr);
static int   SockParse(Sock *sockPtr, int spooler);
static void  SockPoll(Sock *sockPtr, int type, PollData *pdata);
static int   SockSpoolerQueue(Driver *drvPtr, Sock *sockPtr);
static void  SpoolerQueueStart(SpoolerQueue *queuePtr, Ns_ThreadProc *proc);
static void  SpoolerQueueStop(SpoolerQueue *queuePtr, Ns_Time *timeoutPtr, CONST char *name);
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
static ServerMap *defMapPtr   = NULL; /* Default srv when not found in table */
static Ns_Mutex   reqLock     = NULL; /* Lock for request free list */
static Ns_Mutex   writerlock  = NULL; /* Lock updating streamin information in the writer */
static Request   *firstReqPtr = NULL; /* Free list of request structures */
static Driver    *firstDrvPtr = NULL; /* First in list of all drivers */

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
    Ns_MutexInit(&reqLock);
    Ns_MutexSetName2(&reqLock, "ns:driver","freelist");
    Ns_MutexSetName2(&writerlock, "ns:writer","stream");
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
    int             i, n, defport, noHostNameGiven;
    ServerMap      *mapPtr;
    Ns_DString      ds, *dsPtr = &ds;
    struct in_addr  ia;
    struct hostent *he;
    Driver         *drvPtr;
    DrvWriter      *wrPtr;
    DrvSpooler     *spPtr;
    NsServer       *servPtr = NULL;
    Ns_Set         *set;

    if (server != NULL && (servPtr = NsGetServer(server)) == NULL) {
        return NS_ERROR;
    }

    if (init->version != NS_DRIVER_VERSION_2) {
        Ns_Log(Error, "%s: version field of init argument is invalid: %d",
               module, init->version);
        return NS_ERROR;
    }

    path = (init->path ? init->path : Ns_ConfigGetPath(server, module, NULL));
    set = Ns_ConfigCreateSection(path);

    /*
     * Determine the hostname used for the local address to bind
     * to and/or the HTTP location string.
     */

    host = Ns_ConfigGetValue(path, "hostname");
    noHostNameGiven = (host == NULL);
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
            Ns_Log(Error, "%s: no addresses for %s", module, he->h_name);
            return NS_ERROR;
        }

        memcpy(&ia.s_addr, he->h_addr_list[0], sizeof(ia.s_addr));
        address = ns_inet_ntoa(ia);

	if (address && path) {
	    Ns_SetUpdate(set, "address", address);
	}

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

    if (noHostNameGiven && host && path) {
	Ns_SetUpdate(set, "hostname", host);
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
    Ns_MutexInit(&drvPtr->lock);
    Ns_MutexSetName2(&drvPtr->lock, "ns:drv", module);

    if (ns_sockpair(drvPtr->trigger) != 0) {
        Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
    }

    drvPtr->server       = server;
    drvPtr->module       = ns_strdup(module);
    /*drvPtr->name         = init->name;*/
    drvPtr->name         = drvPtr->module;
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

    drvPtr->uploadpath = ns_strdup(Ns_ConfigString(path, "uploadpath", nsconf.tmpDir));

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
        Ns_DStringInit(dsPtr);
        Ns_DStringVarAppend(dsPtr, drvPtr->protocol, "://", host, NULL);
        if (drvPtr->port != defport) {
            Ns_DStringPrintf(dsPtr, ":%d", drvPtr->port);
        }
        drvPtr->location = Ns_DStringExport(dsPtr);
    }

    drvPtr->nextPtr = firstDrvPtr;
    firstDrvPtr = drvPtr;

    /*
     * Add extra headers, which have to be of the form of
     * attribute/value pairs.
     */

    {
	char *extraHeaders = Ns_ConfigGetValue(path, "extraheaders");

	if (extraHeaders != NULL) {
	    int objc;
	    Tcl_Obj **objv, *headers = Tcl_NewStringObj(extraHeaders, -1);
	    int result = Tcl_ListObjGetElements(NULL, headers, &objc, &objv);

	    if (result != TCL_OK || objc % 2 != 0) {
		Ns_Log(Warning, "Ignoring invalid value for extraheaders: %s", extraHeaders);
	    } else {
		Ns_DStringInit(dsPtr);
		for (i = 0; i < objc; i +=2) {
		    Ns_DStringVarAppend(dsPtr,
					Tcl_GetString(objv[i]), ": ", 
					Tcl_GetString(objv[i+1]), "\r\n", 
					NULL);
		    drvPtr->extraHeaders = Ns_DStringExport(dsPtr);
		}
	    }
	}
    }

    /*
     * Check if upload spooler are enabled
     */

    spPtr = &drvPtr->spooler;
    spPtr->threads = Ns_ConfigIntRange(path, "spoolerthreads", 0, 0, 32);

    if (spPtr->threads > 0) {
        Ns_Log(Notice, "%s: enable %d spooler thread(s) "
               "for uploads >= %" TCL_LL_MODIFIER "d bytes", module,
               spPtr->threads, drvPtr->readahead);
        for (i = 0; i < spPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1, sizeof(SpoolerQueue));
            char buffer[100];

	    sprintf(buffer,"ns:driver:spooler:%d",i);
	    Ns_MutexSetName2(&queuePtr->lock, buffer,"queue");
            queuePtr->id = i;
            Push(queuePtr, spPtr->firstPtr);
        }
    } else {
        Ns_Log(Notice, "%s: enable %d spooler thread(s) ",
               module, spPtr->threads);
    }

    /*
     * Enable writer threads
     */

    wrPtr = &drvPtr->writer;
    wrPtr->threads = Ns_ConfigIntRange(path, "writerthreads", 0, 0, 32);

    if (wrPtr->threads > 0) {
        wrPtr->maxsize = Ns_ConfigIntRange(path, "writersize",
                                           1024*1024, 1024, INT_MAX);
        wrPtr->bufsize = Ns_ConfigIntRange(path, "writerbufsize",
                                           8192, 512, INT_MAX);
        wrPtr->streaming = Ns_ConfigBool(path, "writerstreaming", NS_FALSE);
        Ns_Log(Notice, "%s: enable %d writer thread(s) "
               "for downloads >= %d bytes, bufsize=%d bytes, HTML streaming %d",
               module, wrPtr->threads, wrPtr->maxsize, wrPtr->bufsize, wrPtr->streaming);
        for (i = 0; i < wrPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1, sizeof(SpoolerQueue));
            char buffer[100];

	    sprintf(buffer,"ns:driver:writer:%d",i);
	    Ns_MutexSetName2(&queuePtr->lock, buffer,"queue");
            queuePtr->id = i;
            Push(queuePtr, wrPtr->firstPtr);
        }
    } else {
        Ns_Log(Notice, "%s: enable %d writer thread(s) ",
               module, wrPtr->threads);
    }

    /*
     * Map Host headers for drivers not bound to servers.
     */

    if (server == NULL) {
	Ns_Set *set;

        if (defserver == NULL) {
            Ns_Fatal("%s: virtual servers configured,"
                     " but %s has no defaultserver defined", module, path);
        }
        defMapPtr = NULL;
        path = Ns_ConfigGetPath(NULL, module, "servers", NULL);
        set  = Ns_ConfigGetSection(path);
        Ns_DStringInit(dsPtr);
        for (i = 0; set != NULL && i < Ns_SetSize(set); ++i) {
            server  = Ns_SetKey(set, i);
            host    = Ns_SetValue(set, i);
            servPtr = NsGetServer(server);
            if (servPtr == NULL) {
                Ns_Log(Error, "%s: no such server: %s", module, server);
            } else {
		Tcl_HashEntry  *hPtr = Tcl_CreateHashEntry(&hosts, host, &n);
                if (!n) {
                    Ns_Log(Error, "%s: duplicate host map: %s", module, host);
                } else {
                    Ns_DStringVarAppend(dsPtr, drvPtr->protocol, "://",host,NULL);
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
        Ns_DStringFree(dsPtr);

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

    NsAsyncWriterQueueDisable(1);

    while (drvPtr != NULL) {
        Ns_MutexLock(&drvPtr->lock);
        Ns_Log(Notice, "[driver:%s]: stopping", drvPtr->name);
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


void
NsStopSpoolers(void)
{
    Driver *drvPtr = firstDrvPtr;

    Ns_Log(Notice, "driver: stopping writer and spooler threads");
    
    /*
     * Shutdown all spooler and writer threads
     */
    while (drvPtr != NULL) {
	Ns_Time timeout;
	Ns_GetTime(&timeout);
	Ns_IncrTime(&timeout, nsconf.shutdowntimeout, 0);
	
	SpoolerQueueStop(drvPtr->writer.firstPtr, &timeout, "writer");
	SpoolerQueueStop(drvPtr->spooler.firstPtr, &timeout, "spooler");
        drvPtr = drvPtr->nextPtr;
    }
}



/*
 *----------------------------------------------------------------------
 *
 * NsWakeupDriver --
 *
 *      Wake up the associated DriverThread.  
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The poll waiting for this trigger will be interruped.
 *
 *----------------------------------------------------------------------
 */
void
NsWakeupDriver(Driver *drvPtr) {
    assert(drvPtr);
    SockTrigger(drvPtr->trigger[1]);
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
            Ns_Log(Warning, "[driver:%s]: shutdown timeout", drvPtr->module);
        } else {
            Ns_Log(Notice, "[driver:%s]: stopped", drvPtr->module);
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
NsGetRequest(Sock *sockPtr, Ns_Time *nowPtr)
{
    Request *reqPtr;

    if (sockPtr->reqPtr == NULL) {
        int status;

        do {
            status = SockRead(sockPtr, 0, nowPtr);
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

        reqPtr->next           = NULL;
        reqPtr->content        = NULL;
        reqPtr->length         = 0;
        reqPtr->contentLength  = 0;
        reqPtr->avail          = 0;
        reqPtr->leadblanks     = 0;

        reqPtr->expectedLength = 0;
        reqPtr->chunkStartOff  = 0;
        reqPtr->chunkWriteOff  = 0;

        reqPtr->woff           = 0;
        reqPtr->roff           = 0;
        reqPtr->coff           = 0;

        Tcl_DStringFree(&reqPtr->buffer);
        Ns_SetTrunc(reqPtr->headers, 0);

        if (reqPtr->auth) {
	  Ns_SetFree(reqPtr->auth);
	  reqPtr->auth = NULL;
	}

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

    Ns_Log(DriverDebug, "NsSockClose sockPtr %p keep %d", sockPtr, keep);

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

#if 0
    fprintf(stderr, "NsDriverSend: nbufs %d\n", nbufs);
    fprintf(stderr, "NsDriverSend: bufs[0] %d '%s'\n", bufs[0].iov_len, (char *)bufs[0].iov_base);
    {int i; char *p= (char *)bufs[0].iov_base;
      for (i=0; i<bufs[0].iov_len; i++) {
	char c = *(p+i);
	fprintf(stderr, "[%d] '%c' %d, ", i+1, c<32 ? 32 : c, c);
      }
      fprintf(stderr, "\n");
    }
#endif
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
    Ns_Time        now, diff;
    char          *errstr, c, drain[1024];
    int            flags, stopping, pollto, accepted;
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
	int n;

        /*
         * Set the bits for all active drivers if a connection
         * isn't already pending.
         */

        PollReset(&pdata);
        PollSet(&pdata, drvPtr->trigger[0], POLLIN, NULL);

        if (likely(waitPtr == NULL)) {
            drvPtr->pidx = PollSet(&pdata, drvPtr->sock, POLLIN, NULL);
        }

        /*
         * If there are any closing or read-ahead sockets, set the bits
         * and determine the minimum relative timeout.
	 *
	 * TODO: the various poll timeouts should probably be configurable.
         */

        if (readPtr == NULL && closePtr == NULL) {
            pollto = 10 * 1000;
        } else {

            for (sockPtr = readPtr; sockPtr != NULL; sockPtr = sockPtr->nextPtr) {
                SockPoll(sockPtr, POLLIN, &pdata);
            }
            for (sockPtr = closePtr; sockPtr != NULL; sockPtr = sockPtr->nextPtr) {
                SockPoll(sockPtr, POLLIN, &pdata);
            }

            if (Ns_DiffTime(&pdata.timeout, &now, &diff) > 0)  {
		/* 
		 * The resolution of pollto is ms, therefore, we round
		 * up. If we would round down (eg. found 500
		 * microseconds to 0 ms), the time comparison later
		 * would determine that it is to early.
		 */
	        pollto = (int)(diff.sec * 1000 + diff.usec / 1000 + 1);

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
	 * Check whether we should reanimate some connection threads,
	 * when e.g. the number of current threads dropped blow the
	 * minimal value.  Perform this test on timeouts (n == 0;
	 * just for safety reasons) or on explicit wakeup calls.
	 */
	if (n == 0 || PollIn(&pdata, 0)) {
	    if (drvPtr->servPtr) {
		NsEnsureRunningConnectionThreads(drvPtr->servPtr, NULL);
	    } else {
		Tcl_HashSearch search;
		Tcl_HashEntry *hPtr = Tcl_FirstHashEntry(&hosts, &search);
		/* 
		 * In case, we have a "global" driver, we have to
		 * check all associated servers.
		 */
		while (hPtr != NULL) {
		    ServerMap *mapPtr = Tcl_GetHashValue(hPtr);
		    /* 
		     * We could reduce the calls in case multiple host
		     * entries are mapped to the same server.
		     */
		    NsEnsureRunningConnectionThreads(mapPtr->servPtr, NULL);
		    hPtr = Tcl_NextHashEntry(&search);
		}
	    }
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
		    /* too early, keep waiting */
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

	    if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
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
		    /* too early, keep waiting */
                    Push(sockPtr, readPtr);
                }
		
            } else {
                /*
		 * Got some data.
                 * If enabled, perform read-ahead now.
                 */
                if (likely(sockPtr->drvPtr->opts & NS_DRIVER_ASYNC)) {
                    n = SockRead(sockPtr, 0, &now);

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
			
		    case SOCK_READERROR:
			Ns_Log(DriverDebug, "sockread returned read error; close socket");
			SockRelease(sockPtr, n, errno);
			break;

		    default:
			Ns_Log(Warning, "sockread returned unexpected result %d; close socket", n);
			SockRelease(sockPtr, n, errno);
			break;
		    }
                } else {
		  /* potentially blocking driver, NS_DRIVER_ASYNC is not defined */

		    if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
			Ns_Log(Notice, "read-ahead have some data no async sock read, setting sock more  ===== diff time %d", 
			       Ns_DiffTime(&sockPtr->timeout, &now, &diff));
			sockPtr->keep = 0;
			SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                    } else if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
			Push(sockPtr, waitPtr);
		    }
		}
            }
            sockPtr = nextPtr;
        }
	
        /*
         * Attempt to queue any pending connection after reversing the
         * list to ensure oldest connections are tried first.
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
                   && (n = SockAccept(drvPtr, &sockPtr, &now)) != SOCK_ERROR) {

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
#ifdef __APPLE__
		/* 
		 * On Darwin, the first accept() succeeds typically,
		 * but it is useless to try a attempt, since this
		 * leads always to an EAGAIN
		 */
		break;
#endif
            }
	    if (accepted > 1) {
	        Ns_Log(Notice, "... sockAccept accepted %d connections", accepted);
            }
        }

        /*
         * Check for shutdown and get the list of any closing or
         * keep-alive sockets.
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
                SockTimeout(sockPtr, &now, sockPtr->drvPtr->keepwait);
                Push(sockPtr, readPtr);
            } else {
		if (shutdown(sockPtr->sock, SHUT_WR) != 0) {
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

    if (unlikely(pdata->nfds >= pdata->maxfds)) {
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
SockAccept(Driver *drvPtr, Sock **sockPtrPtr, Ns_Time *nowPtr)
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
        sockPtr->flags  = 0;
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
	sockPtr->acceptTime = *nowPtr;
        drvPtr->queuesize++;

        if (status == NS_DRIVER_ACCEPT_DATA) {

            /*
             * If there is already data present then read it without
             * polling if we're in async mode.
             */

            if (drvPtr->opts & NS_DRIVER_ASYNC) {
                status = SockRead(sockPtr, 0, nowPtr);
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
        Ns_Log(DriverDebug, "SockError: %s (%d: %s), sock: %d, peer: %s:%d, request: %.99s",
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
    ssize_t sent;

    switch (code) {
    case 413:
	response = "Bad Request";
        break;
    case 414:
	response = "Request-URI Too Long";
        break;
    case 400:
    default:
	response = "Bad Request";
        break;
    }
    snprintf(header, sizeof(header),"HTTP/1.0 %d ", code);
    iov[0].iov_base = header;
    iov[0].iov_len = strlen(header);
    iov[1].iov_base = response;
    iov[1].iov_len = strlen(response);
    iov[2].iov_base = "\r\n\r\n";
    iov[2].iov_len = 4;
    sent = NsDriverSend(sockPtr, iov, 3, 0);
    if (sent < iov[0].iov_len+iov[1].iov_len+iov[2].iov_len) {
	Ns_Log(Warning, "Driver: partial write while sending error reply");
    }
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
        Ns_Log(Error, "driver: trigger send() failed: %s", errstr);
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
    char 
      *end = bufPtr->string + bufPtr->length, 
      *chunkStart = bufPtr->string + reqPtr->chunkStartOff;

    while (reqPtr->chunkStartOff <  bufPtr->length) {
      char *p = strstr(chunkStart, "\r\n");
      long chunk_length;

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
SockRead(Sock *sockPtr, int spooler, Ns_Time *timePtr)
{
    Driver       *drvPtr = sockPtr->drvPtr;
    Request      *reqPtr = NULL;
    Tcl_DString  *bufPtr = NULL;

    struct iovec  buf;
    char         tbuf[16384];
    size_t       len, nread;
    ssize_t      n;

    /*
     * In case of keepwait, the accept time is not meaningful and
     * reset to 0. In such cases, update acceptTime to the actual
     * begin of a request. This part is intended for async drivers.
     */
    if (sockPtr->acceptTime.sec == 0) {
	assert(timePtr);
	/*fprintf(stderr, "SOCKREAD reset times "
		" start %" PRIu64 ".%06ld"
		" now %" PRIu64 ".%06ld\n",
		(int64_t) sockPtr->acceptTime.sec, sockPtr->acceptTime.usec,
		(int64_t) timePtr->sec, timePtr->usec
		);*/
	sockPtr->acceptTime = *timePtr; 
    }

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
        DrvSpooler   *spPtr  = &drvPtr->spooler;

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
            sprintf(sockPtr->tfile, "%s/%d.XXXXXX", drvPtr->uploadpath, sockPtr->sock);
            sockPtr->tfd = mkstemp(sockPtr->tfile);
	    if (sockPtr->tfd == -1) {
	      Ns_Log(Error, "nssock: cannot create spool file with template '%s': %s", 
		     sockPtr->tfile, strerror(errno));
	    }
        } else {
	    /* GN: don't we need a Ns_ReleaseTemp() on cleanup? */
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

    if (n < 0) {
	Tcl_DStringSetLength(bufPtr, len);
        return SOCK_READERROR;
    }

    if (n == 0) {
	Tcl_DStringSetLength(bufPtr, len);
        return SOCK_MORE;
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
 *      headers.  Return NS_SOCK_READY when finished parsing.
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

static char *strnchr(char *buffer, size_t len, int c) {
    char *end;

    for (end = buffer + len; buffer < end; buffer ++) {
        if (unlikely(*buffer == c)) {
            return buffer;
        }
    }
    return NULL;
}

static int
SockParse(Sock *sockPtr, int spooler)
{
    Request      *reqPtr;
    Tcl_DString  *bufPtr;
    char          save;
    Driver       *drvPtr = sockPtr->drvPtr;

    NsUpdateProgress((Ns_Sock *) sockPtr);

    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;

    /*
     * Scan lines (header) until start of content (body-part)
     */

    while (reqPtr->coff == 0) {
	char *s, *e;
	int cnt;

        /*
         * Find the next line.
         */
        s = bufPtr->string + reqPtr->roff;
        e = strnchr(s, reqPtr->avail, '\n');

        if (unlikely(e == NULL)) {
            /*
             * Input not yet newline terminated - request more.
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

        if (unlikely((e - s) > drvPtr->maxline)) {
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
        if (likely(e > s) && likely(e[-1] == '\r')) {
            --e;
        }

        /*
         * Check for end of headers.
         */
        if (unlikely(e == s)) {
	    int gzip;

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
                                            PRIdz ", maxinput=%" TCL_LL_MODIFIER "d",
                               reqPtr->length, drvPtr->maxinput);
			/* 
			 * We have to read the full request (although
			 * it is too large) to drain the
			 * channel. Otherwise, the server might close
			 * the connection *before* it has received
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
	    
	    /*
	     * Clear NS_CONN_ZIPACCEPTED flag
	     */
	    sockPtr->flags &= ~(NS_CONN_ZIPACCEPTED);

	    s = Ns_SetIGet(reqPtr->headers, "Accept-Encoding");
	    if (s != NULL) {
		/* get gzip from accept-encoding header */
		gzip = NsParseAcceptEnconding(reqPtr->request.version, s);
	    } else {
		/* no accept-encoding header; don't allow gzip */
		gzip = 0;
	    }
	    if (gzip) {
		/*
		 * Don't allow gzip results for Range requests.
		 */
		s = Ns_SetIGet(reqPtr->headers, "Range");
		if (s == NULL) {
		    sockPtr->flags |= NS_CONN_ZIPACCEPTED;
		}
	    }

        } else {
            save = *e;
            *e = '\0';

            if (unlikely(reqPtr->request.line == NULL)) {
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

            if (unlikely(Ns_SetSize(reqPtr->headers) > drvPtr->maxheaders)) {
                Ns_Log(DriverDebug, "SockParse: maxheaders reached of %d bytes",
                       drvPtr->maxheaders);
                return SOCK_TOOMANYHEADERS;
            }

            *e = save;
            if (unlikely(reqPtr->request.version <= 0.0)) {

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
            Ns_Log(Debug, "spooling content to file: size=%" PRIdz ", file=%s",
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
	    int result = write(sockPtr->tfd, "\0", 1); 
	    if (result == -1) {
		Ns_Log(Error, "socket: could not append terminating 0-byte");
	    }
            sockPtr->tsize = reqPtr->length + 1;
            sockPtr->taddr = mmap(0, sockPtr->tsize, prot, MAP_PRIVATE,
                                  sockPtr->tfd, 0);
            if (sockPtr->taddr == MAP_FAILED) {
                sockPtr->taddr = NULL;
                return SOCK_ERROR;
            }
            reqPtr->content = sockPtr->taddr;
            Ns_Log(Debug, "spooling content to file: readahead=%" 
		   TCL_LL_MODIFIER "d, filesize=%" PRIdz,
                   drvPtr->readahead, sockPtr->tsize);
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
            Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&hosts, host);
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
 *======================================================================
 *  Spooler Thread: Receive asynchronously from the client socket
 *======================================================================
 */

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
    queuePtr->threadname = Ns_ThreadGetName();

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

        /*n =*/ PollWait(&pdata, pollto);

        if (PollIn(&pdata, 0) && unlikely(recv(queuePtr->pipe[0], &c, 1, 0) != 1)) {
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
            if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
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
                n = SockRead(sockPtr, 1, &now);
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
SpoolerQueueStop(SpoolerQueue *queuePtr, Ns_Time *timeoutPtr, CONST char *name)
{
    while (queuePtr != NULL) {
	int status;

        Ns_MutexLock(&queuePtr->lock);
        if (!queuePtr->stopped && !queuePtr->shutdown) {
            Ns_Log(Debug, "%s%d: triggering shutdown", name, queuePtr->id);
            queuePtr->shutdown = 1;
            SockTrigger(queuePtr->pipe[1]);
        }
        status = NS_OK;
        while (!queuePtr->stopped && status == NS_OK) {
            status = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, timeoutPtr);
        }
        if (status != NS_OK) {
	    Ns_Log(Warning, "%s%d: timeout waiting for shutdown", name, queuePtr->id);
        } else {
            /*Ns_Log(Notice, "%s%d: shutdown complete", name, queuePtr->id);*/
	    if (queuePtr->thread) {
		Ns_ThreadJoin(&queuePtr->thread, NULL);
		queuePtr->thread = NULL;
	    } else {
		Ns_Log(Notice, "%s%d: shutdown: thread already gone", name, queuePtr->id);
	    }
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

    Ns_Log(Debug, "Spooler: %d: started fd=%d: %" PRIdz " bytes",
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
 *======================================================================
 *  Writer Thread: Write asynchronously to the client socket
 *======================================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * NsWriterLock, NsWriterUnlock --
 *
 *       Provide an API for locking and unlocking context information
 *       for streaming asynchronous writer jobs.  The locks are just
 *       needed for managing linkage between connPtr and a writer
 *       entry. The lock operations are rather infrequent amnd the
 *       lock duration is very short, such that at a single global
 *       appears sufficient.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Change Mutex state.
 *
 *----------------------------------------------------------------------
 */
void NsWriterLock(void) {
    Ns_MutexLock(&writerlock);
}

void NsWriterUnlock(void) {
    Ns_MutexUnlock(&writerlock);
}


/*
 *----------------------------------------------------------------------
 *
 * WriterSockRequire, WriterSockRelease --
 *
 *      Management functions for WriterSocks. WriterSockRequire() and
 *      WriterSockRelease() omplement reference for obtaining and
 *      freeing WriterSock structures. When a SockStructure is finally
 *      released, it is removed from the queue, the the socket is
 *      closed and the memory is freed.
 *
 * Results:
 *      WriterSockRequire() returns a WriterSock from a connection, 
 *      the other functions return nothing.
 *
 * Side effects:
 *      Updating reference counters, closing socket, freeing memory.
 *
 *----------------------------------------------------------------------
 */

static WriterSock *
WriterSockRequire(Conn *connPtr) {
    WriterSock *wrSockPtr;

    NsWriterLock();
    wrSockPtr = connPtr->streamWriter;
    if (wrSockPtr) {
	wrSockPtr->refCount ++;
    }
    NsWriterUnlock();
    return wrSockPtr;
}

static void
WriterSockRelease(WriterSock *wrSockPtr) {
    SpoolerQueue *queuePtr;

    assert(wrSockPtr);
    wrSockPtr->refCount --;

    Ns_Log(DriverDebug, "WriterSockRelease %p refCount %d", 
	   wrSockPtr, wrSockPtr->refCount);

    if (wrSockPtr->refCount > 0) {
	return;
    }

    Ns_Log(DriverDebug, 
	   "Writer: closed sock=%d, file fd=%d, error=%d/%d, "
           "sent=%" TCL_LL_MODIFIER "d, flags=%X",
           wrSockPtr->sockPtr->sock, wrSockPtr->fd, 
	   wrSockPtr->status, wrSockPtr->err,
           wrSockPtr->nsent, wrSockPtr->flags);

    if (wrSockPtr->streaming) {
	Conn *connPtr;
	NsWriterLock();
	connPtr = wrSockPtr->connPtr;
	if (connPtr && connPtr->streamWriter) {
	    connPtr->streamWriter = NULL;
	}
	NsWriterUnlock();
    }

    /*
     * Remove the entry from the queue and decrement counter
     */
    queuePtr = wrSockPtr->queuePtr;
    if (queuePtr->curPtr == wrSockPtr) {
	queuePtr->curPtr = wrSockPtr->nextPtr;
	queuePtr->queuesize--;
    } else {
	WriterSock *curPtr, *lastPtr = queuePtr->curPtr;
	for (curPtr = lastPtr ? lastPtr->nextPtr : NULL; curPtr; lastPtr = curPtr, curPtr = curPtr->nextPtr) {
	    if (curPtr == wrSockPtr) {
		lastPtr->nextPtr = wrSockPtr->nextPtr;
		queuePtr->queuesize--;
		break;
	    }
	}
    }

    if (wrSockPtr->err || wrSockPtr->status) {
	SockError(wrSockPtr->sockPtr, wrSockPtr->status, wrSockPtr->err);
        NsSockClose(wrSockPtr->sockPtr, 0);
    } else {
        NsSockClose(wrSockPtr->sockPtr, wrSockPtr->keep);
    }
    if (wrSockPtr->clientData) {
	ns_free(wrSockPtr->clientData);
    }
    if (wrSockPtr->fd != INVALID_SOCKET) {
	if (wrSockPtr->streaming != NS_WRITER_STREAM_FINISH) {
	    close(wrSockPtr->fd);
	}
        ns_free(wrSockPtr->file.buf);
    } else if (wrSockPtr->mem.bufs) {
	if (wrSockPtr->mem.fmap.addr) {
	    NsMemUmap(&wrSockPtr->mem.fmap);

	} else {
	    int i;
	    for (i = 0; i < wrSockPtr->mem.nbufs; i++) {
		ns_free(wrSockPtr->mem.bufs[i].iov_base);
	    }
	}
	if (wrSockPtr->mem.bufs != wrSockPtr->mem.preallocated_bufs) {
	    ns_free(wrSockPtr->mem.bufs);
	}
    }
    if (wrSockPtr->headerString) {
	ns_free(wrSockPtr->headerString);
    }

    ns_free(wrSockPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * WriterReadFromSpool --
 *
 *      Utility function of the WriterThread to read blocks from a
 *      file into the output buffer of the writer. It handles
 *      leftovers from previous send attempts and takes care for
 *      locking in case simultaneous reading and writing from the
 *      same file.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Fills up curPtr->file.buf and updates counters/sizes.
 *
 *----------------------------------------------------------------------
 */

static int
WriterReadFromSpool(DrvWriter *wrPtr, WriterSock *curPtr) {
    int            streaming = curPtr->streaming, status = NS_OK;
    Tcl_WideInt    toread, maxsize;
    unsigned char *bufPtr;

    if (streaming) {
	Ns_MutexLock(&curPtr->file.fdlock);
	toread = curPtr->file.toread;
	Ns_MutexUnlock(&curPtr->file.fdlock);
    } else {
	toread = curPtr->file.toread;
    }
    
    maxsize = curPtr->file.maxsize;
    bufPtr  = curPtr->file.buf;
			
    /*
     * When bufsize > 0 we have leftover from previous send. In such
     * cases, move the leftover to the front, and fill the reminder of
     * the buffer with new content.
     */
	
    if (curPtr->file.bufsize > 0) {
	Ns_Log(DriverDebug, 
	       "### Writer %p %.6x leftover %" PRIdz " offset %ld", 
	       curPtr, curPtr->flags, 
	       curPtr->file.bufsize, 
	       (long)curPtr->file.bufoffset);
	if (likely(curPtr->file.bufoffset > 0)) {
	    memmove(curPtr->file.buf, 
		    curPtr->file.buf + curPtr->file.bufoffset, 
		    curPtr->file.bufsize);
	}
	bufPtr = curPtr->file.buf + curPtr->file.bufsize;
	maxsize -= curPtr->file.bufsize;
    }
    if (toread > maxsize) {
	toread = maxsize;
    }
    
    /*
     * Read content from the file into the buffer.
     */
    if (toread > 0) {
	int n;

	if (streaming) {
	    /* 
	     * In streaming mode, the connection thread writes to the
	     * spool file and the writer thread reads from the same
	     * file.  Therefore we have to re-adjust the current
	     * read/writer position, which might be changed by the
	     * other thread. These positions have to be locked, since
	     * seeking might be subject to race conditions. Here we
	     * set the read pointer to the position after the last
	     * send operation.
	     */
	    Ns_MutexLock(&curPtr->file.fdlock);
	    lseek(curPtr->fd, curPtr->nsent, SEEK_SET);
	}
	
	n = read(curPtr->fd, bufPtr, (size_t)toread);
	
	if (n <= 0) {
	    status = SOCK_ERROR;
	} else {
	    /* 
	     * curPtr->file.toread is still protected by curPtr->file.fdlock when
	     * needed.
	     */
	    curPtr->file.toread -= n;
	    curPtr->file.bufsize += n;
	}
	
	if (streaming) {
	    Ns_MutexUnlock(&curPtr->file.fdlock);
	}
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterSend --
 *
 *      Utility function of the WriterThread to send content to the
 *      client. It handles partial reads from the lower level
 *      infrastructure.
 *
 * Results:
 *      either NS_OK or SOCK_ERROR;
 *
 * Side effects:
 *      sends data, might reshuffle iovec.
 *
 *----------------------------------------------------------------------
 */

static int
WriterSend(WriterSock *curPtr, int *err) {
    struct iovec *bufs, vbuf;
    int           nbufs, status = NS_OK;
    size_t        towrite;
    ssize_t       n;
    
    /*
     * Prepare send operation
     */
    if (curPtr->fd > -1) {
	/*
	 * Send a single buffer with curPtr->file.bufsize bytes from the
	 * curPtr->file.buf to the client.
	 */
	vbuf.iov_len = curPtr->file.bufsize;
	vbuf.iov_base = (void *)curPtr->file.buf;
	bufs = &vbuf;
	nbufs = 1;
	towrite = curPtr->file.bufsize;
    } else {
	int i;

	/*
	 * Send multiple buffers.
	 * Get length of remaining buffers
	 */
	towrite = 0;
	for (i = 0; i < curPtr->mem.nsbufs; i ++) {
	    towrite += curPtr->mem.sbufs[i].iov_len;
	}
	Ns_Log(DriverDebug, 
	       "### Writer wants to send remainder nbufs %d len %" PRIdz, 
	       curPtr->mem.nsbufs, towrite);

	/*
	 * Add buffers from the source and fill structure up to max 
	 */
	while (curPtr->mem.bufIdx  < curPtr->mem.nbufs && 
	       curPtr->mem.sbufIdx < UIO_SMALLIOV) {
	    struct iovec *vPtr = &curPtr->mem.bufs[curPtr->mem.bufIdx];
	    
	    if (vPtr->iov_len > 0 && vPtr->iov_base != NULL) {

		Ns_Log(DriverDebug, 
		       "### Writer copies source %d to scratch %d len %" PRIiovlen,
		       curPtr->mem.bufIdx, curPtr->mem.sbufIdx, vPtr->iov_len);

		towrite += Ns_SetVec(curPtr->mem.sbufs, curPtr->mem.sbufIdx++, 
				     vPtr->iov_base, vPtr->iov_len);
		curPtr->mem.nsbufs++;
	    }
	    curPtr->mem.bufIdx++;
	}

	bufs  = curPtr->mem.sbufs;
	nbufs = curPtr->mem.nsbufs;
	Ns_Log(DriverDebug, "### Writer wants to send %d bufs size %" PRIdz, 
	       nbufs, towrite);
    }
    
    n = NsDriverSend(curPtr->sockPtr, bufs, nbufs, 0);
   
    if (n < 0) {
	*err = errno;
	status = SOCK_WRITEERROR;
    } else {
	/* 
	 * We have sent something.
	*/
	if (curPtr->streaming) {
	    Ns_MutexLock(&curPtr->file.fdlock);
	    curPtr->size -= n;
	    Ns_MutexUnlock(&curPtr->file.fdlock);
	} else {
	    curPtr->size -= n;
	}
	curPtr->nsent += n;
	curPtr->sockPtr->timeout.sec = 0;

	if (curPtr->fd > -1) {
	    curPtr->file.bufsize -= n;
	    curPtr->file.bufoffset = n;
	    /* for partial transmits bufsize is now > 0 */
	} else {	
	    if (n < towrite) {
		/*
		 * We have a partial transmit from the iovec
		 * structure. We have to compact it to fill content in
		 * the next round.
		 */
		curPtr->mem.sbufIdx = Ns_ResetVec(curPtr->mem.sbufs, curPtr->mem.nsbufs, n);
		curPtr->mem.nsbufs -= curPtr->mem.sbufIdx;
		
		memmove(curPtr->mem.sbufs, curPtr->mem.sbufs + curPtr->mem.sbufIdx, 
		/* move the iovecs to the start of the scratch buffers */
			(size_t) sizeof(struct iovec) * curPtr->mem.nsbufs);
	    }
	}
    }

    return status;
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
    int             err, stopping, pollto, status;
    Ns_Time         now;
    Sock           *sockPtr;
    Driver         *drvPtr;
    DrvWriter      *wrPtr;
    WriterSock     *curPtr, *nextPtr, *writePtr;
    PollData        pdata;

    Ns_ThreadSetName("-writer%d-", queuePtr->id);
    queuePtr->threadname = Ns_ThreadGetName();

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
	unsigned char c;

        /*
         * If there are any write sockets, set the bits.
         */

        PollReset(&pdata);
        PollSet(&pdata, queuePtr->pipe[0], POLLIN, NULL);

        if (writePtr == NULL) {
            pollto = 30 * 1000;
        } else {
	    pollto = 1 * 1000;
            for (curPtr = writePtr; curPtr != NULL; curPtr = curPtr->nextPtr) {
		Ns_Log(DriverDebug, "### Writer pollcollect %p size %" PRIdz " streaming %d", 
		       curPtr, curPtr->size, curPtr->streaming);
		if (likely(curPtr->size > 0)) {
                    SockPoll(curPtr->sockPtr, POLLOUT, &pdata);
		    pollto = -1;
		} else if (unlikely(curPtr->streaming == NS_WRITER_STREAM_FINISH)) {
		    pollto = -1;
		}
	    }
        }

        /*
         * Select and drain the trigger pipe if necessary.
         */
        PollWait(&pdata, pollto);

        if (PollIn(&pdata, 0) && unlikely(recv(queuePtr->pipe[0], &c, 1, 0) != 1)) {
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
	    int streaming;

            nextPtr = curPtr->nextPtr;

            sockPtr = curPtr->sockPtr;
            drvPtr  = sockPtr->drvPtr;
            wrPtr   = &drvPtr->writer;
            err = status = NS_OK;

	    /* the truth value of streaming does not change through concurrency */
	    streaming = curPtr->streaming; 

	    if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
		Ns_Log(DriverDebug, "### Writer %p reached POLLHUP fd %d", curPtr, sockPtr->sock);
		status = SOCK_CLOSE;
		err = 0;

	    } else if (likely(PollOut(&pdata, sockPtr->pidx)) || (streaming == NS_WRITER_STREAM_FINISH)) {
		Ns_Log(DriverDebug, 
                       "### Writer %p can write to client fd %d (trigger %d) streaming %.6x"
		       " size %" PRIdz " nsent %" TCL_LL_MODIFIER "d bufsize %" PRIdz,
                       curPtr, sockPtr->sock, PollIn(&pdata, 0), streaming,
                       curPtr->size, curPtr->nsent, curPtr->file.bufsize);
		if (unlikely(curPtr->size < 1)) {
		    /*
		     * Size < 0 means that verything was sent.
		     */
		    if (streaming != NS_WRITER_STREAM_ACTIVE) {
			if (streaming == NS_WRITER_STREAM_FINISH) {
			    Ns_ReleaseTemp(curPtr->fd);
			}
			status = SOCK_CLOSE;
		    }
		} else {
		    /*
		     * If size > 0, there is still something to send.
		     * If we are spooling from a file, read some data
		     * from the (spool) file and place it into curPtr->file.buf.
		     */
		    if (curPtr->fd > -1) {
			status = WriterReadFromSpool(wrPtr, curPtr);
		    }
		    
		    if (status == NS_OK) {
			status = WriterSend(curPtr, &err);
		    }
		} 
            } else {

                /*
                 *  Mark when first timeout occured or check if it is already
                 *  for too long and we need to stop this socket
                 */
                if (sockPtr->timeout.sec == 0) {
		    Ns_Log(DriverDebug, "Writer %p fd %d setting sendwait %d", 
			   curPtr, sockPtr->sock, curPtr->sockPtr->drvPtr->sendwait);
                    SockTimeout(sockPtr, &now, curPtr->sockPtr->drvPtr->sendwait);
		} else if (Ns_DiffTime(&sockPtr->timeout, &now, NULL) <= 0) {
		    Ns_Log(DriverDebug, "Writer %p fd %d timeout", curPtr, sockPtr->sock);
		    err    = ETIMEDOUT;
		    status = SOCK_CLOSETIMEOUT;
                }
            }

            /*
             * Check result status and close the socket in case of
             * timeout or completion
             */

	    Ns_MutexLock(&queuePtr->lock);
            if (status == NS_OK) {
                if (curPtr->size > 0 || streaming == NS_WRITER_STREAM_ACTIVE) {
		    Ns_Log(DriverDebug, 
			   "Writer %p continue OK (size %" PRIdz ") => PUSH", 
			   curPtr, curPtr->size);
                    Push(curPtr, writePtr);
		} else {
		    Ns_Log(DriverDebug, 
			   "Writer %p done OK (size %" PRIdz ") => RELEASE", 
			   curPtr, curPtr->size);
		    WriterSockRelease(curPtr);
		}
	    } else {
		/*
		 * status might be SOCK_CLOSE or SOCK_CLOSETIMEOUT, or SOCK_ERR
		 */ 
		Ns_Log(DriverDebug, 
		       "Writer %p fd %d release, not OK (status %d) => RELEASE", 
		       curPtr, curPtr->sockPtr->sock, status);
		curPtr->status = status;
		curPtr->err    = err;
		WriterSockRelease(curPtr);
            }
	    Ns_MutexUnlock(&queuePtr->lock);
            curPtr = nextPtr;
        }

        /*
         * Add more sockets to the writer queue
         */

	if (queuePtr->sockPtr) {
	    Ns_MutexLock(&queuePtr->lock);
	    if (queuePtr->sockPtr) {
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
	    }
	    Ns_MutexUnlock(&queuePtr->lock);
	}

        /*
         * Check for shutdown
         */

        stopping = queuePtr->shutdown;
    }
    PollFree(&pdata);

    Ns_Log(Notice, "exiting");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = 1;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * NsWriterFinish --
 *
 *      Finish a streaming writer job (typically called at the close
 *      of a connection). A streaming writer job is fed typically by a
 *      sequence of ns_write operations. After such an operation, the
 *      WriterThread has to keep the writer job alive. 
 *      NsWriterFinish() tells the WriterThread that no more
 *      other writer jobs will come from this connection.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Change the state of the writer job. and trigger the queue.
 *
 *----------------------------------------------------------------------
 */

void 
NsWriterFinish(WriterSock *wrSockPtr) {
    Ns_Log(DriverDebug, "NsWriterFinish: %p", wrSockPtr);
    wrSockPtr->streaming = NS_WRITER_STREAM_FINISH;
    SockTrigger(wrSockPtr->queuePtr->pipe[1]);
}


/*
 *----------------------------------------------------------------------
 *
 * NsWriterQueue --
 *
 *      Submit a new job to the writer queue.
 *
 * Results:
 *
 *      NS_ERROR means that the Writer thread refuses to accept this
 *      job and that the client (the connection thread) has to handle
 *      this data.  NS_OK means that the Writer thread cares for
 *      transmitting the content to the client.
 *
 * Side effects:
 *      Potentially adding a job to the writer queue.
 *
 *----------------------------------------------------------------------
 */

int
NsWriterQueue(Ns_Conn *conn, size_t nsend, Tcl_Channel chan, FILE *fp, int fd,
              struct iovec *bufs, int nbufs, int everysize)
{
    Conn          *connPtr = (Conn*)conn;
    WriterSock    *wrSockPtr; 
    SpoolerQueue  *queuePtr;
    DrvWriter     *wrPtr;
    int            trigger = 0, headerSize;

    if (connPtr == NULL || connPtr->sockPtr == NULL) {
        return NS_ERROR;
    }

    wrPtr = &connPtr->sockPtr->drvPtr->writer;

    Ns_Log(DriverDebug, 
	   "NsWriterQueue: size %" PRIdz " bufs %p (%d) flags %.6x stream %.6x chan %p fd %d thread %d", 
	   nsend, bufs, nbufs, connPtr->flags, connPtr->flags & NS_CONN_STREAM, chan, fd, wrPtr->threads);

    if (wrPtr->threads == 0) {
        Ns_Log(DriverDebug, "NsWriterQueue: no writer threads configured");
        return NS_ERROR;
    }

    if (nsend < (size_t)wrPtr->maxsize && !everysize && connPtr->fd == 0) {
        Ns_Log(DriverDebug, "NsWriterQueue: file is too small(%" PRIdz " < %d)",
               nsend, wrPtr->maxsize);
        return NS_ERROR;
    }

    if (connPtr->flags & NS_CONN_STREAM || connPtr->fd > 0) {
	int         first = 0, wrote = 0;
	WriterSock *wrSockPtr = NULL;

	if (wrPtr->streaming == NS_FALSE) {
	    return NS_ERROR;
	}

	if (unlikely(fp != NULL || fd > -1)) {
	    Ns_Log(DriverDebug, "NsWriterQueue: does not stream from this source via writer");
	    return NS_ERROR;
	}

	Ns_Log(DriverDebug, "NsWriterQueue: streaming writer job");

	if (connPtr->fd == 0) {
	    /*
	     * Create a new temporary spool file.
	     */
	    first = 1;
	    fd = connPtr->fd = Ns_GetTemp();

	    Ns_Log(DriverDebug, "NsWriterQueue: new tmp file has fd %d", fd);
	
	} else {
	    /*
	     * Reuse previously created spool file.
	     */
	    wrSockPtr = WriterSockRequire(connPtr);
	    if (wrSockPtr == NULL) {
		Ns_Log(Notice, 
		       "NsWriterQueue: writer job was already canceled; maybe user dropped connection.");
		return NS_ERROR;
	    }
	    Ns_MutexLock(&wrSockPtr->file.fdlock);
	    lseek(connPtr->fd, 0, SEEK_END);
	}

	/*
	 * For the time being, handle just "string data" in streaming
	 * output (iovec bufs). Write the content to the spool file.
	 */
	{
	    int i;
	    assert(bufs != NULL);
	    for (i = 0; i < nbufs; i++) {
		int j = write(connPtr->fd, bufs[i].iov_base, bufs[i].iov_len);
		wrote += j;
		Ns_Log(Debug, "NsWriterQueue: fd %d [%d] spooled %d of %" PRIiovlen " OK %d", 
		       connPtr->fd, i, j, bufs[i].iov_len, j == bufs[i].iov_len);
	    }
	}

	if (first) {
	    bufs = NULL;
	    connPtr->nContentSent = wrote;
	    fcntl(connPtr->fd, F_SETFL, O_NONBLOCK);
	    /*
	     * Fall through to register stream writer with temp file 
	     */
	} else {
	    /*
	     * This is a later streaming operation, where the writer
	     * job (streamWriter) was previously established. Update
	     * the controlling variables (size and toread), and the
	     * length info for the access log, and trigger the writer
	     * to notify it about the change.
	     */
	    assert(wrSockPtr != NULL);
	    connPtr->streamWriter->size += wrote;
	    connPtr->streamWriter->file.toread += wrote;
	    Ns_MutexUnlock(&wrSockPtr->file.fdlock);

	    connPtr->nContentSent += wrote;
	    if (likely(wrSockPtr->queuePtr != NULL)) {
		SockTrigger(wrSockPtr->queuePtr->pipe[1]);
	    }
	    WriterSockRelease(wrSockPtr);
	    return TCL_OK;
	}
    } else {
	if (fp != NULL) {
	    /* 
	     * The client provided an open file pointer and closes it
	     */
	    fd = ns_sockdup(fileno(fp));
	} else if (fd != -1) {
	    /* 
	     * The client provided an open file descriptor and closes it 
	     */
	    fd = ns_sockdup(fd);
	} else if (chan != NULL) {
	    /* 
	     * The client provided an open tcl channel and closes it 
	     */
	    if (Tcl_GetChannelHandle(chan, TCL_READABLE,
				     (ClientData)&fd) != TCL_OK) {
		return NS_ERROR;
	    }
	    fd = ns_sockdup(fd);
        }
    }

    Ns_Log(DriverDebug, "NsWriterQueue: writer threads %d nsend %" PRIdz " maxsize %d",
	   wrPtr->threads, nsend, wrPtr->maxsize);

    assert(connPtr->poolPtr);
    /* Ns_MutexLock(&connPtr->poolPtr->threads.lock); */
    connPtr->poolPtr->stats.spool++;
    /* Ns_MutexUnlock(&connPtr->poolPtr->threads.lock); */

    wrSockPtr = (WriterSock *)ns_calloc(1, sizeof(WriterSock));
    wrSockPtr->sockPtr = connPtr->sockPtr;
    wrSockPtr->sockPtr->timeout.sec = 0;
    wrSockPtr->flags = connPtr->flags;
    wrSockPtr->refCount = 1;

    /*
     * Make sure we have proper content length header for keep-alives
     */
    Ns_ConnSetLengthHeader(conn, (wrSockPtr->flags & NS_CONN_STREAM) ? -1 : nsend);

    /*
     * Flush the headers
     */

    if (!(conn->flags & NS_CONN_SENTHDRS)) {
	Tcl_DString    ds;

	Ns_DStringInit(&ds);
	Ns_Log(DriverDebug, "add header (fd %d)\n", fd);
	conn->flags |= NS_CONN_SENTHDRS;
	Ns_CompleteHeaders(conn, nsend, 0, &ds);

	wrSockPtr->headerString = ns_strdup(Tcl_DStringValue(&ds));
	headerSize = Ns_DStringLength(&ds);
	Ns_DStringFree(&ds);
    } else {
	headerSize = 0;
    }

    if (fd != -1) {
	// maybe add mmap support for files (fd != -1)

	wrSockPtr->fd = fd;
	if (unlikely(headerSize >= wrPtr->bufsize)) {
	    /* 
	     * We have a header which is larger than bufsize; place it
	     * as "leftover" and use the headerString as buffer for file
	     * reads (rather rare case)
	     */
	    wrSockPtr->file.buf = (unsigned char *)wrSockPtr->headerString;
	    wrSockPtr->file.maxsize = headerSize;
	    wrSockPtr->file.bufsize = headerSize;
	    wrSockPtr->headerString = NULL;
	} else if (headerSize > 0) {
	    /* 
	     * We have a header that fits into the bufsize; place it
	     * as "leftover" at the end of the buffer.
	     */
	    wrSockPtr->file.buf = ns_malloc(wrPtr->bufsize);
	    memcpy(wrSockPtr->file.buf, wrSockPtr->headerString, headerSize);
	    wrSockPtr->file.bufsize = headerSize;
	    wrSockPtr->file.maxsize = wrPtr->bufsize;
	    ns_free(wrSockPtr->headerString);
	    wrSockPtr->headerString = NULL;
	} else {
	    assert(wrSockPtr->headerString == NULL);
	    wrSockPtr->file.buf = ns_malloc(wrPtr->bufsize);
	    wrSockPtr->file.maxsize = wrPtr->bufsize;
	}
	wrSockPtr->file.bufoffset = 0;
	wrSockPtr->file.toread = nsend;

    } else if (bufs != NULL) {
	int   i, j, headerbufs = headerSize > 0 ? 1 : 0;

	wrSockPtr->fd = INVALID_SOCKET;
	
	if (nbufs+headerbufs < UIO_SMALLIOV) {
	    wrSockPtr->mem.bufs = wrSockPtr->mem.preallocated_bufs;
	} else {
	    Ns_Log(Notice, "NsWriterQueue: alloc %d iovecs", nbufs);
	    wrSockPtr->mem.bufs = ns_calloc(nbufs+headerbufs, sizeof(struct iovec));
	}
	wrSockPtr->mem.nbufs = nbufs+headerbufs;
	if (headerbufs) {
	    wrSockPtr->mem.bufs[0].iov_base = wrSockPtr->headerString;
	    wrSockPtr->mem.bufs[0].iov_len  = headerSize;
	}

	if (connPtr->fmap.addr != NULL) {
	    Ns_Log(DriverDebug, "NsWriterQueue: deliver fmapped %p", connPtr->fmap.addr);
	    /*
	     * Deliver an mmapped file, no need to copy content
	     */
	    for (i = 0, j=headerbufs; i < nbufs; i++, j++) {
		wrSockPtr->mem.bufs[j].iov_base = bufs[i].iov_base;
		wrSockPtr->mem.bufs[j].iov_len  = bufs[i].iov_len;
	    }
	    /*
	     * Make a copy of the fmap structure and make clear that
	     * we unmap in the writer thread.
	     */
	    wrSockPtr->mem.fmap = connPtr->fmap;
	    connPtr->fmap.addr = NULL;
	    /* header string will be freed via wrSockPtr->headerString */

	} else {
	    /*
	     * Deliver an content from iovec. The lifetime of the
	     * source is unknown, we have to copy the content.
	     */
	    for (i = 0, j=headerbufs; i < nbufs; i++, j++) {
		wrSockPtr->mem.bufs[j].iov_base = ns_malloc(bufs[i].iov_len);
		wrSockPtr->mem.bufs[j].iov_len  = bufs[i].iov_len;
		memcpy(wrSockPtr->mem.bufs[j].iov_base, bufs[i].iov_base, bufs[i].iov_len);
	    }
	    /* header string will be freed a buf[0] */
	    wrSockPtr->headerString = NULL;
	}

    } else {
        ns_free(wrSockPtr);
        return NS_ERROR;
    }
    
    /*
     * Add header size to total size.
     */
    nsend += headerSize;


    if (connPtr->clientData) {
	wrSockPtr->clientData = ns_strdup(connPtr->clientData);
    }
    wrSockPtr->startTime = *Ns_ConnStartTime(conn);

    /*
     * Setup streaming context before sending potentially headers.
     */

    if ((wrSockPtr->flags & NS_CONN_STREAM) != 0) { 
	wrSockPtr->streaming = NS_WRITER_STREAM_ACTIVE;
	assert(connPtr->streamWriter == NULL);
	/*
	 * Add a reference to the stream writer to the connection such
	 * it can efficiently append to a stream when multiple output
	 * operations happen. The backpointer (from the stream writer
	 * to the connection is needed to clear the reference to the
	 * writer in case the writer is deleted. No locks are needed,
	 * since nobody can share this structure yet.
	 */
	connPtr->streamWriter = wrSockPtr;
	wrSockPtr->connPtr = connPtr; 
    }

    /*
     * Tell connection, that writer handles the output (including
     * closing the connection to the client).
     */

    connPtr->flags |= NS_CONN_SENT_VIA_WRITER;

    wrSockPtr->keep = connPtr->keep > 0 ? 1 : 0;
    wrSockPtr->size = nsend;
    
    if ((wrSockPtr->flags & NS_CONN_STREAM) == 0) { 
	connPtr->sockPtr = NULL;
	connPtr->nContentSent = nsend - headerSize;
    }

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

    Ns_Log(DriverDebug, "Writer: %d: started sock=%d, fd=%d: "
           "size=%" PRIdz ", flags=%X: %s",
           queuePtr->id, wrSockPtr->sockPtr->sock, wrSockPtr->fd,
           nsend, wrSockPtr->flags, connPtr->reqPtr->request.url);
    
    /*
     * Now add new writer socket to the writer thread's queue
     */

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = 1;
    }

    Push(wrSockPtr, queuePtr->sockPtr);
    Ns_MutexUnlock(&queuePtr->lock);
    wrSockPtr->queuePtr = queuePtr;

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
    Tcl_DString   ds, *dsPtr = &ds;
    Ns_Conn      *conn;
    Driver       *drvPtr;
    DrvWriter    *wrPtr = NULL;
    WriterSock   *wrSockPtr;
    SpoolerQueue *queuePtr;
    char         *driverName;
    NsServer     *servPtr = NULL;

    static CONST char *opts[] = {
        "submit", "submitfile", "list", "size", "streaming", NULL
    };

    enum {
        cmdSubmitIdx, cmdSubmitFileIdx, cmdListIdx, cmdSizeIdx, cmdStreamingIdx
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
	    struct iovec vbuf;
	    vbuf.iov_base = (void *) data;
	    vbuf.iov_len = size;
            rc = NsWriterQueue(conn, size, NULL, NULL, -1, &vbuf, 1, 1);
            Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
        }
        break;
    }

    case cmdSubmitFileIdx: {
        struct stat st;
        char       *name;
        Tcl_Obj    *fileObj = NULL;
	int         headers = 0;
        Tcl_WideInt offset = 0, size = 0;
	size_t      nrbytes;

        Ns_ObjvSpec opts[] = {
            {"-headers",  Ns_ObjvBool,    &headers, (void *) NS_TRUE},
            {"-offset",   Ns_ObjvWideInt, &offset,  NULL},
            {"-size",     Ns_ObjvWideInt, &size,    NULL},
            {NULL, NULL, NULL, NULL}
        };
        Ns_ObjvSpec args[] = {
            {"file",      Ns_ObjvObj,     &fileObj,    NULL},
            {NULL, NULL, NULL, NULL}
        };

        if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        }

        if (conn == NULL) {
            Tcl_AppendResult(interp, "no connection", NULL);
            return TCL_ERROR;
        }

        name = Tcl_GetString(fileObj);

	rc = stat(name, &st);
	if (rc != 0) {
	    Tcl_AppendResult(interp, "file does not exist '", name, "'", NULL);
	    return TCL_ERROR;
	}

        fd = open(name, O_RDONLY);
        if (fd == -1) {
	    Tcl_AppendResult(interp, "could not open file '", name, "'", NULL);
            return TCL_ERROR;
        }

	if (size < 0 || size > st.st_size) {
	    Tcl_AppendResult(interp, "size must be a positive value less or equal filesize", NULL);
	    close(fd);
	    return TCL_ERROR;
	}

	if (offset < 0 || offset > st.st_size) {
	    Tcl_AppendResult(interp, "offset must be a positive value less or equal filesize", NULL);
	    close(fd);
	    return TCL_ERROR;
	} 

	if (size > 0) {
	    if (size + offset > st.st_size) {
		Tcl_AppendResult(interp, "offset + size must be less or equal filesize", NULL);
		close(fd);
		return TCL_ERROR;
	    }
	    nrbytes = size;
	} else {
	    nrbytes = st.st_size - offset;
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

        rc = NsWriterQueue(conn, nrbytes, NULL, NULL, fd, NULL, 0, 1);

        Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
        close(fd);

        break;
    }

    case cmdListIdx: 

	if (objc < 2) {
	usage_error:
	    Tcl_WrongNumArgs(interp, 2, objv, "?-server server?");
	    return TCL_ERROR;
	}

	if (objc > 4) {
	    goto usage_error;
	} else if (objc > 2) {

	    int                 nextArgIdx;
	    static CONST char  *options[]           = {"-server", NULL};
	    enum                                      {OServerIdx};
	    ClientData          optionClientData[1] = {NULL};
	    Ns_OptionConverter *optionConverter[1]  = {Ns_OptionServer};

	    if (Ns_ParseOptions(options, optionConverter, optionClientData, interp, 2, 
				Ns_NrElements(options)-1, &nextArgIdx, objc, objv) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (*Tcl_GetStringResult(interp) != '\0') {
		return TCL_ERROR;
	    }
	    servPtr = optionClientData[OServerIdx];
	}

        Tcl_DStringInit(dsPtr);

        for (drvPtr = firstDrvPtr; drvPtr != NULL; drvPtr = drvPtr->nextPtr) {
	    /*
	     * if server was specified, list only results from this server.
	     */
	    if (servPtr && servPtr != drvPtr->servPtr) {
		continue;
	    }

            wrPtr = &drvPtr->writer;
            queuePtr = wrPtr->firstPtr;
            while (queuePtr != NULL) {
                Ns_MutexLock(&queuePtr->lock);
                wrSockPtr = queuePtr->curPtr;
                while (wrSockPtr != NULL) {
                    Ns_DStringPrintf(dsPtr, "{%" PRIu64 ".%06ld %s %s %s %d "
                                     "%" PRIdz " %" TCL_LL_MODIFIER "d ",
				     (int64_t) wrSockPtr->startTime.sec, wrSockPtr->startTime.usec,
				     queuePtr->threadname,
                                     drvPtr->name,
                                     ns_inet_ntoa(wrSockPtr->sockPtr->sa.sin_addr),
                                     wrSockPtr->fd, wrSockPtr->size, wrSockPtr->nsent);
                    Ns_DStringAppendElement(dsPtr, wrSockPtr->clientData ? wrSockPtr->clientData : "");
                    Ns_DStringAppend(dsPtr, "} ");
                    wrSockPtr = wrSockPtr->nextPtr;
                }
                Ns_MutexUnlock(&queuePtr->lock);
                queuePtr = queuePtr->nextPtr;
            }
        }
        Tcl_AppendResult(interp, ds.string, 0);
        Tcl_DStringFree(dsPtr);
        break;

    case cmdSizeIdx:
    case cmdStreamingIdx:
	if (objc < 3 || objc > 4) {
	    Tcl_WrongNumArgs(interp, 2, objv, "driver ?value?");
	    return TCL_ERROR;
	}
	driverName = Tcl_GetString(objv[2]);
	
	/* look up driver with the specified name */
        for (drvPtr = firstDrvPtr; drvPtr; drvPtr = drvPtr->nextPtr) {
	    if (strncmp(driverName, drvPtr->name, strlen(driverName)) == 0) {
		if (drvPtr->writer.firstPtr != NULL) {wrPtr = &drvPtr->writer;}
		break;
	    }
	}

	if (wrPtr == NULL) {
	    Tcl_AppendResult(interp, "no writer configured for a driver with name ", driverName, NULL);
	    return TCL_ERROR;
	}

	if (opt == cmdSizeIdx) {
	    if (objc == 4) {
		int value = 0;

		if (Tcl_GetIntFromObj(interp, objv[3], &value) != NS_OK || value < 1024) {
		    Tcl_AppendResult(interp, "argument is not an integer in valid range: ", 
				     Tcl_GetString(objv[3]), " (min 1024)", NULL);
		    return TCL_ERROR;
		}
		wrPtr->maxsize = value;
	    }
	    Tcl_SetObjResult(interp, Tcl_NewIntObj(wrPtr->maxsize));

	} else {
	    if (objc == 4) {
		int value = 0;

		if (Tcl_GetBooleanFromObj(interp, objv[3], &value) != TCL_OK) {
		    return TCL_ERROR;
		}
		wrPtr->streaming = value;
	    }
	    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(wrPtr->streaming));
	}
	break;
    }

    return NS_OK;
}

/*
 *======================================================================
 *  Async (log) writer: Write asynchronously to a disk
 *======================================================================
 */


/*
 *----------------------------------------------------------------------
 *
 * NsAsyncWriterQueueEnable --
 *
 *      Enable async writing and start the AsyncWriterThread if
 *      necessary
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potentially starting a thread an set stopped to 0.
 *
 *----------------------------------------------------------------------
 */
void 
NsAsyncWriterQueueEnable() 
{
    SpoolerQueue  *queuePtr;

    if (!Ns_ConfigBool(NS_CONFIG_PARAMETERS, "asynclogwriter", NS_FALSE)) {
	/*
	 * Asyncwriter is disabled, nothing to do.
	 */
	return;
    }

    /*
     * In case, the async writer is not allocated started, the static
     * variable asyncWriter is NULL.
     */
    if (asyncWriter == NULL) {
	Ns_MutexLock(&reqLock);
	if (asyncWriter == NULL) {
	    /*
	     * Allocate and initialize writer thread context.
	     */
	    asyncWriter = ns_calloc(1, sizeof(AsyncWriter));
	    Ns_MutexUnlock(&reqLock);
	    Ns_MutexSetName2(&asyncWriter->lock, "ns:driver","async-writer");
	    /*
	     * Allocate and initialize a Spooler Queue for this thread.
	     */
	    queuePtr = ns_calloc(1, sizeof(SpoolerQueue));
	    Ns_MutexSetName2(&queuePtr->lock, "ns:driver:async-writer","queue");
	    asyncWriter->firstPtr = queuePtr;
	    /*
	     * Start the spooler queue
	     */
	    SpoolerQueueStart(queuePtr, AsyncWriterThread);

	} else {
	    Ns_MutexUnlock(&reqLock);
	}
    }


    assert(asyncWriter);
    queuePtr = asyncWriter->firstPtr;
    assert(queuePtr);

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->stopped = 0;
    Ns_MutexUnlock(&queuePtr->lock);
}

/*
 *----------------------------------------------------------------------
 *
 * NsAsyncWriterQueueDisable --
 *
 *      Disable async writing but don't touch the writer thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Disable async writing by setting stopped to 1.
 *
 *----------------------------------------------------------------------
 */
void 
NsAsyncWriterQueueDisable(int shutdown) 
{
    if (asyncWriter) {
	SpoolerQueue *queuePtr = asyncWriter->firstPtr;
	Ns_Time timeout;

	assert(queuePtr);

	Ns_GetTime(&timeout);
	Ns_IncrTime(&timeout, nsconf.shutdowntimeout, 0);

	Ns_MutexLock(&queuePtr->lock);
	queuePtr->stopped = 1;
	queuePtr->shutdown = shutdown;

	/*
	 * Trigger the AsyncWriter Thread to drain the spooler queue.
	 */
	SockTrigger(queuePtr->pipe[1]);
	Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, &timeout);

	Ns_MutexUnlock(&queuePtr->lock);
	
	if (shutdown) {
	    ns_free(queuePtr);
	    ns_free(asyncWriter);
	    asyncWriter = NULL;
	}
    }
 }


/*
 *----------------------------------------------------------------------
 *
 * NsAsyncWrite --
 *
 *      Perform an asynchronous write operation via a writer thread in
 *      case a writer thread is configured and running. The intention
 *      of the asynchronous write operations is to reduce latencies in
 *      connection threads.
 *
 * Results:
 *      NS_OK, when write was performed via writer thread, 
 *      NS_ERROR otherwise (but data is written).
 *
 * Side effects:
 *      I/O Operation.
 *
 *----------------------------------------------------------------------
 */
int 
NsAsyncWrite(int fd, char *buffer, size_t nbyte) 
{
    SpoolerQueue  *queuePtr;
    int            trigger = 0;
    AsyncWriteData *wdPtr, *newWdPtr;

    /*
     * If the async writer has not started or is deactivated, behave
     * like a write() command. If the write() fails, we can't do much,
     * since writing of an error message to the log might bring us
     * into an infinte loop.
     */
    if (asyncWriter == NULL || asyncWriter->firstPtr->stopped) {
        int unused NS_GNUC_UNUSED =
	  write(fd, buffer, nbyte);
        return NS_ERROR;
    }

    /*
     * Allocate a writer cmd and initialize it. In order to provide an
     * interface compatible to write(), we copy the provided data,
     * such it can be freed by the caller. Wen we would give up the
     * interface, we could free the memory block after writing, and
     * save a malloc/free operation on the data.
     */
    newWdPtr = ns_calloc(1, sizeof(AsyncWriteData));
    newWdPtr->fd = fd;
    newWdPtr->bufsize = nbyte;
    newWdPtr->data = ns_malloc(nbyte + 1);
    memcpy(newWdPtr->data, buffer, newWdPtr->bufsize);
    newWdPtr->buf  = newWdPtr->data;
    newWdPtr->size = newWdPtr->bufsize;

    /*
     * Now add new writer socket to the writer thread's queue. In most
     * cases, the queue will be empty.
     */
    queuePtr = asyncWriter->firstPtr;
    assert(queuePtr);

    Ns_MutexLock(&queuePtr->lock);
    wdPtr = queuePtr->sockPtr;
    if (wdPtr) {
	newWdPtr->nextPtr = queuePtr->sockPtr;
	queuePtr->sockPtr = newWdPtr;
    } else {
	queuePtr->sockPtr = newWdPtr;
	trigger = 1;
    }
    Ns_MutexUnlock(&queuePtr->lock);

    /*
     * Wake up writer thread if desired
     */
    if (trigger) { 
        SockTrigger(queuePtr->pipe[1]);
    }

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * AsyncWriterRelease --
 *
 *      Deallocate write data. This function has the same interface as
 *      the *Release operations for the writer/spooler threads.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      free memory
 *
 *----------------------------------------------------------------------
 */
static void
AsyncWriterRelease(AsyncWriteData *wdPtr, int reason, int err)
{
    ns_free(wdPtr->data);
    ns_free(wdPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * AsyncWriterThread --
 *
 *      Thread that implements non-blocking write operations to files
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Write to files.
 *
 *----------------------------------------------------------------------
 */

static void
AsyncWriterThread(void *arg)
{
    SpoolerQueue   *queuePtr = (SpoolerQueue*)arg;
    unsigned char   c;
    int             n, err, stopping, pollto, status;
    AsyncWriteData *curPtr, *nextPtr, *writePtr;
    PollData        pdata;

    /*fprintf(stderr, "--- AsyncWriterThread started queuePtr %p asyncWriter %p asyncWriter->firstPtr %p\n", 
	    queuePtr,
	    asyncWriter, asyncWriter->firstPtr);*/
    Ns_ThreadSetName("-asynclogwriter%d-", queuePtr->id);
    queuePtr->threadname = Ns_ThreadGetName();

    /*
     * Allocate and initialize controlling variables
     */

    PollCreate(&pdata);
    writePtr = NULL;
    stopping = 0;

    /*
     * Loop forever until signalled to shutdown and all
     * connections are complete and gracefully closed.
     */

    while (!stopping) {

        /*
         * Always listen to the trigger pipe. We could as well perform
         * in the writer thread async write operations, but for the
         * effect of reducing latency in connection threads, this is
         * not an issue. To keep things simple, we perform the
         * typically small write operations without testing for POLLOUT.
         */

        PollReset(&pdata);
        PollSet(&pdata, queuePtr->pipe[0], POLLIN, NULL);

        if (writePtr == NULL) {
            pollto = 30 * 1000;
        } else {
	    pollto = 0;
        }

        /*
         * wait for data
         */
        /*n =*/ PollWait(&pdata, pollto);

        /*
         * Select and drain the trigger pipe if necessary.
         */
        if (PollIn(&pdata, 0)) {
	    if (recv(queuePtr->pipe[0], &c, 1, 0) != 1) {
		Ns_Fatal("asynclogwriter: trigger recv() failed: %s",
			 ns_sockstrerror(ns_sockerrno));
	    }
	    if (queuePtr->stopped) {
		/*
		 * Drain the queue from everything
		 */
		for (curPtr = writePtr; curPtr;  curPtr = curPtr->nextPtr) {
		    int unused NS_GNUC_UNUSED = 
		      write(curPtr->fd, curPtr->buf, curPtr->bufsize);
		}
		writePtr = NULL;

		for (curPtr = queuePtr->sockPtr; curPtr;  curPtr = curPtr->nextPtr) {
		    int unused NS_GNUC_UNUSED = 
		      write(curPtr->fd, curPtr->buf, curPtr->bufsize);
		}
		queuePtr->sockPtr = NULL;

		/* 
		 * Notify the caller (normally
		 * NsAsyncWriterQueueDisable()) that we are done
		 */
		Ns_CondBroadcast(&queuePtr->cond);
	    }
	}

        /*
         * Write to all available file descriptors
         */

        curPtr = writePtr;
        writePtr = NULL;

        while (curPtr != NULL) {

            nextPtr = curPtr->nextPtr;
            err = status = NS_OK;

	    /*
	     * write the actual data and allow for partial write operations.
	     */
	    n = write(curPtr->fd, curPtr->buf, curPtr->bufsize);
	    if (n < 0) {
		err = errno;
		status = NS_ERROR;
	    } else {
		curPtr->size -= n;
		curPtr->nsent += n;
		curPtr->bufsize -= n;
		if (curPtr->data) {
		    curPtr->buf += n;
		}
	    }

            if (status != NS_OK) {
                AsyncWriterRelease(curPtr, SOCK_WRITEERROR, err);
                queuePtr->queuesize--;
            } else {

		/*
		 * The write operation was successful. Check if there
		 * is some remaining data to write. If not we are done
		 * with this request can can release the write buffer.
		 */
                if (curPtr->size > 0) {
                    Push(curPtr, writePtr);
                } else {
                    AsyncWriterRelease(curPtr, 0, 0);
                    queuePtr->queuesize--;
                }
            }

            curPtr = nextPtr;
        }


	/*
	 * Check for shutdown
	 */
        stopping = queuePtr->shutdown;
	if (stopping) {
	    curPtr = queuePtr->sockPtr;
	    assert(writePtr == NULL);
	    while (curPtr != NULL) {
	      int unused NS_GNUC_UNUSED = 
		write(curPtr->fd, curPtr->buf, curPtr->bufsize);
		curPtr = curPtr->nextPtr;
	    }
	} else {
	    /*
	     * Add fresh jobs to the writer queue. This means actually to
	     * move jobs from queuePtr->sockPtr (kept name for being able
	     * to use the same queue as above) to the currently active
	     * jobs in queuePtr->curPtr.
	     */
	    Ns_MutexLock(&queuePtr->lock);
	    curPtr = queuePtr->sockPtr;
	    queuePtr->sockPtr = NULL;
	    while (curPtr != NULL) {
		nextPtr = curPtr->nextPtr;
		Push(curPtr, writePtr);
		queuePtr->queuesize++;
		curPtr = nextPtr;
	    }
	    queuePtr->curPtr = writePtr;
	    Ns_MutexUnlock(&queuePtr->lock);
	}

    }

    PollFree(&pdata);
    
    queuePtr->stopped = 1;
    Ns_Log(Notice, "exiting");

}
