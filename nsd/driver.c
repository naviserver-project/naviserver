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
 * The following are valid driver state flags.
 */

#define DRIVER_STARTED           1u
#define DRIVER_STOPPED           2u
#define DRIVER_SHUTDOWN          4u
#define DRIVER_FAILED            8u

/*
 * The following maintains Host header to server mappings.
 */

typedef struct ServerMap {
    NsServer *servPtr;
    char      location[1];
} ServerMap;

/*
 * The following maintains the spooler state mapping
 */
static const SpoolerStateMap spoolerStateMap[] = {
    {SPOOLER_CLOSE,        SOCK_CLOSE},
    {SPOOLER_READERROR,    SOCK_READERROR},
    {SPOOLER_WRITEERROR,   SOCK_WRITEERROR},
    {SPOOLER_CLOSETIMEOUT, SOCK_CLOSETIMEOUT},
    {SPOOLER_OK,           SOCK_READY}
};
/*
 * The following structure manages polling.  The PollIn macro is
 * used for the common case of checking for readability.
 */

typedef struct PollData {
    unsigned int nfds;          /* Number of fd's being monitored. */
    unsigned int maxfds;        /* Max fd's (will grow as needed). */
    struct pollfd *pfds;        /* Dynamic array of poll struct's. */
    Ns_Time timeout;            /* Min timeout, if any, for next spin. */
} PollData;

#define PollIn(ppd, i)           (((ppd)->pfds[(i)].revents & POLLIN)  == POLLIN )
#define PollOut(ppd, i)          (((ppd)->pfds[(i)].revents & POLLOUT) == POLLOUT)
#define PollHup(ppd, i)          (((ppd)->pfds[(i)].revents & POLLHUP) == POLLHUP)

/*
 * Async writer definitons
 */

typedef struct AsyncWriter {
    Ns_Mutex      lock;        /* Lock around writer queues */
    SpoolerQueue *firstPtr;    /* List of writer threads */
} AsyncWriter;

/* AsyncWriteData is similar to WriterSock */
typedef struct AsyncWriteData {
    struct AsyncWriteData *nextPtr;
    char                  *data;
    int                    fd;
    Tcl_WideInt            nsent;
    size_t                 size;
    size_t                 bufsize;
    const char            *buf;
} AsyncWriteData;

static AsyncWriter *asyncWriter = NULL;

/*
 * Static functions defined in this file.
 */

static Ns_ThreadProc DriverThread;
static Ns_ThreadProc SpoolerThread;
static Ns_ThreadProc WriterThread;
static Ns_ThreadProc AsyncWriterThread;

static Tcl_ObjCmdProc WriterListObjCmd;
static Tcl_ObjCmdProc WriterSizeObjCmd;
static Tcl_ObjCmdProc WriterStreamingObjCmd;
static Tcl_ObjCmdProc WriterSubmitObjCmd;
static Tcl_ObjCmdProc WriterSubmitFileObjCmd;

static DrvWriter *DriverWriterFromObj(Tcl_Obj *driverObj)
    NS_GNUC_NONNULL(1);

static NS_SOCKET DriverListen(Driver *drvPtr)
    NS_GNUC_NONNULL(1);
static NS_DRIVER_ACCEPT_STATUS DriverAccept(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static ssize_t DriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1);
static bool    DriverKeep(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void    DriverClose(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode DriverInit(const char *server, const char *moduleName, const char *threadName,
                                const Ns_DriverInitData *init,
                                NsServer *servPtr, const char *path, const char *bindaddr,
                                const char *defserver, const char *address, const char *host,
                                int driverCount)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(6)
    NS_GNUC_NONNULL(7) NS_GNUC_NONNULL(9) NS_GNUC_NONNULL(10);

static void  SockSetServer(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static SockState SockAccept(Driver *drvPtr, Sock **sockPtrPtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1);
static Ns_ReturnCode SockQueue(Sock *sockPtr, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);

static Sock *SockNew(Driver *drvPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;
static void  SockRelease(Sock *sockPtr, SockState reason, int err)
    NS_GNUC_NONNULL(1);

static void  SockError(Sock *sockPtr, SockState reason, int err);
static void  SockSendResponse(Sock *sockPtr, int code, const char *errMsg)
    NS_GNUC_NONNULL(1);
static void  SockTrigger(NS_SOCKET sock);
static void  SockTimeout(Sock *sockPtr, const Ns_Time *nowPtr, long timeout)
    NS_GNUC_NONNULL(1);
static void  SockClose(Sock *sockPtr, int keep)
    NS_GNUC_NONNULL(1);
static SockState SockRead(Sock *sockPtr, int spooler, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);
static SockState SockParse(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void SockPoll(Sock *sockPtr, short type, PollData *pdata)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static int  SockSpoolerQueue(Driver *drvPtr, Sock *sockPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void SpoolerQueueStart(SpoolerQueue *queuePtr, Ns_ThreadProc *proc)
    NS_GNUC_NONNULL(2);
static void SpoolerQueueStop(SpoolerQueue *queuePtr, const Ns_Time *timeoutPtr, const char *name)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static void PollCreate(PollData *pdata)
    NS_GNUC_NONNULL(1);
static void PollFree(PollData *pdata)
    NS_GNUC_NONNULL(1);
static void PollReset(PollData *pdata)
    NS_GNUC_NONNULL(1);
static NS_POLL_NFDS_TYPE PollSet(PollData *pdata, NS_SOCKET sock, short type, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);
static int PollWait(const PollData *pdata, int waittime)
    NS_GNUC_NONNULL(1);
static bool ChunkedDecode(Request *reqPtr, bool update)
    NS_GNUC_NONNULL(1);
static WriterSock *WriterSockRequire(const Conn *connPtr)
    NS_GNUC_NONNULL(1);
static void WriterSockRelease(WriterSock *wrSockPtr)
    NS_GNUC_NONNULL(1);
static SpoolerState WriterReadFromSpool(WriterSock *curPtr)
    NS_GNUC_NONNULL(1);
static SpoolerState WriterSend(WriterSock *curPtr, int *err)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void AsyncWriterRelease(AsyncWriteData *wdPtr)
    NS_GNUC_NONNULL(1);

static void WriteError(const char *msg, int fd, size_t wantWrite, ssize_t written)
    NS_GNUC_NONNULL(1);

static size_t EndOfHeader(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void RequestNew(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void RequestFree(Sock *sockPtr)
    NS_GNUC_NONNULL(1);
static void LogBuffer(Ns_LogSeverity severity, const char *msg, const char *buffer, size_t len)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * Static variables defined in this file.
 */

Ns_LogSeverity Ns_LogTaskDebug;
Ns_LogSeverity Ns_LogRequestDebug;
Ns_LogSeverity Ns_LogConnchanDebug;

static Ns_LogSeverity   DriverDebug;        /* Severity at which to log verbose debugging. */
static Tcl_HashTable    hosts;              /* Host header to server table */
static const ServerMap *defMapPtr   = NULL; /* Default srv when not found in table */
static Ns_Mutex         reqLock     = NULL; /* Lock for request free list */
static Ns_Mutex         writerlock  = NULL; /* Lock updating streamin information in the writer */
static Request         *firstReqPtr = NULL; /* Free list of request structures */
static Driver          *firstDrvPtr = NULL; /* First in list of all drivers */

#define Push(x, xs) ((x)->nextPtr = (xs), (xs) = (x))


static void
WriteError(const char *msg, int fd, size_t wantWrite, ssize_t written)
{
    fprintf(stderr, "%s: Warning: wanted to write %" PRIuz
            " bytes, wrote %ld to file descriptor %d\n",
            msg, wantWrite, (long)written, fd);
}



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
    Ns_LogTaskDebug = Ns_CreateLogSeverity("Debug(task)");
    Ns_LogRequestDebug = Ns_CreateLogSeverity("Debug(request)");
    Ns_LogConnchanDebug = Ns_CreateLogSeverity("Debug(connchan)");
    Ns_MutexInit(&reqLock);
    Ns_MutexSetName2(&reqLock, "ns:driver", "freelist");
    Ns_MutexSetName2(&writerlock, "ns:writer", "stream");
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

Ns_ReturnCode
Ns_DriverInit(const char *server, const char *module, const Ns_DriverInitData *init)
{
    Ns_ReturnCode  status = NS_OK;
    NsServer      *servPtr = NULL;

    NS_NONNULL_ASSERT(module != NULL);
    NS_NONNULL_ASSERT(init != NULL);

    /*
     * If a server is provided, servPtr must be set.
     */
    if (server != NULL) {
        servPtr = NsGetServer(server);
        if (unlikely(servPtr == NULL)) {
            Ns_Log(Bug, "cannot lookup server structure for server: %s", module);
            status = NS_ERROR;
        }
    }

    if (status == NS_OK && init->version < NS_DRIVER_VERSION_4) {
        Ns_Log(Warning, "%s: driver version is too old (version %d), Version 4 is recommended",
               module, init->version);
    }
#ifdef HAVE_IPV6
    if (status == NS_OK && init->version < NS_DRIVER_VERSION_3) {
        Ns_Log(Error, "%s: driver version is too old (version %d) and does not support IPv6",
               module, init->version);
        status = NS_ERROR;
    }
#endif
    if (status == NS_OK && init->version < NS_DRIVER_VERSION_2) {
        Ns_Log(Error, "%s: version field of driver is invalid: %d",
               module, init->version);
        status = NS_ERROR;
    }

    if (status == NS_OK) {
        const char *path, *host, *address, *bindaddr, *defserver;
        bool        noHostNameGiven;
        int         nrDrivers;
        Ns_Set     *set;

        path = ((init->path != NULL) ? init->path : Ns_ConfigGetPath(server, module, (char *)0));

        set = Ns_ConfigCreateSection(path);

        /*
         * Determine the hostname used for the local address to bind
         * to and/or the HTTP location string.
         */

        host = Ns_ConfigGetValue(path, "hostname");
        noHostNameGiven = (host == NULL);
        bindaddr = address = Ns_ConfigGetValue(path, "address");
        if (bindaddr == NULL) {
            bindaddr = address = NS_IP_UNSPECIFIED;
        }
        defserver = Ns_ConfigGetValue(path, "defaultserver");

        /*
         * If the listen address was not specified, attempt to determine it
         * through a DNS lookup of the specified hostname or the server's
         * primary hostname.
         */
        if (address == NULL) {
            const char  *hostName;
            Tcl_DString  ds;

            Tcl_DStringInit(&ds);
            hostName = noHostNameGiven ? Ns_InfoHostname() : host;

            if (Ns_GetAddrByHost(&ds, hostName) == NS_TRUE) {
                if (path != NULL) {
                    address = ns_strdup(Tcl_DStringValue(&ds));
                    Ns_SetUpdate(set, "address", address);
                }
            }

            /*
             * Finally, if no hostname was specified, set it to the hostname
             * derived from the lookup(s) above.
             */

            if (host == NULL) {
                Tcl_DStringTrunc(&ds, 0);

                if (Ns_GetHostByAddr(&ds, address) == NS_TRUE) {
                    host = ns_strdup(Tcl_DStringValue(&ds));
                }
            }
            Tcl_DStringFree(&ds);
        }

        /*
         * If the hostname was not specified and not determined by the lookup
         * above, set it to the specified or derived IP address string.
         */

        if (host == NULL) {
            host = address;
        }

        if (noHostNameGiven && host != NULL && path != NULL) {
            Ns_SetUpdate(set, "hostname", host);
        }

        nrDrivers = Ns_ConfigIntRange(path, "driverthreads", 1, 1, 32);
        if (nrDrivers > 1) {
#if !defined(SO_REUSEPORT)
            Ns_Log(Warning,
                   "server %s module %d requests %d driverthreads, but is not supported by the operating system",
                   server, module, nrDrivers);
            Ns_SetUpdate(set, "driverthreads", "1");
            nrDrivers = 1;
#endif
        }

        /*
         * The common parameters are determined, create the driver thread(s)
         */
        {
            size_t maxModuleNameLength = strlen(module) + (size_t)TCL_INTEGER_SPACE + 1u;
            char  *moduleName = ns_malloc(maxModuleNameLength);
            int    i;

            for (i = 0; i < nrDrivers; i++) {
                snprintf(moduleName, maxModuleNameLength, "%s:%d", module, i);
                status = DriverInit(server, module, moduleName, init,
                                    servPtr, path, bindaddr, defserver, address, host, i);
                if (status != NS_OK) {
                    break;
                }
            }
            ns_free(moduleName);
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverInit --
 *
 *      Helper function of Ns_DriverInit. This function actuall allocates and
 *      initialized the driver structure.
 *
 * Results:
 *      NS_OK if initialized, NS_ERROR if config or other error.
 *
 * Side effects:
 *      Listen socket will be opened later in NsStartDrivers.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
DriverInit(const char *server, const char *moduleName, const char *threadName,
           const Ns_DriverInitData *init,
           NsServer *servPtr, const char *path, const char *bindaddr,
           const char *defserver, const char *address, const char *host,
           int driverCount)
{
    const char     *defproto;
    ServerMap      *mapPtr;
    Driver         *drvPtr;
    DrvWriter      *wrPtr;
    DrvSpooler     *spPtr;
    int             i, n;
    unsigned short  defport;

    NS_NONNULL_ASSERT(threadName != NULL);
    NS_NONNULL_ASSERT(init != NULL);
    NS_NONNULL_ASSERT(path != NULL);
    NS_NONNULL_ASSERT(bindaddr != NULL);
    NS_NONNULL_ASSERT(address != NULL);
    NS_NONNULL_ASSERT(host != NULL);

    /*
     * Set the protocol and port defaults.
     */
    if (init->protocol != NULL) {
        defproto = init->protocol;
        defport = init->defaultPort;
    } else {
        defproto = "unknown";
        defport = 0u;
    }
    Ns_Log(DriverDebug, "DriverInit server <%s> threadName %s proto %s port %hu",
           server, threadName, defproto, defport);

    /*
     * Allocate a new driver instance and set configurable parameters.
     */

    drvPtr = ns_calloc(1u, sizeof(Driver));

    Ns_MutexInit(&drvPtr->lock);
    Ns_MutexSetName2(&drvPtr->lock, "ns:drv", threadName);

    Ns_MutexInit(&drvPtr->spooler.lock);
    Ns_MutexSetName2(&drvPtr->spooler.lock, "ns:drv:spool", threadName);

    Ns_MutexInit(&drvPtr->writer.lock);
    Ns_MutexSetName2(&drvPtr->writer.lock, "ns:drv:writer", threadName);

    if (ns_sockpair(drvPtr->trigger) != 0) {
        Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
    }

    drvPtr->server         = server;
    drvPtr->type           = init->name;
    drvPtr->moduleName     = ns_strdup(moduleName);
    drvPtr->threadName     = ns_strdup(threadName);
    drvPtr->listenProc     = init->listenProc;
    drvPtr->acceptProc     = init->acceptProc;
    drvPtr->recvProc       = init->recvProc;
    drvPtr->sendProc       = init->sendProc;
    drvPtr->sendFileProc   = init->sendFileProc;
    drvPtr->keepProc       = init->keepProc;
    drvPtr->requestProc    = init->requestProc;
    drvPtr->closeProc      = init->closeProc;
    drvPtr->clientInitProc = init->clientInitProc;
    drvPtr->arg            = init->arg;
    drvPtr->opts           = init->opts;
    drvPtr->servPtr        = servPtr;

    drvPtr->bufsize        = (size_t)Ns_ConfigIntRange(path, "bufsize", 16384, 1024, INT_MAX);
    drvPtr->maxinput       = Ns_ConfigWideIntRange(path, "maxinput",
                                                   (Tcl_WideInt)1024*1024,
                                                   (Tcl_WideInt)1024,
                                                   LLONG_MAX);
    drvPtr->maxupload      = Ns_ConfigWideIntRange(path, "maxupload",
                                                   (Tcl_WideInt)0,
                                                   (Tcl_WideInt)0,
                                                   drvPtr->maxinput);
    drvPtr->readahead      = Ns_ConfigWideIntRange(path, "readahead",
                                                   (Tcl_WideInt)drvPtr->bufsize,
                                                   (Tcl_WideInt)drvPtr->bufsize,
                                                   drvPtr->maxinput);

    drvPtr->maxline        = Ns_ConfigIntRange(path, "maxline",      8192, 256, INT_MAX);
    drvPtr->maxheaders     = Ns_ConfigIntRange(path, "maxheaders",    128,   8, INT_MAX);
    drvPtr->maxqueuesize   = Ns_ConfigIntRange(path, "maxqueuesize", 1024,   1, INT_MAX);
    drvPtr->sendwait       = Ns_ConfigIntRange(path, "sendwait",       30,   1, INT_MAX);
    drvPtr->recvwait       = Ns_ConfigIntRange(path, "recvwait",       30,   1, INT_MAX);
    drvPtr->closewait      = Ns_ConfigIntRange(path, "closewait",       2,   0, INT_MAX);
    drvPtr->keepwait       = Ns_ConfigIntRange(path, "keepwait",        5,   0, INT_MAX);
    drvPtr->backlog        = Ns_ConfigIntRange(path, "backlog",       256,   1, INT_MAX);
    drvPtr->driverthreads  = Ns_ConfigIntRange(path, "driverthreads",   1,   1, 32);
    drvPtr->reuseport      = Ns_ConfigBool(path,     "reuseport",       NS_FALSE);

    drvPtr->acceptsize     = Ns_ConfigIntRange(path, "acceptsize", drvPtr->backlog, 1, INT_MAX);

    drvPtr->keepmaxuploadsize   = (size_t)Ns_ConfigIntRange(path, "keepalivemaxuploadsize",
                                                            0, 0, INT_MAX);
    drvPtr->keepmaxdownloadsize = (size_t)Ns_ConfigIntRange(path, "keepalivemaxdownloadsize",
                                                            0, 0, INT_MAX);

    if (drvPtr->driverthreads > 1) {
#if !defined(SO_REUSEPORT)
        drvPtr->driverthreads = 1;
        drvPtr->reuseport = NS_FALSE;
#else
        /*
         * When driver threads > 1, reuseport has to be active
         */
        drvPtr->reuseport = NS_TRUE;
#endif
    }
    if (drvPtr->reuseport) {
        /*
         * Reuseport was specified
         */
#if !defined(SO_REUSEPORT)
        Ns_Log(Warning,
               "parameter %s reuseport was specified, but is not supported by the operating system",
               path);
        drvPtr->reuseport = NS_FALSE;
#endif
    }

    drvPtr->uploadpath = ns_strdup(Ns_ConfigString(path, "uploadpath", nsconf.tmpDir));

    /*
     * If activated, maxupload has to be at least readahead bytes. Tell the
     * user in case the config values are overruled.
     */
    if ((drvPtr->maxupload > 0) &&
        (drvPtr->maxupload < drvPtr->readahead)) {
        Ns_Log(Warning,
               "parameter %s maxupload % " TCL_LL_MODIFIER
               "d invalid; can be either 0 or must be >= %" TCL_LL_MODIFIER
               "d (size of readahead)",
               path, drvPtr->maxupload, drvPtr->readahead);
        drvPtr->maxupload = drvPtr->readahead;
    }

    /*
     * Determine the port and then set the HTTP location string either
     * as specified in the config file or constructed from the
     * protocol, hostname and port.
     */

    drvPtr->bindaddr = bindaddr;
    drvPtr->protocol = ns_strdup(defproto);
    drvPtr->address  = ns_strdup(address);
    drvPtr->port     = (unsigned short)Ns_ConfigIntRange(path, "port", (int)defport, 0, 65535);
    drvPtr->location = Ns_ConfigGetValue(path, "location");

    if (drvPtr->location != NULL && (strstr(drvPtr->location, "://") != NULL)) {
        drvPtr->location = ns_strdup(drvPtr->location);
    } else {
        Tcl_DString ds, *dsPtr = &ds;

        Ns_DStringInit(dsPtr);
        Ns_HttpLocationString(dsPtr, drvPtr->protocol, host, drvPtr->port, defport);
        drvPtr->location = Ns_DStringExport(dsPtr);
    }

    drvPtr->nextPtr = firstDrvPtr;
    firstDrvPtr = drvPtr;

    /*
     * Add extra headers, which have to be of the form of
     * attribute/value pairs.
     */
    {
        const char *extraHeaders = Ns_ConfigGetValue(path, "extraheaders");

        if (extraHeaders != NULL) {
            int       objc;
            Tcl_Obj **objv, *headers = Tcl_NewStringObj(extraHeaders, -1);
            int       result = Tcl_ListObjGetElements(NULL, headers, &objc, &objv);

            if (result != TCL_OK || objc % 2 != 0) {
                Ns_Log(Warning, "Ignoring invalid value for extraheaders: %s", extraHeaders);
            } else {
                Tcl_DString ds, *dsPtr = &ds;

                Ns_DStringInit(dsPtr);
                for (i = 0; i < objc; i +=2) {
                    (void) Ns_DStringVarAppend(dsPtr,
                                               Tcl_GetString(objv[i]), ": ",
                                               Tcl_GetString(objv[i+1]), "\r\n",
                                               (char *)0);
                }
                drvPtr->extraHeaders = Ns_DStringExport(dsPtr);
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
               "for uploads >= %" TCL_LL_MODIFIER "d bytes", threadName,
               spPtr->threads, drvPtr->readahead);

        for (i = 0; i < spPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1u, sizeof(SpoolerQueue));
            char          buffer[100];

            sprintf(buffer, "ns:driver:spooler:%d", i);
            Ns_MutexSetName2(&queuePtr->lock, buffer, "queue");
            queuePtr->id = i;
            Push(queuePtr, spPtr->firstPtr);
        }
    } else {
        Ns_Log(Notice, "%s: enable %d spooler thread(s) ",
               threadName, spPtr->threads);
    }

    /*
     * Enable writer threads
     */

    wrPtr = &drvPtr->writer;
    wrPtr->threads = Ns_ConfigIntRange(path, "writerthreads", 0, 0, 32);

    if (wrPtr->threads > 0) {
        wrPtr->maxsize = (size_t)Ns_ConfigIntRange(path, "writersize",
                                                   1024*1024, 1024, INT_MAX);
        wrPtr->bufsize = (size_t)Ns_ConfigIntRange(path, "writerbufsize",
                                                   8192, 512, INT_MAX);
        wrPtr->doStream = Ns_ConfigBool(path, "writerstreaming", NS_FALSE)
            ? NS_WRITER_STREAM_ACTIVE : NS_WRITER_STREAM_NONE;
        Ns_Log(Notice, "%s: enable %d writer thread(s) "
               "for downloads >= %" PRIdz " bytes, bufsize=%" PRIdz " bytes, HTML streaming %d",
               threadName, wrPtr->threads, wrPtr->maxsize, wrPtr->bufsize, wrPtr->doStream);

        for (i = 0; i < wrPtr->threads; i++) {
            SpoolerQueue *queuePtr = ns_calloc(1u, sizeof(SpoolerQueue));
            char          buffer[100];

            sprintf(buffer, "ns:driver:writer:%d", i);
            Ns_MutexSetName2(&queuePtr->lock, buffer, "queue");
            queuePtr->id = i;
            Push(queuePtr, wrPtr->firstPtr);
        }
    } else {
        Ns_Log(Notice, "%s: enable %d writer thread(s) ",
               threadName, wrPtr->threads);
    }

    /*
     * Map Host headers for drivers not bound to servers.
     */

    if (server == NULL && driverCount == 0) {
        const Ns_Set *lset;
        size_t        j;
        Tcl_DString ds, *dsPtr = &ds;

        if (defserver == NULL) {
            Ns_Fatal("%s: virtual servers configured,"
                     " but %s has no defaultserver defined", moduleName, path);
        }
        assert(defserver != NULL);

        defMapPtr = NULL;
        path = Ns_ConfigGetPath(NULL, moduleName, "servers", (char *)0);
        lset = Ns_ConfigGetSection(path);

        Ns_DStringInit(dsPtr);
        for (j = 0u; lset != NULL && j < Ns_SetSize(lset); ++j) {
            server  = Ns_SetKey(lset, j);
            host    = Ns_SetValue(lset, j);
            servPtr = NsGetServer(server);
            if (servPtr == NULL) {
                Ns_Log(Error, "%s: no such server: %s", moduleName, server);
            } else {
                Tcl_HashEntry  *hPtr = Tcl_CreateHashEntry(&hosts, host, &n);

                if (n == 0) {
                    Ns_Log(Error, "%s: duplicate host map: %s", moduleName, host);
                } else {
                    (void) Ns_DStringVarAppend(dsPtr, drvPtr->protocol, "://", host, (char *)0);
                    mapPtr = ns_malloc(sizeof(ServerMap) + (size_t)ds.length);
                    mapPtr->servPtr  = servPtr;
                    memcpy(mapPtr->location, ds.string, (size_t)ds.length + 1u);
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
            Ns_Fatal("%s: default server %s not defined in %s", moduleName, server, path);
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

    /*
     * Signal and wait for each driver to start.
     */
    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {

        if (drvPtr->port == 0u) {
            /*
             * Don't start a driver having port zero.
             */
            continue;
        }

        Ns_ThreadCreate(DriverThread, drvPtr, 0, &drvPtr->thread);
        Ns_MutexLock(&drvPtr->lock);
        while ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            Ns_CondWait(&drvPtr->cond, &drvPtr->lock);
        }
        /*if ((drvPtr->flags & DRIVER_FAILED)) {
          status = NS_ERROR;
          }*/
        Ns_MutexUnlock(&drvPtr->lock);
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
    Driver         *drvPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    NsAsyncWriterQueueDisable(NS_TRUE);

    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        if ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            continue;
        }
        Ns_MutexLock(&drvPtr->lock);
        Ns_Log(Notice, "[driver:%s]: stopping", drvPtr->threadName);
        drvPtr->flags |= DRIVER_SHUTDOWN;
        Ns_CondBroadcast(&drvPtr->cond);
        Ns_MutexUnlock(&drvPtr->lock);
        SockTrigger(drvPtr->trigger[1]);
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
    const Driver *drvPtr;

    Ns_Log(Notice, "driver: stopping writer and spooler threads");

    /*
     * Shutdown all spooler and writer threads
     */
    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        Ns_Time timeout;

        if ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            continue;
        }
        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, nsconf.shutdowntimeout, 0);

        SpoolerQueueStop(drvPtr->writer.firstPtr, &timeout, "writer");
        SpoolerQueueStop(drvPtr->spooler.firstPtr, &timeout, "spooler");
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DriverInfoObjCmd --
 *
 *      Return public info of all drivers.
 *      Subcommand of NsTclDriverObjCmd.
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverInfoObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);
        Tcl_HashTable names;     /* names of the driver modules without duplicates */

        Tcl_InitHashTable(&names, TCL_STRING_KEYS);

        /*
         * Iterate over all modules, not necessarily all driver threads
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            int isNew = 0;

            (void)Tcl_CreateHashEntry(&names, drvPtr->moduleName, &isNew);
            if (isNew == 1) {
                Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("module", 6));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->moduleName, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("type", 4));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->type, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("server", 6));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->server != NULL ? drvPtr->server : "", -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("location", 8));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->location, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("address", 7));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->address, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("protocol", 8));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->protocol, -1));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("sendwait", 8));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewLongObj(drvPtr->sendwait));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("recvwait", 8));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewLongObj(drvPtr->sendwait));

                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("extraheaders", 12));
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->extraHeaders, -1));

                Tcl_ListObjAppendElement(interp, resultObj, listObj);
            }
        }
        Tcl_SetObjResult(interp, resultObj);
        Tcl_DeleteHashTable(&names);
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * DriverStatsObjCmd --
 *
 *      Return statistics of all drivers.
 *      Subcommand of NsTclDriverObjCmd.
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverStatsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {

        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);

        /*
         * Iterate over all drivers and collect results.
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);


            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("thread", 6));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->threadName, -1));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("module", 6));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(drvPtr->moduleName, -1));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("received", 8));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.received));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("spooled", 7));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.spooled));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("partial", 7));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.partial));

            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj("errors", 6));
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewWideIntObj(drvPtr->stats.errors));

            Tcl_ListObjAppendElement(interp, resultObj, listObj);
        }
        Tcl_SetObjResult(interp, resultObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverThreadsObjCmd --
 *
 *      Return the names of driver threads
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverThreadsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);

        /*
         * Iterate over all drivers and collect results.
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(drvPtr->threadName, -1));
        }
        Tcl_SetObjResult(interp, resultObj);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * DriverNamesObjCmd --
 *
 *      Return the names of drivers.
 *
 * Results:
 *      Standard Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
DriverNamesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Driver *drvPtr;
        Tcl_Obj      *resultObj = Tcl_NewListObj(0, NULL);
        Tcl_HashTable names;     /* names of the drivers without duplicates */

        Tcl_InitHashTable(&names, TCL_STRING_KEYS);

        /*
         * Iterate over all drivers and collect results.
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            int            isNew;

            (void)Tcl_CreateHashEntry(&names, drvPtr->moduleName, &isNew);
            if (isNew == 1) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(drvPtr->moduleName, -1));
            }
        }
        Tcl_SetObjResult(interp, resultObj);
        Tcl_DeleteHashTable(&names);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclDriverObjCmd -
 *
 *      Give information about drivers. Currently, just the statistics.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclDriverObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"info",       DriverInfoObjCmd},
        {"names",      DriverNamesObjCmd},
        {"threads",    DriverThreadsObjCmd},
        {"stats",      DriverStatsObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
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
NsWakeupDriver(const Driver *drvPtr) {
    NS_NONNULL_ASSERT(drvPtr != NULL);
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
NsWaitDriversShutdown(const Ns_Time *toPtr)
{
    Driver       *drvPtr;
    Ns_ReturnCode status = NS_OK;

    for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
        if ((drvPtr->flags & DRIVER_STARTED) == 0u) {
            continue;
        }
        Ns_MutexLock(&drvPtr->lock);
        while ((drvPtr->flags & DRIVER_STOPPED) == 0u && status == NS_OK) {
            status = Ns_CondTimedWait(&drvPtr->cond, &drvPtr->lock, toPtr);
        }
        Ns_MutexUnlock(&drvPtr->lock);
        if (status != NS_OK) {
            Ns_Log(Warning, "[driver:%s]: shutdown timeout", drvPtr->threadName);
        } else {
            Ns_Log(Notice, "[driver:%s]: stopped", drvPtr->threadName);
            Ns_ThreadJoin(&drvPtr->thread, NULL);
            drvPtr->thread = NULL;
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetRequest --
 *
 *      Return the request buffer, reading it if necessary (i.e., if not an
 *      async read-ahead connection). This function is called at the start of
 *      connection processing.
 * Results:
 *      Pointer to Request structure or NULL on error.
 *
 * Side effects:
 *      May wait for content to arrive if necessary.
 *
 *----------------------------------------------------------------------
 */
Request *
NsGetRequest(Sock *sockPtr, const Ns_Time *nowPtr)
{
    Request *reqPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    /*
     * The underlying Request structure is allocated by RequestNew(), which
     * must be called for the sockPtr prior to calling this function. reqPtr
     * should be NULL just in error cases.
     */
    reqPtr = sockPtr->reqPtr;

    if (likely(reqPtr != NULL)) {

        if (likely(reqPtr->request.line != NULL)) {
            Ns_Log(DriverDebug, "NsGetRequest got the preparsed request <%s> from the driver",
                   reqPtr->request.line);

        } else if (sockPtr->drvPtr->requestProc == NULL) {
            /*
             * Non-HTTP driver can send the drvPtr->requestProc to perform
             * their own request handling.
             */
            SockState status;

            Ns_Log(DriverDebug, "NsGetRequest has to read+parse the request");
            /*
             * We have no parsed request so far. So, do it now.
             */
            do {
                Ns_Log(DriverDebug, "NsGetRequest calls SockRead");
                status = SockRead(sockPtr, 0, nowPtr);
            } while (status == SOCK_MORE);

            /*
             * If anything went wrong, clean the request provided by
             * SockRead() and flag the error by returing NULL.
             */
            if (status != SOCK_READY) {
                if (sockPtr->reqPtr != NULL) {
                    Ns_Log(DriverDebug, "NsGetRequest calls RequestFree");
                    RequestFree(sockPtr);
                }
                reqPtr = NULL;
            }
        } else {
            Ns_Log(DriverDebug, "NsGetRequest found driver specific request Proc, probably from a non-HTTP driver");
        }
    } else {
        Ns_Log(DriverDebug, "NsGetRequest has reqPtr NULL");
    }

    return reqPtr;
}



/*
 *----------------------------------------------------------------------
 *
 * NsSockClose --
 *
 *      Return a connection to the DriverThread for closing or keepalive.
 *      "keep" might be NS_TRUE/NS_FALSE or -1 if undecided.
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
    Driver *drvPtr;
    bool    trigger = NS_FALSE;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    drvPtr = sockPtr->drvPtr;

    Ns_Log(DriverDebug, "NsSockClose sockPtr %p keep %d", (void *)sockPtr, keep);

    SockClose(sockPtr, keep);
    /*
     * Free the request, unless it is from a non-HTTP driver (who might not
     * fill out the request structure).
     */
    if (sockPtr->reqPtr != NULL) {
        Ns_Log(DriverDebug, "NsSockClose calls RequestFree");
        RequestFree(sockPtr);
    }

    Ns_MutexLock(&drvPtr->lock);
    if (drvPtr->closePtr == NULL) {
        trigger = NS_TRUE;
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
 *      File description of socket, or NS_INVALID_SOCKET on error.
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

    NS_NONNULL_ASSERT(drvPtr != NULL);

    sock = (*drvPtr->listenProc)((Ns_Driver *) drvPtr,
                                 drvPtr->bindaddr,
                                 drvPtr->port,
                                 drvPtr->backlog,
                                 drvPtr->reuseport);
    if (sock == NS_INVALID_SOCKET) {
        Ns_Log(Error, "%s: failed to listen on [%s]:%d: %s",
               drvPtr->threadName, drvPtr->address, drvPtr->port,
               ns_sockstrerror(ns_sockerrno));
    } else {
        Ns_Log(Notice,
#ifdef HAVE_IPV6
               "%s: listening on [%s]:%d",
#else
               "%s: listening on %s:%d",
#endif
               drvPtr->threadName, drvPtr->address, drvPtr->port);
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
    socklen_t n = (socklen_t)sizeof(struct NS_SOCKADDR_STORAGE);

    NS_NONNULL_ASSERT(sockPtr != NULL);

    return (*sockPtr->drvPtr->acceptProc)((Ns_Sock *) sockPtr,
                                          sockPtr->drvPtr->sock,
                                          (struct sockaddr *) &(sockPtr->sa), &n);
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
    ssize_t result;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    timeout.sec = sockPtr->drvPtr->recvwait;
    timeout.usec = 0;

    if (likely(sockPtr->drvPtr->recvProc != NULL)) {
        result = (*sockPtr->drvPtr->recvProc)((Ns_Sock *) sockPtr, bufs, nbufs, &timeout, 0u);
    } else {
        Ns_Log(Warning, "driver: no recvProc registered for driver %s", sockPtr->drvPtr->threadName);
        result = -1;
    }

    return result;
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
NsDriverSend(Sock *sockPtr, const struct iovec *bufs, int nbufs, unsigned int flags)
{
    Ns_Time timeout;
    ssize_t result;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    assert(sockPtr->drvPtr != NULL);

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

    if (likely(sockPtr->drvPtr->sendProc != NULL)) {
        result = (*sockPtr->drvPtr->sendProc)((Ns_Sock *) sockPtr, bufs, nbufs,
                                              &timeout, flags);
    } else {
        Ns_Log(Warning, "connchan: no sendProc registered for driver %s", sockPtr->drvPtr->threadName);
        result = -1;
    }

    return result;
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
NsDriverSendFile(Sock *sockPtr, Ns_FileVec *bufs, int nbufs, unsigned int flags)
{
    const Driver *drvPtr = sockPtr->drvPtr;
    ssize_t       n;
    Ns_Time       timeout;

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
 *      NS_TRUE if the socket is OK for keepalive, NS_FALSE if this is not possible.
 *
 * Side effects:
 *      Depends on driver.
 *
 *----------------------------------------------------------------------
 */

static bool
DriverKeep(Sock *sockPtr)
{
    NS_NONNULL_ASSERT(sockPtr != NULL);
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
    NS_NONNULL_ASSERT(sockPtr != NULL);
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
    char           charBuffer[1], drain[1024];
    int            pollto, accepted;
    bool           stopping;
    unsigned int   flags;
    Sock          *sockPtr, *closePtr, *nextPtr, *waitPtr, *readPtr;
    PollData       pdata;

    assert(arg != NULL);

    Ns_ThreadSetName("-driver:%s-", drvPtr->threadName);
    Ns_Log(Notice, "starting");

    flags = DRIVER_STARTED;
    drvPtr->sock = DriverListen(drvPtr);

    if (drvPtr->sock == NS_INVALID_SOCKET) {
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

    PollCreate(&pdata);
    Ns_GetTime(&now);
    closePtr = waitPtr = readPtr = NULL;
    stopping = ((flags & DRIVER_SHUTDOWN) != 0u);

    if (!stopping) {
        Ns_Log(Notice, "driver: accepting connections");
    }

    while (!stopping) {
        int n;

        /*
         * Set the bits for all active drivers if a connection
         * isn't already pending.
         */

        PollReset(&pdata);
        (void)PollSet(&pdata, drvPtr->trigger[0], (short)POLLIN, NULL);

        if (likely(waitPtr == NULL)) {
            drvPtr->pidx = PollSet(&pdata, drvPtr->sock, (short)POLLIN, NULL);
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
                SockPoll(sockPtr, (short)POLLIN, &pdata);
            }
            for (sockPtr = closePtr; sockPtr != NULL; sockPtr = sockPtr->nextPtr) {
                SockPoll(sockPtr, (short)POLLIN, &pdata);
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

        Ns_Log(DriverDebug, "=== PollWait returned %d, trigger[0] %d", n, PollIn(&pdata, 0));

        if (PollIn(&pdata, 0) && unlikely(ns_recv(drvPtr->trigger[0], charBuffer, 1u, 0) != 1)) {
            const char *errstr = ns_sockstrerror(ns_sockerrno);

            Ns_Fatal("driver: trigger ns_recv() failed: %s", errstr);
        }
        /*
         * Check whether we should re-animate some connection threads,
         * when e.g. the number of current threads dropped blow the
         * minimal value.  Perform this test on timeouts (n == 0;
         * just for safety reasons) or on explicit wakeup calls.
         */
        if ((n == 0) || PollIn(&pdata, 0)) {
            if (drvPtr->servPtr != NULL) {
                NsEnsureRunningConnectionThreads(drvPtr->servPtr, NULL);
            } else {
                Tcl_HashSearch       search;
                const Tcl_HashEntry *hPtr = Tcl_FirstHashEntry(&hosts, &search);
                /*
                 * In case, we have a "global" driver, we have to
                 * check all associated servers.
                 */
                while (hPtr != NULL) {
                    const ServerMap *mapPtr = Tcl_GetHashValue(hPtr);
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
                if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
                    /*
                     * Peer has closed the connection
                     */
                    SockRelease(sockPtr, SOCK_CLOSE, 0);
                } else if (likely(PollIn(&pdata, sockPtr->pidx))) {
                    /*
                     * Got some data
                     */
                    ssize_t recieved = ns_recv(sockPtr->sock, drain, sizeof(drain), 0);
                    if (recieved <= 0) {
                        Ns_Log(DriverDebug, "poll closewait pollin; sockrelease SOCK_READERROR (sock %d)",
                               sockPtr->sock);
                        SockRelease(sockPtr, SOCK_READERROR, 0);
                    } else {
                        Push(sockPtr, closePtr);
                    }
                } else if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                    /* no PollHup, no PollIn, maybe timeout */
                    Ns_Log(DriverDebug, "poll closewait timeout; sockrelease SOCK_CLOSETIMEOUT (sock %d)",
                           sockPtr->sock);
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

        while (likely(sockPtr != NULL)) {
            nextPtr = sockPtr->nextPtr;

            if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
                /*
                 * Peer has closed the connection
                 */
                SockRelease(sockPtr, SOCK_CLOSE, 0);

            } else if (unlikely(!PollIn(&pdata, sockPtr->pidx))
                       && ((sockPtr->reqPtr == NULL) || (sockPtr->reqPtr->leftover == 0u))) {
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
                if (likely((sockPtr->drvPtr->opts & NS_DRIVER_ASYNC) != 0u)) {
                    SockState s = SockRead(sockPtr, 0, &now);

                    /*
                     * Queue for connection processing if ready.
                     */

                    switch (s) {
                    case SOCK_SPOOL:
                        sockPtr->drvPtr->stats.spooled++;
                        if (SockSpoolerQueue(sockPtr->drvPtr, sockPtr) == 0) {
                            Push(sockPtr, readPtr);
                        }
                        break;

                    case SOCK_MORE:
                        sockPtr->drvPtr->stats.partial++;
                        SockTimeout(sockPtr, &now, sockPtr->drvPtr->recvwait);
                        Push(sockPtr, readPtr);
                        break;

                    case SOCK_READY:
                        if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                            Push(sockPtr, waitPtr);
                        }
                        break;

                        /*
                         * Already handled or normal cases
                         */
                    case SOCK_ENTITYTOOLARGE:
                    case SOCK_BADREQUEST:
                    case SOCK_BADHEADER:
                    case SOCK_TOOMANYHEADERS:
                    case SOCK_CLOSE:
                        SockRelease(sockPtr, s, errno);
                        break;

                        /*
                         * Exceptions
                         */
                    case SOCK_READERROR:
                    case SOCK_CLOSETIMEOUT:
                    case SOCK_ERROR:
                    case SOCK_READTIMEOUT:
                    case SOCK_SHUTERROR:
                    case SOCK_WRITEERROR:
                    case SOCK_WRITETIMEOUT:
                    default:
                        sockPtr->drvPtr->stats.errors++;
                        Ns_Log(Warning, "sockread returned unexpected result %d; close socket", s);
                        SockRelease(sockPtr, s, errno);
                        break;
                    }
                } else {
                    /*
                     * Potentially blocking driver, NS_DRIVER_ASYNC is not defined
                     */
                    if (Ns_DiffTime(&sockPtr->timeout, &now, &diff) <= 0) {
                        sockPtr->drvPtr->stats.errors++;
                        Ns_Log(Notice, "read-ahead has some data, no async sock read ===== diff time %ld",
                               Ns_DiffTime(&sockPtr->timeout, &now, &diff));
                        sockPtr->keep = NS_FALSE;
                        SockRelease(sockPtr, SOCK_READTIMEOUT, 0);
                    } else {
                        if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                            Push(sockPtr, waitPtr);
                        }
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
            SockState s;

            accepted = 0;
            while (accepted < drvPtr->acceptsize
                   && drvPtr->queuesize < drvPtr->maxqueuesize
                   && PollIn(&pdata, drvPtr->pidx)
                   && (s = SockAccept(drvPtr, &sockPtr, &now)) != SOCK_ERROR) {

                switch (s) {
                case SOCK_SPOOL:
                    sockPtr->drvPtr->stats.spooled++;
                    if (SockSpoolerQueue(sockPtr->drvPtr, sockPtr) == 0) {
                        Push(sockPtr, readPtr);
                    }
                    break;

                case SOCK_MORE:
                    sockPtr->drvPtr->stats.partial++;
                    SockTimeout(sockPtr, &now, sockPtr->drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;

                case SOCK_READY:
                    if (SockQueue(sockPtr, &now) == NS_TIMEOUT) {
                        Push(sockPtr, waitPtr);
                    }
                    break;

                case SOCK_BADHEADER:
                case SOCK_BADREQUEST:
                case SOCK_CLOSE:
                case SOCK_CLOSETIMEOUT:
                case SOCK_ENTITYTOOLARGE:
                case SOCK_ERROR:
                case SOCK_READERROR:
                case SOCK_READTIMEOUT:
                case SOCK_SHUTERROR:
                case SOCK_TOOMANYHEADERS:
                case SOCK_WRITEERROR:
                case SOCK_WRITETIMEOUT:
                default:
                    Ns_Fatal("driver: SockAccept returned: %d", s);
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

        stopping = ((flags & DRIVER_SHUTDOWN) != 0u);

        /*
         * Update the timeout for each closing socket and add to the
         * close list if some data has been read from the socket
         * (i.e., it's not a closing keep-alive connection).
         */
        while (sockPtr != NULL) {
            nextPtr = sockPtr->nextPtr;
            if (sockPtr->keep) {
                Ns_Log(DriverDebug, "setting keepwait %ld for socket %d",
                       sockPtr->drvPtr->keepwait,  sockPtr->sock);

                SockTimeout(sockPtr, &now, sockPtr->drvPtr->keepwait);
                Push(sockPtr, readPtr);
            } else {

                /*
                 * Purely packet oriented drivers set on close the fd to
                 * NS_INVALID_SOCKET. Since we cannot "shutdown" an udp-socket
                 * for writing, we bypass this call.
                 */
                if (sockPtr->sock == NS_INVALID_SOCKET) {
                    SockRelease(sockPtr, SOCK_CLOSE, errno);

                } else if (shutdown(sockPtr->sock, SHUT_WR) != 0) {
                    SockRelease(sockPtr, SOCK_SHUTERROR, errno);

                } else {
                    Ns_Log(DriverDebug, "setting closewait %ld for socket %d",
                           sockPtr->drvPtr->closewait,  sockPtr->sock);
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
            drvPtr->sock = NS_INVALID_SOCKET;
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
    NS_NONNULL_ASSERT(pdata != NULL);
    memset(pdata, 0, sizeof(PollData));
}

static void
PollFree(PollData *pdata)
{
    NS_NONNULL_ASSERT(pdata != NULL);
    ns_free(pdata->pfds);
    memset(pdata, 0, sizeof(PollData));
}

static void
PollReset(PollData *pdata)
{
    NS_NONNULL_ASSERT(pdata != NULL);
    pdata->nfds = 0u;
    pdata->timeout.sec = TIME_T_MAX;
    pdata->timeout.usec = 0;
}

static NS_POLL_NFDS_TYPE
PollSet(PollData *pdata, NS_SOCKET sock, short type, const Ns_Time *timeoutPtr)
{
    NS_NONNULL_ASSERT(pdata != NULL);
    /*
     * Grow the pfds array if necessary.
     */

    if (unlikely(pdata->nfds >= pdata->maxfds)) {
        pdata->maxfds += 100u;
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
PollWait(const PollData *pdata, int waittime)
{
    int n;

    NS_NONNULL_ASSERT(pdata != NULL);

    do {
        n = ns_poll(pdata->pfds, pdata->nfds, waittime);
    } while (n < 0  && errno == NS_EINTR);

    if (n < 0) {
        Ns_Fatal("PollWait: ns_poll() failed: %s", ns_sockstrerror(ns_sockerrno));
    }
    return n;
}

/*
 *----------------------------------------------------------------------
 *
 * RequestNew
 *
 *      Prepares for reading from the socket, allocates a Request struct for
 *      the given socket. It might be reused from the pool or freshly
 *      allocated. Counterpart of RequestFree.
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
RequestNew(Sock *sockPtr)
{
    Request *reqPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    /*
     * Try to get a request from the pool of allocated Requests.
     */
    Ns_MutexLock(&reqLock);
    reqPtr = firstReqPtr;
    if (reqPtr != NULL) {
        Ns_Log(DriverDebug, "RequestNew reuses a Request");
        firstReqPtr = reqPtr->nextPtr;
    }
    Ns_MutexUnlock(&reqLock);

    /*
     * In case we failed, allocate a new Request.
     */
    if (reqPtr == NULL) {
        Ns_Log(DriverDebug, "RequestNew gets a fresh Request");
        reqPtr = ns_calloc(1u, sizeof(Request));
        Tcl_DStringInit(&reqPtr->buffer);
        reqPtr->headers = Ns_SetCreate(NULL);
    }
    sockPtr->reqPtr = reqPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * RequestFree --
 *
 *      Free/clean a socket request structure.  This routine is called
 *      at the end of connection processing or on a socket which
 *      times out during async read-ahead. Counterpart of RequestNew.
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
RequestFree(Sock *sockPtr)
{
    Request *reqPtr;
    bool     keep;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    reqPtr = sockPtr->reqPtr;
    assert(reqPtr != NULL);

    Ns_Log(DriverDebug, "=== RequestFree cleans %p (avail %lu keep %d length %lu contentLength %lu)",
           (void *)reqPtr, reqPtr->avail, sockPtr->keep, reqPtr->length, reqPtr->contentLength);

    keep = (sockPtr->keep) && (reqPtr->avail > reqPtr->contentLength);
    if (keep) {
        size_t      leftover = reqPtr->avail - reqPtr->contentLength;
        const char *offset   = reqPtr->buffer.string + ((size_t)reqPtr->buffer.length - leftover);

        Ns_Log(DriverDebug, "setting leftover to %lu bytes", leftover);
        /*
         * Here it is save to move the data in the buffer, although the
         * reqPtr->content might point to it, since we reinit the content. In
         * case the terminating null character was written to the end of the
         * previous buffer, we have to restore the first character.
         */
        memmove(reqPtr->buffer.string, offset, leftover);
        if (reqPtr->savedChar != '\0') {
            reqPtr->buffer.string[0] = reqPtr->savedChar;
        }
        Tcl_DStringSetLength(&reqPtr->buffer, (int)leftover);
        LogBuffer(DriverDebug, "KEEP BUFFER", reqPtr->buffer.string, leftover); /* TODO: change to DriverDebug */
        reqPtr->leftover = leftover;
    } else {
        /*
         * Clean large buffers in order to avoid memory growth on huge
         * uploads (when maxupload is huge)
         */
        /*fprintf(stderr, "=== reuse buffer size %d avail %d dynamic %d\n",
                reqPtr->buffer.length, reqPtr->buffer.spaceAvl,
                reqPtr->buffer.string == reqPtr->buffer.staticSpace);*/
        if (Tcl_DStringLength(&reqPtr->buffer) > 65536) {
            Tcl_DStringFree(&reqPtr->buffer);
        } else {
            /*
             * Reuse buffer, but set length to 0.
             */
            Tcl_DStringSetLength(&reqPtr->buffer, 0);
        }
        reqPtr->leftover = 0u;
    }

    reqPtr->next           = NULL;
    reqPtr->content        = NULL;
    reqPtr->length         = 0u;
    reqPtr->contentLength  = 0u;

    reqPtr->expectedLength = 0u;
    reqPtr->chunkStartOff  = 0u;
    reqPtr->chunkWriteOff  = 0u;

    reqPtr->roff           = 0u;
    reqPtr->woff           = 0u;
    reqPtr->coff           = 0u;
    reqPtr->avail          = 0u;
    reqPtr->savedChar      = '\0';

    Ns_SetTrunc(reqPtr->headers, 0u);

    if (reqPtr->auth != NULL) {
        Ns_SetFree(reqPtr->auth);
        reqPtr->auth = NULL;
    }

    if (reqPtr->request.line != NULL) {
        Ns_Log(DriverDebug, "RequestFree calls Ns_ResetRequest on %p", (void*)&reqPtr->request);
        Ns_ResetRequest(&reqPtr->request);
    } else {
        Ns_Log(DriverDebug, "RequestFree does not call Ns_ResetRequest on %p", (void*)&reqPtr->request);
    }

    if (!keep) {
        /*
         * Push the reqPtr to the pool for reuse in other connections.
         */
        sockPtr->reqPtr = NULL;

        Ns_MutexLock(&reqLock);
        reqPtr->nextPtr = firstReqPtr;
        firstReqPtr = reqPtr;
        Ns_MutexUnlock(&reqLock);
    } else {
        /*
         * Keep the partly cleaned up reqPtr associated with the connection.
         */
        Ns_Log(DriverDebug, "=== KEEP request structure in sockPtr (don't push into the pool)");
    }
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

static Ns_ReturnCode
SockQueue(Sock *sockPtr, const Ns_Time *timePtr)
{
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    /*
     *  Verify the conditions. Request struct must exist already.
     */
    assert(sockPtr->reqPtr != NULL);

    SockSetServer(sockPtr);
    assert(sockPtr->servPtr != NULL);

    /*
     *  Actual queueing, if not ready spool to the waiting list.
     */
    if (!NsQueueConn(sockPtr, timePtr)) {
        result = NS_TIMEOUT;
    } else {
        result = NS_OK;
    }

    return result;
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
SockPoll(Sock *sockPtr, short type, PollData *pdata)
{
    NS_NONNULL_ASSERT(sockPtr != NULL);
    NS_NONNULL_ASSERT(pdata != NULL);

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
SockTimeout(Sock *sockPtr, const Ns_Time *nowPtr, long timeout)
{
    NS_NONNULL_ASSERT(sockPtr != NULL);
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

static SockState
SockAccept(Driver *drvPtr, Sock **sockPtrPtr, const Ns_Time *nowPtr)
{
    Sock    *sockPtr;
    SockState sockStatus;
    NS_DRIVER_ACCEPT_STATUS status;

    NS_NONNULL_ASSERT(drvPtr != NULL);

    sockPtr = SockNew(drvPtr);

    /*
     * Accept the new connection.
     */

    status = DriverAccept(sockPtr);

    if (unlikely(status == NS_DRIVER_ACCEPT_ERROR)) {
        sockStatus = SOCK_ERROR;

        /*
         * We reach the place frequently, especially on Linux, when we try to
         * accept multiple connection in one sweep. Usually, the errno is
         * EAGAIN.
         */

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

            if ((drvPtr->opts & NS_DRIVER_ASYNC) != 0u) {
                sockStatus = SockRead(sockPtr, 0, nowPtr);
                if ((int)sockStatus < 0) {
                    Ns_Log(DriverDebug, "SockRead returned error %d", sockStatus);

                    SockRelease(sockPtr, sockStatus, errno);
                    sockStatus = SOCK_ERROR;
                    sockPtr = NULL;
                }
            } else {

                /*
                 * Queue this socket without reading, NsGetRequest in the
                 * connection thread will perform actual reading of the
                 * request.
                 */
                sockStatus = SOCK_READY;
            }
        } else if (status == NS_DRIVER_ACCEPT_QUEUE) {

            /*
             *  We need to call RequestNew to make sure socket has
             *  request structure allocated, otherwise NsGetRequest will call
             *  SockRead which is not what this driver wants.
             */
            if (sockPtr->reqPtr == NULL) {
                RequestNew(sockPtr);
            }
            sockStatus = SOCK_READY;
        } else {
            sockStatus = SOCK_MORE;
        }
    }

    *sockPtrPtr = sockPtr;

    return sockStatus;
}


/*
 *----------------------------------------------------------------------
 *
 * SockNew --
 *
 *      Allocate and/or initialize a Sock structure. Counterpart of
 *      SockRelease;
 *
 * Results:
 *      SockPtr
 *
 * Side effects:
 *      Potentially new memory is allocated.
 *
 *----------------------------------------------------------------------
 */

static Sock *
SockNew(Driver *drvPtr)
{
    Sock *sockPtr;

    NS_NONNULL_ASSERT(drvPtr != NULL);

    Ns_MutexLock(&drvPtr->lock);
    sockPtr = drvPtr->sockPtr;
    if (likely(sockPtr != NULL)) {
        drvPtr->sockPtr = sockPtr->nextPtr;
    }
    Ns_MutexUnlock(&drvPtr->lock);

    if (sockPtr == NULL) {
        size_t sockSize = sizeof(Sock) + (nsconf.nextSlsId * sizeof(Ns_Callback *));
        sockPtr = ns_calloc(1u, sockSize);
        sockPtr->drvPtr = drvPtr;
    } else {
        sockPtr->tfd    = 0;
        sockPtr->taddr  = NULL;
        sockPtr->keep   = NS_FALSE;
        sockPtr->flags  = 0u;
        sockPtr->arg    = NULL;
    }
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
SockRelease(Sock *sockPtr, SockState reason, int err)
{
    Driver *drvPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    Ns_Log(DriverDebug, "SockRelease reason %d err %d (sock %d)", reason, err, sockPtr->sock);

    drvPtr = sockPtr->drvPtr;
    assert(drvPtr != NULL);

    SockError(sockPtr, reason, err);
    SockClose(sockPtr, (int)NS_FALSE);
    NsSlsCleanup(sockPtr);

    drvPtr->queuesize--;

    if (sockPtr->reqPtr != NULL) {
        Ns_Log(DriverDebug, "SockRelease calls RequestFree");
        RequestFree(sockPtr);
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
SockError(Sock *sockPtr, SockState reason, int err)
{
    const char *errMsg = NULL;

    switch (reason) {
    case SOCK_READY:
    case SOCK_SPOOL:
    case SOCK_MORE:
    case SOCK_CLOSE:
    case SOCK_CLOSETIMEOUT:
        /* This is normal, never log. */
        break;

    case SOCK_READTIMEOUT:
        /*
         * For this case, whether this is acceptable or not
         * depends upon whether this sock was a keep-alive
         * that we were allowing to 'linger'.
         */
        if (!sockPtr->keep) {
            errMsg = "Timeout during read";
        }
        break;

    case SOCK_WRITETIMEOUT:
        errMsg = "Timeout during write";
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
    case SOCK_ERROR:
        errMsg = "Unknown Error";
        SockSendResponse(sockPtr, 400, errMsg);
        break;
    }

    if (errMsg != NULL) {
        char ipString[NS_IPADDR_SIZE];

        Ns_Log(DriverDebug, "SockError: %s (%d: %s), sock: %d, peer: [%s]:%d, request: %.99s",
               errMsg,
               err, (err != 0) ? strerror(err) : "",
               sockPtr->sock,
               ns_inet_ntop((struct sockaddr *)&(sockPtr->sa), ipString, sizeof(ipString)),
               Ns_SockaddrGetPort((struct sockaddr *)&(sockPtr->sa)),
               (sockPtr->reqPtr != NULL) ? sockPtr->reqPtr->buffer.string : "");
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

static void
SockSendResponse(Sock *sockPtr, int code, const char *errMsg)
{
    struct iovec iov[3];
    char         header[32];
    ssize_t      sent;
    const char  *response = NULL;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    switch (code) {
    case 413:
        response = "Request Entity Too Large";
        break;
    case 414:
        response = "Request-URI Too Long";
        break;
    case 400: /* fall through */
    default:
        response = "Bad Request";
        break;
    }
    snprintf(header, sizeof(header), "HTTP/1.0 %d ", code);
    iov[0].iov_base = header;
    iov[0].iov_len  = strlen(header);
    iov[1].iov_base = (void *)response;
    iov[1].iov_len  = strlen(response);
    iov[2].iov_base = (void *)"\r\n\r\n";
    iov[2].iov_len  = 4u;
    sent = NsDriverSend(sockPtr, iov, 3, 0u);
    if (sent < (ssize_t)(iov[0].iov_len + iov[1].iov_len + iov[2].iov_len)) {
        Ns_Log(Warning, "Driver: partial write while sending error reply");
    }

    if (sockPtr->reqPtr != NULL) {
        Request     *reqPtr = sockPtr->reqPtr;
        Tcl_DString  dsReqLine;
        const char  *requestLine = (reqPtr->request.line != NULL) ? reqPtr->request.line : "";

        (void)ns_inet_ntop((struct sockaddr *)&(sockPtr->sa), sockPtr->reqPtr->peer, NS_IPADDR_SIZE);

        Tcl_DStringInit(&dsReqLine);
        Ns_Log(Warning, "invalid request: %d (%s) from peer %s request '%s' offsets: read %lu write %lu content %lu, avail %lu",
               code, errMsg,
               reqPtr->peer,
               Ns_DStringAppendPrintable(&dsReqLine, requestLine, strlen(requestLine)),
               reqPtr->roff,
               reqPtr->woff,
               reqPtr->coff,
               reqPtr->avail);
        Tcl_DStringFree(&dsReqLine);

        LogBuffer(Warning, "REQ BUFFER", reqPtr->buffer.string, (size_t)reqPtr->buffer.length);

    } else {
        Ns_Log(Warning, "invalid request: %d (%s) - no request information available",
               code, errMsg);
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
SockTrigger(NS_SOCKET sock)
{
    if (send(sock, "", 1, 0) != 1) {
        const char *errstr = ns_sockstrerror(ns_sockerrno);

        Ns_Log(Error, "driver: trigger send() failed: %s", errstr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * SockClose --
 *
 *      Closes connection socket, does all cleanups. The input parameter
 *      "keep" might be NS_TRUE/NS_FALSE or -1 if undecided.
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
    NS_NONNULL_ASSERT(sockPtr != NULL);

    if (keep != 0) {
        keep = (int)DriverKeep(sockPtr);
    }
    if (keep == (int)NS_FALSE) {
        DriverClose(sockPtr);
    }
    sockPtr->keep = (bool)keep;

    /*
     * Unconditionally remove temporary file, connection thread
     * should take care about very large uploads.
     */

    if (sockPtr->tfile != NULL) {
        unlink(sockPtr->tfile);
        ns_free(sockPtr->tfile);
        sockPtr->tfile = NULL;

        if (sockPtr->tfd > 0) {
            /*
             * Close and reset fd. The fd should be > 0 unless we are in error
             * conditions.
             */
            (void) ns_close(sockPtr->tfd);
        }
        sockPtr->tfd = 0;

    } else if (sockPtr->tfd > 0) {
        /*
         * This must be a fd allocated via Ns_GetTemp();
         */
        Ns_ReleaseTemp(sockPtr->tfd);
        sockPtr->tfd = 0;
    }

#ifndef _WIN32
    /*
     * Unmmap temp file used for spooled content.
     */
    if (sockPtr->taddr != NULL) {
        munmap(sockPtr->taddr, (size_t)sockPtr->tsize);
    }
    sockPtr->taddr = NULL;
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
 *      NS_TRUE when chunk was complete, NS_FALSE otherwise
 *
 * Side effects:
 *      updates the buffer if update is true (and adjusts reqPtr->chunkWriteOff)
 *      updates always reqPtr->chunkStartOff to allow incremental operations
 *
 *----------------------------------------------------------------------
 */
static bool
ChunkedDecode(Request *reqPtr, bool update)
{
    const Tcl_DString *bufPtr;
    const char        *end, *chunkStart;
    bool              success = NS_TRUE;

    NS_NONNULL_ASSERT(reqPtr != NULL);

    bufPtr = &reqPtr->buffer;
    end = bufPtr->string + bufPtr->length;
    chunkStart = bufPtr->string + reqPtr->chunkStartOff;

    while (reqPtr->chunkStartOff <  (size_t)bufPtr->length) {
        char   *p = strstr(chunkStart, "\r\n");
        size_t  chunk_length;

        if (p == NULL) {
            Ns_Log(DriverDebug, "ChunkedDecode: chunk did not find end-of-line");
            success = NS_FALSE;
            break;
        }

        *p = '\0';
        chunk_length = (size_t)strtol(chunkStart, NULL, 16);
        *p = '\r';

        if (p + 2 + chunk_length > end) {
            Ns_Log(DriverDebug, "ChunkedDecode: chunk length past end of buffer");
            success = NS_FALSE;
            break;
        }
        if (update) {
            char *writeBuffer = bufPtr->string + reqPtr->chunkWriteOff;

            memmove(writeBuffer, p + 2, chunk_length);
            reqPtr->chunkWriteOff += chunk_length;
            *(writeBuffer + chunk_length) = '\0';
        }
        reqPtr->chunkStartOff += (size_t)(p - chunkStart) + 4u + chunk_length;
        chunkStart = bufPtr->string + reqPtr->chunkStartOff;
    }

    return success;
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
 *      SOCK_CLOSE: peer closed connection
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

static SockState
SockRead(Sock *sockPtr, int spooler, const Ns_Time *timePtr)
{
    const Driver       *drvPtr;
    Request      *reqPtr;
    Tcl_DString  *bufPtr;
    struct iovec  buf;
    char          tbuf[16384];
    size_t        buflen, nread;
    ssize_t       n;
    SockState     resultState;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    drvPtr = sockPtr->drvPtr;

    tbuf[0] = '\0';

    /*
     * In case of keepwait, the accept time is not meaningful and
     * reset to 0. In such cases, update acceptTime to the actual
     * begin of a request. This part is intended for async drivers.
     */
    if (sockPtr->acceptTime.sec == 0) {
        assert(timePtr != NULL);
        sockPtr->acceptTime = *timePtr;
    }

    /*
     * Initialize Request structure if needed
     */
    if (sockPtr->reqPtr == NULL) {
        RequestNew(sockPtr);
    }

    /*
     * On the first read, attempt to read-ahead bufsize bytes.
     * Otherwise, read only the number of bytes left in the
     * content.
     */

    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;
    if (reqPtr->length == 0u) {
        nread = drvPtr->bufsize;
    } else {
        nread = reqPtr->length - reqPtr->avail;
    }

    /*
     * Grow the buffer to include space for the next bytes.
     */

    buflen = (size_t)bufPtr->length;
    n = (ssize_t)(buflen + nread);
    if (unlikely(n > drvPtr->maxinput)) {
        n = (ssize_t)drvPtr->maxinput;
        nread = (size_t)n - buflen;
        if (nread == 0u) {
            Ns_Log(DriverDebug, "SockRead: maxinput reached %" TCL_LL_MODIFIER "d",
                   drvPtr->maxinput);
            return SOCK_ERROR;
        }
    }

    /*
     * Use temp file for content larger than readahead bytes.
     */
#ifndef _WIN32
    if (reqPtr->coff > 0u                     /* We are in the content part (after the header) */
        && !reqPtr->chunkStartOff             /* Never spool chunked encoded data since we decode in memory */
        && reqPtr->length > (size_t)drvPtr->readahead /* We need more data */
        && sockPtr->tfd <= 0                  /* We have no spool fd */
        ) {
        const DrvSpooler *spPtr = &drvPtr->spooler;

        Ns_Log(DriverDebug, "SockRead: require tmp file for content spooling (length %lu > readahead %lu)",
               reqPtr->length, drvPtr->readahead);

        /*
         * In driver mode send this Sock to the spooler thread if
         * it is running
         */

        if (spooler == 0 && spPtr->threads > 0) {
            return SOCK_SPOOL;
        }

        /*
         * If maxupload is specified and content size exceeds the configured
         * values, spool uploads into normal temp file (not deleted).  We do
         * not want to map such large files into memory.
         */
        if (drvPtr->maxupload > 0
            && reqPtr->length > (size_t)drvPtr->maxupload
            ) {
            sockPtr->tfile = ns_malloc(strlen(drvPtr->uploadpath) + 16u);
            sprintf(sockPtr->tfile, "%s/%d.XXXXXX", drvPtr->uploadpath, sockPtr->sock);
            sockPtr->tfd = ns_mkstemp(sockPtr->tfile);
            if (sockPtr->tfd == NS_INVALID_FD) {
                Ns_Log(Error, "SockRead: cannot create spool file with template '%s': %s",
                       sockPtr->tfile, strerror(errno));
            }
        } else {
            /*
             * Get a temporary fd. These FDs are used for mmapping.
             */
            sockPtr->tfd = Ns_GetTemp();
        }

        if (unlikely(sockPtr->tfd == NS_INVALID_FD)) {
            Ns_Log(DriverDebug, "SockRead: spool fd invalid");
            return SOCK_ERROR;
        }

        n = (ssize_t)((size_t)bufPtr->length - reqPtr->coff);
        assert(n >= 0);
        if (ns_write(sockPtr->tfd, bufPtr->string + reqPtr->coff, (size_t)n) != n) {
            return SOCK_WRITEERROR;
        }
        Tcl_DStringSetLength(bufPtr, 0);
    }
#endif
    if (sockPtr->tfd > 0) {
        buf.iov_base = tbuf;
        buf.iov_len = MIN(nread, sizeof(tbuf));
    } else {
        Tcl_DStringSetLength(bufPtr, (int)(buflen + nread));
        buf.iov_base = bufPtr->string + reqPtr->woff;
        buf.iov_len = nread;
    }

    if (reqPtr->leftover > 0u) {
        n = (ssize_t)reqPtr->leftover;
        reqPtr->leftover = 0u;
        buflen = 0u;
        Ns_Log(DriverDebug, "SockRead receive from leftover %ld bytes", n);
    } else {
        n = DriverRecv(sockPtr, &buf, 1);
        Ns_Log(DriverDebug, "SockRead receive from network %ld bytes", n);
    }

    if (n < 0) {
        Tcl_DStringSetLength(bufPtr, (int)buflen);
        /*
         * The driver returns -1 when the peer closed the connection, but
         * clears the errno such we can distinguish form error conditons.
         */
        if (errno == 0) {
            return SOCK_CLOSE;
        }
        return SOCK_READERROR;
    }

    if (n == 0) {
        Tcl_DStringSetLength(bufPtr, (int)buflen);
        return SOCK_MORE;
    }

    if (sockPtr->tfd > 0) {
        if (ns_write(sockPtr->tfd, tbuf, (size_t)n) != n) {
            return SOCK_WRITEERROR;
        }
    } else {
        Tcl_DStringSetLength(bufPtr, (int)(buflen + (size_t)n));
    }

    reqPtr->woff  += (size_t)n;
    reqPtr->avail += (size_t)n;

    /*
     * This driver needs raw buffer, it is binary or non-HTTP request
     */

    if ((drvPtr->opts & NS_DRIVER_NOPARSE) != 0u) {
        return SOCK_READY;
    }

    resultState = SockParse(sockPtr);

    return resultState;
}


/*----------------------------------------------------------------------
 *
 * LogBuffer --
 *
 *      Debug function to output buffer content when the provided severity is
 *      enabled. The function prints just visible characters and space as is
 *      and prints the hex code otherwise.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Writes to error.log
 *
 *----------------------------------------------------------------------
 */
static void
LogBuffer(Ns_LogSeverity severity, const char *msg, const char *buffer, size_t len)
{
    Tcl_DString ds;

    NS_NONNULL_ASSERT(msg != NULL);
    NS_NONNULL_ASSERT(buffer != NULL);

    if (Ns_LogSeverityEnabled(severity)) {

        Tcl_DStringInit(&ds);
        Tcl_DStringAppend(&ds, msg, -1);
        Tcl_DStringAppend(&ds, ": ", 2);
        (void)Ns_DStringAppendPrintable(&ds, buffer, len);

        Ns_Log(severity, "%s", ds.string);
        Tcl_DStringFree(&ds);
    }
}


/*----------------------------------------------------------------------
 *
 * EndOfHeader --
 *
 *      Function to be called (once), when end of header is reached. At this
 *      time, all request header lines were parsed already correctly.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Update various reqPtr fields and signal error conditions via
 *      sockPtr->flags.
 *
 *----------------------------------------------------------------------
 */
static size_t
EndOfHeader(Sock *sockPtr)
{
    bool        gzip;
    Request    *reqPtr;
    const char *s;

    NS_NONNULL_ASSERT(sockPtr != NULL);

    reqPtr = sockPtr->reqPtr;
    assert(reqPtr != NULL);

    reqPtr->chunkStartOff = 0u;

    /*
     * Handle content-length, which might be provided or not,
     */
    s = Ns_SetIGet(reqPtr->headers, "content-length");
    if (s == NULL) {
        s = Ns_SetIGet(reqPtr->headers, "Transfer-Encoding");

        if (s != NULL) {
            /* Lower case is in the standard, capitalized by Mac OS X */
            if (STREQ(s, "chunked") || STREQ(s, "Chunked")) {
                Tcl_WideInt expected;

                reqPtr->chunkStartOff = reqPtr->roff;
                reqPtr->chunkWriteOff = reqPtr->chunkStartOff;
                reqPtr->contentLength = 0u;

                /* We need expectedLength for safely terminating read loop */

                s = Ns_SetIGet(reqPtr->headers, "X-Expected-Entity-Length");

                if ((s != NULL)
                    && (Ns_StrToWideInt(s, &expected) == NS_OK)
                    && (expected > 0) ) {
                    reqPtr->expectedLength = (size_t)expected;
                }
                s = NULL;
            }
        }
    }

    if (s != NULL) {
        Tcl_WideInt length;

        /*
         * Content-length was provided. We honor just meaningful
         * content-length hints only.
         */

        if ((Ns_StrToWideInt(s, &length) == NS_OK) && (length > 0)) {
            reqPtr->length = (size_t)length;
            /*
             * Handle too large input requests
             */
            if (reqPtr->length > (size_t)sockPtr->drvPtr->maxinput) {
                Ns_Log(DriverDebug, "SockParse: request too large, length=%"
                       PRIdz ", maxinput=%" TCL_LL_MODIFIER "d",
                       reqPtr->length, sockPtr->drvPtr->maxinput);
                /*
                 * We have to read the full request (although it is
                 * too large) to drain the channel. Otherwise, the
                 * server might close the connection *before* it has
                 * received full request with its body. Such a
                 * premature close leads to an error message in
                 * clients like firefox. Therefore we do not return
                 * SOCK_ENTITYTOOLARGE here, but just flag the
                 * condition. ...
                 *
                 * Possible future improvements: Currently, the
                 * content is really received and kept. We might
                 * simply drain the input and ignore the read content,
                 * but this requires handling for the various input
                 * modes (spooling, chunked content, etc.). We should
                 * make the same for the other reply-codes in
                 * SockError().
                 */
                sockPtr->flags = NS_CONN_ENTITYTOOLARGE;
                sockPtr->keep = NS_FALSE;

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
        gzip = NsParseAcceptEncoding(reqPtr->request.version, s);
    } else {
        /* no accept-encoding header; don't allow gzip */
        gzip = NS_FALSE;
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

    /*
     * Set up request length for spooling and further read operations
     */
    if (reqPtr->contentLength != 0u) {
        /*
         * Content-Length was provided, use it
         */
        reqPtr->length = reqPtr->contentLength;
    }

    return reqPtr->roff;
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

static SockState
SockParse(Sock *sockPtr)
{
    const Tcl_DString  *bufPtr;
    const Driver       *drvPtr;
    Request            *reqPtr;
    char                save;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    drvPtr = sockPtr->drvPtr;

    NsUpdateProgress((Ns_Sock *) sockPtr);

    reqPtr = sockPtr->reqPtr;
    bufPtr = &reqPtr->buffer;

    /*
     * Scan lines (header) until start of content (body-part)
     */

    while (reqPtr->coff == 0u) {
        char *s, *e;
        size_t cnt;

        /*
         * Find the next header line.
         */
        s = bufPtr->string + reqPtr->roff;
        e = memchr(s, INTCHAR('\n'), reqPtr->avail);

        if (unlikely(e == NULL)) {
            /*
             * Input not yet newline terminated - request more data.
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
            sockPtr->keep = NS_FALSE;
            if (reqPtr->request.line == NULL) {
                Ns_Log(DriverDebug, "SockParse: maxline reached of %d bytes",
                       drvPtr->maxline);
                sockPtr->flags = NS_CONN_REQUESTURITOOLONG;
                Ns_Log(Warning, "request line is too long (%lu bytes)", (e - s));
            } else {
                sockPtr->flags = NS_CONN_LINETOOLONG;
                Ns_Log(Warning, "request header line is too long (%lu bytes)", (e - s));
            }
        }

        /*
         * Update next read pointer to end of this line.
         */
        cnt = (size_t)(e - s) + 1u;
        reqPtr->roff  += cnt;
        reqPtr->avail -= cnt;

        /*
         * Adjust end pointer to the last content character before the line
         * terminator.
         */
        if (likely(e > s) && likely(*(e-1) == '\r')) {
            --e;
        }

        /*
         * Check for end of headers.
         */
        if (unlikely(e == s)) {
            /*
             * We are at end of headers
             */
            reqPtr->coff = EndOfHeader(sockPtr);

        } else {
            /*
             * We have the request-line or a header line to process.
             */
            save = *e;
            *e = '\0';

            if (unlikely(reqPtr->request.line == NULL)) {
                /*
                 * There is no request-line set. The received line must the
                 * the request-line.
                 */
                Ns_Log(DriverDebug, "SockParse (%d): parse request line <%s>", sockPtr->sock, s);

                if (Ns_ParseRequest(&reqPtr->request, s) == NS_ERROR) {
                    /*
                     * Invalid request.
                     */
                    return SOCK_BADREQUEST;
                }

                /*
                 * HTTP 0.9 did not have a HTTP-version number or request headers
                 * and no empty line terminating the request header.
                 */
                if (unlikely(reqPtr->request.version < 1.0)) {
                    /*
                     * Pre-HTTP/1.0 request.
                     */
                    reqPtr->coff = reqPtr->roff;
                    Ns_Log(Notice, "pre-HTTP/1.0 request <%s>", reqPtr->request.line);
                }

            } else if (Ns_ParseHeader(reqPtr->headers, s, Preserve) != NS_OK) {
                /*
                 * Invalid header.
                 */
                return SOCK_BADHEADER;

            } else {
                /*
                 * Check for max number of headers
                 */
                if (unlikely(Ns_SetSize(reqPtr->headers) > (size_t)drvPtr->maxheaders)) {
                    Ns_Log(DriverDebug, "SockParse (%d): maxheaders reached of %d bytes",
                           sockPtr->sock, drvPtr->maxheaders);
                    return SOCK_TOOMANYHEADERS;
                }
            }

            *e = save;
        }
    }

    if (unlikely(reqPtr->request.line == NULL)) {
        /*
         * We are at end of headers, but we have not parsed a request line
         * (maybe just two line feeds).
         */
        return SOCK_BADREQUEST;
    }

    /*
     * We are in the request body
     */
    assert(reqPtr->coff > 0u);
    assert(reqPtr->request.line != NULL);

    /*
     * Check if all content has arrived.
     */
    Ns_Log(Dev, "=== length < avail (length %lu, avail %lu) tfd %d tfile %p chunkStartOff %lu",
           reqPtr->length, reqPtr->avail,sockPtr->tfd, (void *)sockPtr->tfile, reqPtr->chunkStartOff);

    if (reqPtr->chunkStartOff != 0u) {
        /*
         * Chunked encoding was provided
         */
        bool   complete;
        size_t currentContentLength;

        complete = ChunkedDecode(reqPtr, NS_TRUE);
        currentContentLength = reqPtr->chunkWriteOff - reqPtr->coff;

        /*
         * A chunk might be complete, but it might not be the last
         * chunk from the client. The best thing would be to be able
         * to read until eof here. In cases, where the (optional)
         * expectedLength was provided by the client, we terminate
         * depending on that information
         */
        if ((!complete)
            || (reqPtr->expectedLength != 0u && currentContentLength < reqPtr->expectedLength)) {
            /*
             * ChunkedDecode wants more data
             */
            return SOCK_MORE;
        }
        /*
         * ChunkedDecode has enough data
         */
        reqPtr->length = (size_t)currentContentLength;
    }

    if (reqPtr->avail < reqPtr->length) {
        Ns_Log(DriverDebug, "SockRead wait for more input");
        /*
         * Wait for more input.
         */
        return SOCK_MORE;
    }

    Ns_Log(Dev, "=== all required data is available (avail %lu, length %lu, "
           "readahead %lu maxupload %lu) tfd %d",
           reqPtr->avail, reqPtr->length, drvPtr->readahead, drvPtr->maxupload,
           sockPtr->tfd);

    /*
     * We have all required data in the receive buffer or in a temporary file.
     *
     * - Uploads > drvPtr->readahead: these are put into temporary files.
     *
     * - Uploads > drvPtr->maxupload: these are put into temporary files
     *   without mmapping, no content parsing will be performed in memory.
     */
    if (sockPtr->tfile != NULL) {
        reqPtr->content = NULL;
        reqPtr->next = NULL;
        reqPtr->avail = 0u;
        Ns_Log(DriverDebug, "content spooled to file: size %" PRIdz ", file %s",
               reqPtr->length, sockPtr->tfile);

        return SOCK_READY;
    }

    /*
     * Uploads < maxupload are spooled to files and mmapped in order to
     * provide the usual interface via [ns_conn content].
     */
    if (sockPtr->tfd > 0) {
#ifdef _WIN32
        /*
         * For _WIN32, tfd should never be set, since tfd-spooling is not
         * implemented for windows.
         */
        assert(0);
#else
        int prot = PROT_READ | PROT_WRITE;
        /*
         * Add a byte to make sure, the string termination with \0 below falls
         * always into the mmapped area. On some older OSes this might lead to
         * crashes when we hitting page boundaries.
         */
        ssize_t result = ns_write(sockPtr->tfd, "\0", 1);
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
        Ns_Log(Debug, "content spooled to mmapped file: readahead=%"
               TCL_LL_MODIFIER "d, filesize=%" PRIdz,
               drvPtr->readahead, sockPtr->tsize);
#endif
    } else {
        /*
         * Set the content the begin of the remaining buffer (content offset).
         * This happens as well when reqPtr->contentLength is 0, but it is
         * needed for chunked input processing.
         */
        reqPtr->content = bufPtr->string + reqPtr->coff;
    }
    reqPtr->next = reqPtr->content;

    /*
     * Add a terminating null character. The content might be from the receive
     * buffer (DString) or from the mmaped file. Non-mmapped files are handled
     * above.
     */
    if (reqPtr->length > 0u) {
        assert(sockPtr->tfile == NULL);
        Ns_Log(DriverDebug, "SockRead adds null terminating character at content[%lu]", reqPtr->length);

        reqPtr->savedChar = reqPtr->content[reqPtr->length];
        reqPtr->content[reqPtr->length] = '\0';
        if (sockPtr->taddr == NULL) {
            LogBuffer(DriverDebug, "UPDATED BUFFER", sockPtr->reqPtr->buffer.string, (size_t)reqPtr->buffer.length);
        }
    }

    return SOCK_READY;
}

/*
 *----------------------------------------------------------------------
 *
 * SockSetServer --
 *
 *      Set virtual server from driver context or Host header.
 *
 * Results:
 *      void.
 *
 * Side effects:
 *
 *      Updates sockPtr->servPtr. In case an invalid server set, or the
 *      required host field in HTTP/1.1 is missing the HTTP-method is set to
 *      the constant "BAD".
 *
 *----------------------------------------------------------------------
 */

static void
SockSetServer(Sock *sockPtr)
{
    const ServerMap *mapPtr = NULL;
    const char      *host;
    int              status = 1;
    Request         *reqPtr;

    NS_NONNULL_ASSERT(sockPtr != NULL);


    reqPtr = sockPtr->reqPtr;
    assert(reqPtr != NULL);

    sockPtr->servPtr  = sockPtr->drvPtr->servPtr;
    sockPtr->location = sockPtr->drvPtr->location;

    host = Ns_SetIGet(reqPtr->headers, "Host");
    Ns_Log(DriverDebug, "SockSetServer host %s request line %s", host, reqPtr->request.line);

    if (unlikely((host == NULL) && (reqPtr->request.version >= 1.1))) {
        /*
         * HTTP/1.1 requires host header
         */
        Ns_Log(Notice, "request header field \"Host\" is missing in HTTP/1.1 request: \"%s\"\n",
               reqPtr->request.line);
        status = 0;
    }
    if (sockPtr->servPtr == NULL) {
        if (host != NULL) {
            const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&hosts, host);

            if (hPtr != NULL) {
                mapPtr = Tcl_GetHashValue(hPtr);
            }
        }
        if (mapPtr == NULL) {
            mapPtr = defMapPtr;
        }
        if (mapPtr != NULL) {
            sockPtr->servPtr  = mapPtr->servPtr;
            sockPtr->location = mapPtr->location;
        }
        if (sockPtr->servPtr == NULL) {
            Ns_Log(Warning, "cannot determine server for request: \"%s\" (host \"%s\")\n",
                   reqPtr->request.line, host);
            status = 0;
        }
    }

    if (unlikely(status == 0)) {
        Ns_Log(DriverDebug, "SockSetServer sets method to BAD");
        ns_free((char *)reqPtr->request.method);
        reqPtr->request.method = ns_strdup("BAD");
    }

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
    char           charBuffer[1];
    int            pollto;
    bool           stopping;
    Sock          *sockPtr, *nextPtr, *waitPtr, *readPtr;
    Ns_Time        now, diff;
    const Driver  *drvPtr;
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
    stopping = NS_FALSE;

    while (!stopping) {

        /*
         * If there are any read sockets, set the bits
         * and determine the minimum relative timeout.
         */

        PollReset(&pdata);
        (void)PollSet(&pdata, queuePtr->pipe[0], (short)POLLIN, NULL);

        if (readPtr == NULL) {
            pollto = 30 * 1000;
        } else {
            sockPtr = readPtr;
            while (sockPtr != NULL) {
                SockPoll(sockPtr, (short)POLLIN, &pdata);
                sockPtr = sockPtr->nextPtr;
            }
            pollto = -1;
        }

        /*
         * Select and drain the trigger pipe if necessary.
         */

        /*n =*/ (void) PollWait(&pdata, pollto);

        if (PollIn(&pdata, 0) && unlikely(ns_recv(queuePtr->pipe[0], charBuffer, 1u, 0) != 1)) {
            Ns_Fatal("spooler: trigger ns_recv() failed: %s",
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

            } else if (!PollIn(&pdata, sockPtr->pidx)) {
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
                SockState n = SockRead(sockPtr, 1, &now);
                switch (n) {
                case SOCK_MORE:
                    SockTimeout(sockPtr, &now, drvPtr->recvwait);
                    Push(sockPtr, readPtr);
                    break;

                case SOCK_READY:
                    assert(sockPtr->reqPtr != NULL);
                    SockSetServer(sockPtr);
                    Push(sockPtr, waitPtr);
                    break;

                case SOCK_BADHEADER:
                case SOCK_BADREQUEST:
                case SOCK_CLOSE:
                case SOCK_CLOSETIMEOUT:
                case SOCK_ENTITYTOOLARGE:
                case SOCK_ERROR:
                case SOCK_READERROR:
                case SOCK_READTIMEOUT:
                case SOCK_SHUTERROR:
                case SOCK_SPOOL:
                case SOCK_TOOMANYHEADERS:
                case SOCK_WRITEERROR:
                case SOCK_WRITETIMEOUT:
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
                if (!NsQueueConn(sockPtr, &now)) {
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
    queuePtr->stopped = NS_TRUE;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);
}

static void
SpoolerQueueStart(SpoolerQueue *queuePtr, Ns_ThreadProc *proc)
{
    NS_NONNULL_ASSERT(proc != NULL);

    while (queuePtr != NULL) {
        if (ns_sockpair(queuePtr->pipe) != 0) {
            Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
        }
        Ns_ThreadCreate(proc, queuePtr, 0, &queuePtr->thread);
        queuePtr = queuePtr->nextPtr;
    }
}

static void
SpoolerQueueStop(SpoolerQueue *queuePtr, const Ns_Time *timeoutPtr, const char *name)
{

    NS_NONNULL_ASSERT(timeoutPtr != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    while (queuePtr != NULL) {
        Ns_ReturnCode status;

        Ns_MutexLock(&queuePtr->lock);
        if (!queuePtr->stopped && !queuePtr->shutdown) {
            Ns_Log(Debug, "%s%d: triggering shutdown", name, queuePtr->id);
            queuePtr->shutdown = NS_TRUE;
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
            if (queuePtr->thread != NULL) {
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
    bool          trigger = NS_FALSE;
    SpoolerQueue *queuePtr;

    NS_NONNULL_ASSERT(drvPtr != NULL);
    NS_NONNULL_ASSERT(sockPtr != NULL);
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
        trigger = NS_TRUE;
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
 *      WriterSockRelease() are responsible for obtaining and
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
WriterSockRequire(const Conn *connPtr) {
    WriterSock *wrSockPtr;

    NS_NONNULL_ASSERT(connPtr != NULL);

    NsWriterLock();
    wrSockPtr = connPtr->strWriter;
    if (wrSockPtr != NULL) {
        wrSockPtr->refCount ++;
    }
    NsWriterUnlock();
    return wrSockPtr;
}

static void
WriterSockRelease(WriterSock *wrSockPtr) {
    SpoolerQueue *queuePtr;

    NS_NONNULL_ASSERT(wrSockPtr != NULL);

    wrSockPtr->refCount --;

    Ns_Log(DriverDebug, "WriterSockRelease %p refCount %d keep %d",
           (void *)wrSockPtr, wrSockPtr->refCount, wrSockPtr->keep);

    if (wrSockPtr->refCount > 0) {
        return;
    }

    Ns_Log(DriverDebug,
           "Writer: closed sock %d, file fd %d, error %d/%d, "
           "sent=%" TCL_LL_MODIFIER "d, flags=%X",
           wrSockPtr->sockPtr->sock, wrSockPtr->fd,
           wrSockPtr->status, wrSockPtr->err,
           wrSockPtr->nsent, wrSockPtr->flags);

    if (wrSockPtr->doStream != NS_WRITER_STREAM_NONE) {
        Conn *connPtr;

        NsWriterLock();
        connPtr = wrSockPtr->connPtr;
        if (connPtr != NULL && connPtr->strWriter != NULL) {
            connPtr->strWriter = NULL;
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
        for (curPtr = (lastPtr != NULL) ? lastPtr->nextPtr : NULL;
             curPtr != NULL;
             lastPtr = curPtr, curPtr = curPtr->nextPtr
             ) {
            if (curPtr == wrSockPtr) {
                lastPtr->nextPtr = wrSockPtr->nextPtr;
                queuePtr->queuesize--;
                break;
            }
        }
    }

    if ((wrSockPtr->err != 0) || (wrSockPtr->status != SPOOLER_OK)) {
        int i;
        /*
         * Lookup the matching sockState from the spooler state. The array has
         * just 5 elements, on average, just 2 comparisons are needed (since OK ist at the end)
         */
        for (i = 0; i < Ns_NrElements(spoolerStateMap); i++) {
            if (spoolerStateMap[i].spoolerState == wrSockPtr->status) {
                SockError(wrSockPtr->sockPtr, spoolerStateMap[i].sockState, wrSockPtr->err);
                break;
            }
        }
        NsSockClose(wrSockPtr->sockPtr, (int)NS_FALSE);
    } else {
        NsSockClose(wrSockPtr->sockPtr, (int)wrSockPtr->keep);
    }
    if (wrSockPtr->clientData != NULL) {
        ns_free(wrSockPtr->clientData);
    }
    if (wrSockPtr->fd != NS_INVALID_FD) {
        if (wrSockPtr->doStream != NS_WRITER_STREAM_FINISH) {
            (void) ns_close(wrSockPtr->fd);
        }
        ns_free(wrSockPtr->c.file.buf);
    } else if (wrSockPtr->c.mem.bufs != NULL) {
        if (wrSockPtr->c.mem.fmap.addr != NULL) {
            NsMemUmap(&wrSockPtr->c.mem.fmap);

        } else {
            int i;
            for (i = 0; i < wrSockPtr->c.mem.nbufs; i++) {
                ns_free((char *)wrSockPtr->c.mem.bufs[i].iov_base);
            }
        }
        if (wrSockPtr->c.mem.bufs != wrSockPtr->c.mem.preallocated_bufs) {
            ns_free(wrSockPtr->c.mem.bufs);
        }
    }
    if (wrSockPtr->headerString != NULL) {
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
 *      Fills up curPtr->c.file.buf and updates counters/sizes.
 *
 *----------------------------------------------------------------------
 */

static SpoolerState
WriterReadFromSpool(WriterSock *curPtr) {
    NsWriterStreamState doStream;
    SpoolerState        status = SPOOLER_OK;
    size_t              maxsize, toRead;
    unsigned char      *bufPtr;

    NS_NONNULL_ASSERT(curPtr != NULL);

    doStream = curPtr->doStream;
    if (doStream != NS_WRITER_STREAM_NONE) {
        Ns_MutexLock(&curPtr->c.file.fdlock);
        toRead = curPtr->c.file.toRead;
        Ns_MutexUnlock(&curPtr->c.file.fdlock);
    } else {
        toRead = curPtr->c.file.toRead;
    }

    maxsize = curPtr->c.file.maxsize;
    bufPtr  = curPtr->c.file.buf;

    /*
     * When bufsize > 0 we have leftover from previous send. In such
     * cases, move the leftover to the front, and fill the reminder of
     * the buffer with new c.
     */

    if (curPtr->c.file.bufsize > 0u) {
        Ns_Log(DriverDebug,
               "### Writer %p %.6x leftover %" PRIdz " offset %ld",
               (void *)curPtr,
               curPtr->flags,
               curPtr->c.file.bufsize,
               (long)curPtr->c.file.bufoffset);
        if (likely(curPtr->c.file.bufoffset > 0)) {
            memmove(curPtr->c.file.buf,
                    curPtr->c.file.buf + curPtr->c.file.bufoffset,
                    curPtr->c.file.bufsize);
        }
        bufPtr = curPtr->c.file.buf + curPtr->c.file.bufsize;
        maxsize -= curPtr->c.file.bufsize;
    }
    if (toRead > maxsize) {
        toRead = maxsize;
    }

    /*
     * Read content from the file into the buffer.
     */
    if (toRead > 0u) {
        ssize_t n;

        if (doStream != NS_WRITER_STREAM_NONE) {
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
            Ns_MutexLock(&curPtr->c.file.fdlock);
            (void) ns_lseek(curPtr->fd, (off_t)curPtr->nsent, SEEK_SET);
        }

        n = ns_read(curPtr->fd, bufPtr, toRead);

        if (n <= 0) {
            status = SPOOLER_READERROR;
        } else {
            /*
             * curPtr->c.file.toRead is still protected by curPtr->c.file.fdlock when
             * needed.
             */
            curPtr->c.file.toRead -= (size_t)n;
            curPtr->c.file.bufsize += (size_t)n;
        }

        if (doStream != NS_WRITER_STREAM_NONE) {
            Ns_MutexUnlock(&curPtr->c.file.fdlock);
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

static SpoolerState
WriterSend(WriterSock *curPtr, int *err) {
    const struct iovec *bufs;
    struct iovec        vbuf;
    int                 nbufs;
    SpoolerState        status = SPOOLER_OK;
    size_t              toWrite;
    ssize_t             n;

    NS_NONNULL_ASSERT(curPtr != NULL);
    NS_NONNULL_ASSERT(err != NULL);

    /*
     * Prepare send operation
     */
    if (curPtr->fd != NS_INVALID_FD) {
        /*
         * Send a single buffer with curPtr->c.file.bufsize bytes from the
         * curPtr->c.file.buf to the client.
         */
        vbuf.iov_len = curPtr->c.file.bufsize;
        vbuf.iov_base = (void *)curPtr->c.file.buf;
        bufs = &vbuf;
        nbufs = 1;
        toWrite = curPtr->c.file.bufsize;
    } else {
        int i;

        /*
         * Send multiple buffers.
         * Get length of remaining buffers
         */
        toWrite = 0u;
        for (i = 0; i < curPtr->c.mem.nsbufs; i ++) {
            toWrite += curPtr->c.mem.sbufs[i].iov_len;
        }
        Ns_Log(DriverDebug,
               "### Writer wants to send remainder nbufs %d len %" PRIdz,
               curPtr->c.mem.nsbufs, toWrite);

        /*
         * Add buffers from the source and fill structure up to max
         */
        while (curPtr->c.mem.bufIdx  < curPtr->c.mem.nbufs &&
               curPtr->c.mem.sbufIdx < UIO_SMALLIOV) {
            const struct iovec *vPtr = &curPtr->c.mem.bufs[curPtr->c.mem.bufIdx];

            if (vPtr->iov_len > 0u && vPtr->iov_base != NULL) {

                Ns_Log(DriverDebug,
                       "### Writer copies source %d to scratch %d len %" PRIiovlen,
                       curPtr->c.mem.bufIdx, curPtr->c.mem.sbufIdx, vPtr->iov_len);

                toWrite += Ns_SetVec(curPtr->c.mem.sbufs, curPtr->c.mem.sbufIdx++,
                                     vPtr->iov_base, vPtr->iov_len);
                curPtr->c.mem.nsbufs++;
            }
            curPtr->c.mem.bufIdx++;
        }

        bufs  = curPtr->c.mem.sbufs;
        nbufs = curPtr->c.mem.nsbufs;
        Ns_Log(DriverDebug, "### Writer wants to send %d bufs size %" PRIdz,
               nbufs, toWrite);
    }

    n = NsDriverSend(curPtr->sockPtr, bufs, nbufs, 0u);

    if (n < 0) {
        *err = errno;
        status = SPOOLER_WRITEERROR;
    } else {
        /*
         * We have sent something.
         */
        if (curPtr->doStream != NS_WRITER_STREAM_NONE) {
            Ns_MutexLock(&curPtr->c.file.fdlock);
            curPtr->size -= (size_t)n;
            Ns_MutexUnlock(&curPtr->c.file.fdlock);
        } else {
            curPtr->size -= (size_t)n;
        }
        curPtr->nsent += n;
        curPtr->sockPtr->timeout.sec = 0;

        if (curPtr->fd != NS_INVALID_FD) {
            curPtr->c.file.bufsize -= (size_t)n;
            curPtr->c.file.bufoffset = (off_t)n;
            /* for partial transmits bufsize is now > 0 */
        } else {
            if (n < (ssize_t)toWrite) {
                /*
                 * We have a partial transmit from the iovec
                 * structure. We have to compact it to fill content in
                 * the next round.
                 */
                curPtr->c.mem.sbufIdx = Ns_ResetVec(curPtr->c.mem.sbufs, curPtr->c.mem.nsbufs, (size_t)n);
                curPtr->c.mem.nsbufs -= curPtr->c.mem.sbufIdx;

                memmove(curPtr->c.mem.sbufs, curPtr->c.mem.sbufs + curPtr->c.mem.sbufIdx,
                        /* move the iovecs to the start of the scratch buffers */
                        sizeof(struct iovec) * (size_t)curPtr->c.mem.nsbufs);
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
    int             err, pollto;
    bool            stopping;
    Ns_Time         now;
    Sock           *sockPtr;
    const Driver   *drvPtr;
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
    stopping = NS_FALSE;

    while (!stopping) {
        char charBuffer[1];

        /*
         * If there are any write sockets, set the bits.
         */

        PollReset(&pdata);
        (void)PollSet(&pdata, queuePtr->pipe[0], (short)POLLIN, NULL);

        if (writePtr == NULL) {
            pollto = 30 * 1000;
        } else {
            pollto = 1 * 1000;
            for (curPtr = writePtr; curPtr != NULL; curPtr = curPtr->nextPtr) {
                Ns_Log(DriverDebug, "### Writer pollcollect %p size %" PRIdz " streaming %d",
                       (void *)curPtr, curPtr->size, curPtr->doStream);
                if (likely(curPtr->size > 0u)) {
                    SockPoll(curPtr->sockPtr, (short)POLLOUT, &pdata);
                    pollto = -1;
                } else if (unlikely(curPtr->doStream == NS_WRITER_STREAM_FINISH)) {
                    pollto = -1;
                }
            }
        }

        /*
         * Select and drain the trigger pipe if necessary.
         */
        (void) PollWait(&pdata, pollto);

        if (PollIn(&pdata, 0) && unlikely(ns_recv(queuePtr->pipe[0], charBuffer, 1u, 0) != 1)) {
            Ns_Fatal("writer: trigger ns_recv() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }

        /*
         * Write to all available sockets
         */

        Ns_GetTime(&now);
        curPtr = writePtr;
        writePtr = NULL;

        while (curPtr != NULL) {
            NsWriterStreamState doStream;
            SpoolerState        spoolerState = SPOOLER_OK;

            nextPtr = curPtr->nextPtr;
            sockPtr = curPtr->sockPtr;
            err = 0;

            /* the truth value of doStream does not change through concurrency */
            doStream = curPtr->doStream;

            if (unlikely(PollHup(&pdata, sockPtr->pidx))) {
                Ns_Log(DriverDebug, "### Writer %p reached POLLHUP fd %d", (void *)curPtr, sockPtr->sock);
                spoolerState = SPOOLER_CLOSE;
                err = 0;

            } else if (likely(PollOut(&pdata, sockPtr->pidx)) || (doStream == NS_WRITER_STREAM_FINISH)) {
                Ns_Log(DriverDebug,
                       "### Writer %p can write to client fd %d (trigger %d) streaming %.6x"
                       " size %" PRIdz " nsent %" TCL_LL_MODIFIER "d bufsize %" PRIdz,
                       (void *)curPtr, sockPtr->sock, PollIn(&pdata, 0), doStream,
                       curPtr->size, curPtr->nsent, curPtr->c.file.bufsize);
                if (unlikely(curPtr->size < 1u)) {
                    /*
                     * Size < 0 means that everything was sent.
                     */
                    if (doStream != NS_WRITER_STREAM_ACTIVE) {
                        if (doStream == NS_WRITER_STREAM_FINISH) {
                            Ns_ReleaseTemp(curPtr->fd);
                        }
                        spoolerState = SPOOLER_CLOSE;
                    }
                } else {
                    /*
                     * If size > 0, there is still something to send.
                     * If we are spooling from a file, read some data
                     * from the (spool) file and place it into curPtr->c.file.buf.
                     */
                    if (curPtr->fd != NS_INVALID_FD) {
                        spoolerState = WriterReadFromSpool(curPtr);
                    }

                    if (spoolerState == SPOOLER_OK) {
                        spoolerState = WriterSend(curPtr, &err);
                    }
                }
            } else {

                /*
                 *  Mark when first timeout occured or check if it is already
                 *  for too long and we need to stop this socket
                 */
                if (sockPtr->timeout.sec == 0) {
                    Ns_Log(DriverDebug, "Writer %p fd %d setting sendwait %ld",
                           (void *)curPtr, sockPtr->sock, curPtr->sockPtr->drvPtr->sendwait);
                    SockTimeout(sockPtr, &now, curPtr->sockPtr->drvPtr->sendwait);
                } else if (Ns_DiffTime(&sockPtr->timeout, &now, NULL) <= 0) {
                    Ns_Log(DriverDebug, "Writer %p fd %d timeout", (void *)curPtr, sockPtr->sock);
                    err          = ETIMEDOUT;
                    spoolerState = SPOOLER_CLOSETIMEOUT;
                }
            }

            /*
             * Check result status and close the socket in case of
             * timeout or completion
             */

            Ns_MutexLock(&queuePtr->lock);
            if (spoolerState == SPOOLER_OK) {
                if (curPtr->size > 0u || doStream == NS_WRITER_STREAM_ACTIVE) {
                    Ns_Log(DriverDebug,
                           "Writer %p continue OK (size %" PRIdz ") => PUSH",
                           (void *)curPtr, curPtr->size);
                    Push(curPtr, writePtr);
                } else {
                    Ns_Log(DriverDebug,
                           "Writer %p done OK (size %" PRIdz ") => RELEASE",
                           (void *)curPtr, curPtr->size);
                    WriterSockRelease(curPtr);
                }
            } else {
                /*
                 * spoolerState might be SPOOLER_CLOSE or SPOOLER_*TIMEOUT, or SPOOLER_*ERROR
                 */
                Ns_Log(DriverDebug,
                       "Writer %p fd %d release, not OK (status %d) => RELEASE",
                       (void *)curPtr, curPtr->sockPtr->sock, (int)spoolerState);
                curPtr->status = spoolerState;
                curPtr->err    = err;
                WriterSockRelease(curPtr);
            }
            Ns_MutexUnlock(&queuePtr->lock);
            curPtr = nextPtr;
        }

        /*
         * Add more sockets to the writer queue
         */

        if (queuePtr->sockPtr != NULL) {
            Ns_MutexLock(&queuePtr->lock);
            if (queuePtr->sockPtr != NULL) {
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
    queuePtr->stopped = NS_TRUE;
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

    NS_NONNULL_ASSERT(wrSockPtr != NULL);

    Ns_Log(DriverDebug, "NsWriterFinish: %p", (void *)wrSockPtr);
    wrSockPtr->doStream = NS_WRITER_STREAM_FINISH;
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

Ns_ReturnCode
NsWriterQueue(Ns_Conn *conn, size_t nsend, Tcl_Channel chan, FILE *fp, int fd,
              struct iovec *bufs, int nbufs, int everysize)
{
    Conn          *connPtr = (Conn *)conn;
    WriterSock    *wrSockPtr;
    SpoolerQueue  *queuePtr;
    DrvWriter     *wrPtr;
    bool           trigger = NS_FALSE;
    size_t         headerSize;
    Ns_ReturnCode  status = NS_OK;

    NS_NONNULL_ASSERT(conn != NULL);

    if (unlikely(connPtr->sockPtr == NULL)) {
        Ns_Log(Warning,
               "NsWriterQueue: called without sockPtr size %" PRIdz " bufs %d flags %.6x stream %.6x chan %p fd %d",
               nsend, nbufs, connPtr->flags, connPtr->flags & NS_CONN_STREAM,
               (void *)chan, fd);
        status = NS_ERROR;
    } else {

        wrPtr = &connPtr->sockPtr->drvPtr->writer;

        Ns_Log(DriverDebug,
               "NsWriterQueue: size %" PRIdz " bufs %p (%d) flags %.6x stream %.6x chan %p fd %d thread %d",
               nsend, (void *)bufs, nbufs, connPtr->flags, connPtr->flags & NS_CONN_STREAM,
               (void *)chan, fd, wrPtr->threads);

        if (unlikely(wrPtr->threads == 0)) {
            Ns_Log(DriverDebug, "NsWriterQueue: no writer threads configured");
            status = NS_ERROR;

        } else if (nsend < (size_t)wrPtr->maxsize && everysize == 0 && connPtr->fd == 0) {
            Ns_Log(DriverDebug, "NsWriterQueue: file is too small(%" PRIdz " < %" PRIdz ")",
                   nsend, wrPtr->maxsize);
            status = NS_ERROR;
        }
    }
    if (status != NS_OK) {
        return status;
    }
        
    if (((connPtr->flags & NS_CONN_STREAM) != 0u) || connPtr->fd > 0) {
        bool        first = NS_FALSE;
        size_t      wrote = 0u;
        WriterSock *wrSockPtr1 = NULL;

        if (wrPtr->doStream == NS_WRITER_STREAM_NONE) {
            status = NS_ERROR;

        } else if (unlikely(fp != NULL || fd != NS_INVALID_FD)) {
            Ns_Log(DriverDebug, "NsWriterQueue: does not stream from this source via writer");
            status = NS_ERROR;

        } else {

            Ns_Log(DriverDebug, "NsWriterQueue: streaming writer job");
        
            if (connPtr->fd == 0) {
                /*
                 * Create a new temporary spool file.
                 */
                first = NS_TRUE;
                fd = connPtr->fd = Ns_GetTemp();

                Ns_Log(DriverDebug, "NsWriterQueue: new tmp file has fd %d", fd);
                
            } else {
                /*
                 * Reuse previously created spool file.
                 */
                wrSockPtr1 = WriterSockRequire(connPtr);
                if (wrSockPtr1 == NULL) {
                    Ns_Log(Notice,
                           "NsWriterQueue: writer job was already canceled; maybe user dropped connection.");
                    status = NS_ERROR;
                    
                } else {
                    Ns_MutexLock(&wrSockPtr1->c.file.fdlock);
                    (void)ns_lseek(connPtr->fd, 0, SEEK_END);
                }
            }
        }
        if (status != NS_OK) {
            return status;
        }

        /*
         * For the time being, handle just "string data" in streaming
         * output (iovec bufs). Write the content to the spool file.
         */
        {
            int i;

            assert(bufs != NULL);
            for (i = 0; i < nbufs; i++) {
                ssize_t j = ns_write(connPtr->fd, bufs[i].iov_base, bufs[i].iov_len);

                if (j > 0) {
                    wrote += (size_t)j;
                    Ns_Log(Debug, "NsWriterQueue: fd %d [%d] spooled %" PRIdz " of %" PRIiovlen " OK %d",
                           connPtr->fd, i, j, bufs[i].iov_len, (j == (ssize_t)bufs[i].iov_len));
                } else {
                    Ns_Log(Warning, "NsWriterQueue: spool to fd %d write operation failed",
                           connPtr->fd);
                }
            }
        }

        if (first) {
            bufs = NULL;
            connPtr->nContentSent = wrote;
#ifndef _WIN32
            /*
             * sock_set_blocking can't be used under windows, since sockets
             * are under windows no file descriptors.
             */
            (void)ns_sock_set_blocking(connPtr->fd, NS_FALSE);
#endif
            /*
             * Fall through to register stream writer with temp file
             */
        } else {
            /*
             * This is a later streaming operation, where the writer
             * job (strWriter) was previously established. Update
             * the controlling variables (size and toread), and the
             * length info for the access log, and trigger the writer
             * to notify it about the change.
             */
            assert(wrSockPtr1 != NULL);
            connPtr->strWriter->size += wrote;
            connPtr->strWriter->c.file.toRead += wrote;
            Ns_MutexUnlock(&wrSockPtr1->c.file.fdlock);

            connPtr->nContentSent += wrote;
            if (likely(wrSockPtr1->queuePtr != NULL)) {
                SockTrigger(wrSockPtr1->queuePtr->pipe[1]);
            }
            WriterSockRelease(wrSockPtr1);
            return NS_OK;
        }
    } else {
        if (fp != NULL) {
            /*
             * The client provided an open file pointer and closes it
             */
            fd = ns_dup(fileno(fp));
        } else if (fd != NS_INVALID_FD) {
            /*
             * The client provided an open file descriptor and closes it
             */
            fd = ns_dup(fd);
        } else if (chan != NULL) {
            ClientData clientData;
            /*
             * The client provided an open tcl channel and closes it
             */
            if (Tcl_GetChannelHandle(chan, TCL_READABLE, &clientData) != TCL_OK) {
                return NS_ERROR;
            }
            fd = ns_dup(PTR2INT(clientData));
        }
    }

    Ns_Log(DriverDebug, "NsWriterQueue: writer threads %d nsend %" PRIdz " maxsize %" PRIdz,
           wrPtr->threads, nsend, wrPtr->maxsize);

    assert(connPtr->poolPtr != NULL);
    /* Ns_MutexLock(&connPtr->poolPtr->threads.lock); */
    connPtr->poolPtr->stats.spool++;
    /* Ns_MutexUnlock(&connPtr->poolPtr->threads.lock); */

    wrSockPtr = (WriterSock *)ns_calloc(1u, sizeof(WriterSock));
    wrSockPtr->sockPtr = connPtr->sockPtr;
    wrSockPtr->sockPtr->timeout.sec = 0;
    wrSockPtr->flags = connPtr->flags;
    wrSockPtr->refCount = 1;

    /*
     * Make sure we have proper content length header for keep-alives
     */
    Ns_ConnSetLengthHeader(conn, nsend, (wrSockPtr->flags & NS_CONN_STREAM) != 0u);

    /*
     * Flush the headers
     */

    if ((conn->flags & NS_CONN_SENTHDRS) == 0u) {
        Tcl_DString    ds;

        Ns_DStringInit(&ds);
        Ns_Log(DriverDebug, "add header (fd %d)\n", fd);
        conn->flags |= NS_CONN_SENTHDRS;
        (void)Ns_CompleteHeaders(conn, nsend, 0u, &ds);

        headerSize = (size_t)Ns_DStringLength(&ds);
        if (headerSize > 0u) {
            wrSockPtr->headerString = ns_strdup(Tcl_DStringValue(&ds));
        }
        Ns_DStringFree(&ds);
    } else {
        headerSize = 0u;
    }

    if (fd != NS_INVALID_FD) {
        /* maybe add mmap support for files (fd != NS_INVALID_FD) */

        wrSockPtr->fd = fd;
        if (unlikely(headerSize >= wrPtr->bufsize)) {
            /*
             * We have a header which is larger than bufsize; place it
             * as "leftover" and use the headerString as buffer for file
             * reads (rather rare case)
             */
            wrSockPtr->c.file.buf = (unsigned char *)wrSockPtr->headerString;
            wrSockPtr->c.file.maxsize = headerSize;
            wrSockPtr->c.file.bufsize = headerSize;
            wrSockPtr->headerString = NULL;
        } else if (headerSize > 0u) {
            /*
             * We have a header that fits into the bufsize; place it
             * as "leftover" at the end of the buffer.
             */
            wrSockPtr->c.file.buf = ns_malloc(wrPtr->bufsize);
            memcpy(wrSockPtr->c.file.buf, wrSockPtr->headerString, headerSize);
            wrSockPtr->c.file.bufsize = headerSize;
            wrSockPtr->c.file.maxsize = wrPtr->bufsize;
            ns_free(wrSockPtr->headerString);
            wrSockPtr->headerString = NULL;
        } else {
            assert(wrSockPtr->headerString == NULL);
            wrSockPtr->c.file.buf = ns_malloc(wrPtr->bufsize);
            wrSockPtr->c.file.maxsize = wrPtr->bufsize;
        }
        wrSockPtr->c.file.bufoffset = 0;
        wrSockPtr->c.file.toRead = nsend;

    } else if (bufs != NULL) {
        int   i, j, headerbufs = (headerSize > 0u ? 1 : 0);

        wrSockPtr->fd = NS_INVALID_FD;

        if (nbufs+headerbufs < UIO_SMALLIOV) {
            wrSockPtr->c.mem.bufs = wrSockPtr->c.mem.preallocated_bufs;
        } else {
            Ns_Log(DriverDebug, "NsWriterQueue: alloc %d iovecs", nbufs);
            wrSockPtr->c.mem.bufs = ns_calloc((size_t)nbufs + (size_t)headerbufs, sizeof(struct iovec));
        }
        wrSockPtr->c.mem.nbufs = nbufs+headerbufs;
        if (headerbufs != 0) {
            wrSockPtr->c.mem.bufs[0].iov_base = wrSockPtr->headerString;
            wrSockPtr->c.mem.bufs[0].iov_len  = headerSize;
        }

        if (connPtr->fmap.addr != NULL) {
            Ns_Log(DriverDebug, "NsWriterQueue: deliver fmapped %p", (void *)connPtr->fmap.addr);
            /*
             * Deliver an mmapped file, no need to copy content
             */
            for (i = 0, j=headerbufs; i < nbufs; i++, j++) {
                wrSockPtr->c.mem.bufs[j].iov_base = bufs[i].iov_base;
                wrSockPtr->c.mem.bufs[j].iov_len  = bufs[i].iov_len;
            }
            /*
             * Make a copy of the fmap structure and make clear that
             * we unmap in the writer thread.
             */
            wrSockPtr->c.mem.fmap = connPtr->fmap;
            connPtr->fmap.addr = NULL;
            /* header string will be freed via wrSockPtr->headerString */

        } else {
            /*
             * Deliver an content from iovec. The lifetime of the
             * source is unknown, we have to copy the c.
             */
            for (i = 0, j=headerbufs; i < nbufs; i++, j++) {
                wrSockPtr->c.mem.bufs[j].iov_base = ns_malloc(bufs[i].iov_len);
                wrSockPtr->c.mem.bufs[j].iov_len  = bufs[i].iov_len;
                memcpy(wrSockPtr->c.mem.bufs[j].iov_base, bufs[i].iov_base, bufs[i].iov_len);
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


    if (connPtr->clientData != NULL) {
        wrSockPtr->clientData = ns_strdup(connPtr->clientData);
    }
    wrSockPtr->startTime = *Ns_ConnStartTime(conn);

    /*
     * Setup streaming context before sending potentially headers.
     */

    if ((wrSockPtr->flags & NS_CONN_STREAM) != 0u) {
        wrSockPtr->doStream = NS_WRITER_STREAM_ACTIVE;
        assert(connPtr->strWriter == NULL);
        /*
         * Add a reference to the stream writer to the connection such
         * it can efficiently append to a stream when multiple output
         * operations happen. The backpointer (from the stream writer
         * to the connection is needed to clear the reference to the
         * writer in case the writer is deleted. No locks are needed,
         * since nobody can share this structure yet.
         */
        connPtr->strWriter = wrSockPtr;
        wrSockPtr->connPtr = connPtr;
    }

    /*
     * Tell connection, that writer handles the output (including
     * closing the connection to the client).
     */

    connPtr->flags |= NS_CONN_SENT_VIA_WRITER;

    wrSockPtr->keep = connPtr->keep > 0 ? NS_TRUE : NS_FALSE;
    wrSockPtr->size = nsend;

    if ((wrSockPtr->flags & NS_CONN_STREAM) == 0u) {
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
    wrSockPtr->queuePtr = queuePtr;

    Ns_MutexLock(&queuePtr->lock);
    if (queuePtr->sockPtr == NULL) {
        trigger = NS_TRUE;
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

/*
 *----------------------------------------------------------------------
 *
 * DriverWriterFromObj --
 *
 *      Lookup driver by name and return its DrvWriter.
 *
 * Results:
 *      DrvWriter or NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static DrvWriter *
DriverWriterFromObj(Tcl_Obj *driverObj) {
    Driver     *drvPtr;
    const char *driverName;
    int         driverNameLen;
    DrvWriter  *wrPtr = NULL;

    NS_NONNULL_ASSERT(driverObj != NULL);

    driverName = Tcl_GetStringFromObj(driverObj, &driverNameLen);

    for (drvPtr = firstDrvPtr; drvPtr != NULL; drvPtr = drvPtr->nextPtr) {
        if (strncmp(driverName, drvPtr->threadName, (size_t)driverNameLen) == 0) {
            if (drvPtr->writer.firstPtr != NULL) {
                wrPtr = &drvPtr->writer;
            }
            break;
        }
    }
    return wrPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * WriterSubmitObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer submit" command.
 *      Send the provided data to the client.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterSubmitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int               result = TCL_OK;
    Ns_Conn          *conn;
    Tcl_Obj          *dataObj;
    Ns_ObjvSpec       args[] = {
        {"data", Ns_ObjvObj,  &dataObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK
        || NsConnRequire(interp, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else {
        int            size;
        unsigned char *data = Tcl_GetByteArrayFromObj(dataObj, &size);

        if (data != NULL) {
            struct iovec  vbuf;
            Ns_ReturnCode status;

            vbuf.iov_base = (void *)data;
            vbuf.iov_len = (size_t)size;

            status = NsWriterQueue(conn, (size_t)size, NULL, NULL, -1, &vbuf, 1, 1);
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(status == NS_OK ? 1 : 0));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterSubmitFileObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer submitfile" command.
 *      Send the provided file to the client.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterSubmitFileObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;
    Ns_Conn    *conn;
    char       *fileNameString;
    int         headers = 0;
    Tcl_WideInt offset = 0, size = 0;
    Ns_ObjvSpec lopts[] = {
        {"-headers",  Ns_ObjvBool,    &headers, INT2PTR(NS_TRUE)},
        {"-offset",   Ns_ObjvWideInt, &offset,  NULL},
        {"-size",     Ns_ObjvWideInt, &size,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"file",      Ns_ObjvString, &fileNameString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK)
        || NsConnRequire(interp, &conn) != NS_OK) {
        result = TCL_ERROR;

    } else if (unlikely( Ns_ConnSockPtr(conn) == NULL )) {
        Ns_Log(Warning,
               "NsWriterQueue: called without valid sockPtr, maybe connection already closed");
        Ns_TclPrintfResult(interp, "0");
        result = TCL_OK;

    } else {
        size_t      nrbytes = 0u;
        struct stat st;
        int         fd, rc = stat(fileNameString, &st);

        if (unlikely(rc != 0)) {
            Ns_TclPrintfResult(interp, "file does not exist '%s'", fileNameString);
            result = TCL_ERROR;
        }

        fd = NS_INVALID_FD;
        if (likely( result == TCL_OK )) {

            fd = ns_open(fileNameString, O_RDONLY, 0);
            if (unlikely(fd == NS_INVALID_FD)) {
                Ns_TclPrintfResult(interp, "could not open file '%s'", fileNameString);
                result = TCL_ERROR;

            } else if (unlikely(size < 0 || size > st.st_size)) {
                Ns_TclPrintfResult(interp, "size must be a positive value less or equal filesize");
                result = TCL_ERROR;

            } else if (unlikely(offset < 0 || offset > st.st_size)) {
                Ns_TclPrintfResult(interp, "offset must be a positive value less or equal filesize");
                result = TCL_ERROR;

            } else if (size > 0) {
                if (unlikely(size + offset > st.st_size)) {
                    Ns_TclPrintfResult(interp, "offset + size must be less or equal filesize");
                    result = TCL_ERROR;
                } else {
                    nrbytes = (size_t)size;
                }
            } else {
                nrbytes = (size_t)st.st_size - (size_t)offset;
            }
        }

        if (offset > 0 && result == TCL_OK) {
            if (ns_lseek(fd, (off_t)offset, SEEK_SET) == -1) {
                Ns_TclPrintfResult(interp, "cannot seek to position %ld", (long)offset);
                result = TCL_ERROR;
            }
        }

        if (likely(result == TCL_OK)) {
            Ns_ReturnCode status;

            /*
             *  The caller requested that we build required headers
             */

            if (headers != 0) {
                Ns_ConnSetTypeHeader(conn, Ns_GetMimeType(fileNameString));
            }
            status = NsWriterQueue(conn, nrbytes, NULL, NULL, fd, NULL, 0, 1);
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(status == NS_OK ? 1 : 0));

            (void) ns_close(fd);
        }

        if (fd != NS_INVALID_FD) {
            (void) ns_close(fd);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterListObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer list" command.
 *      List the current writer jobs.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          result = TCL_OK;
    NsServer    *servPtr = NULL;
    Ns_ObjvSpec  lopts[] = {
        {"-server",  Ns_ObjvServer,    &servPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (unlikely(Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK)) {
        result = TCL_ERROR;

    } else {
        Tcl_DString       ds, *dsPtr = &ds;
        const Driver     *drvPtr;
        SpoolerQueue     *queuePtr;

        Tcl_DStringInit(dsPtr);

        for (drvPtr = firstDrvPtr; drvPtr != NULL; drvPtr = drvPtr->nextPtr) {
            const DrvWriter *wrPtr;

            /*
             * if server was specified, list only results from this server.
             */
            if (servPtr != NULL && servPtr != drvPtr->servPtr) {
                continue;
            }

            wrPtr = &drvPtr->writer;
            queuePtr = wrPtr->firstPtr;
            while (queuePtr != NULL) {
                const WriterSock *wrSockPtr;

                Ns_MutexLock(&queuePtr->lock);
                wrSockPtr = queuePtr->curPtr;
                while (wrSockPtr != NULL) {
                    char ipString[NS_IPADDR_SIZE];

                    (void) Ns_DStringNAppend(dsPtr, "{", 1);
                    (void) Ns_DStringAppendTime(dsPtr, &wrSockPtr->startTime);
                    (void) Ns_DStringNAppend(dsPtr, " ", 1);
                    (void) Ns_DStringAppend(dsPtr, queuePtr->threadname);
                    (void) Ns_DStringNAppend(dsPtr, " ", 1);
                    (void) Ns_DStringAppend(dsPtr, drvPtr->threadName);
                    (void) Ns_DStringNAppend(dsPtr, " ", 1);
                    (void) Ns_DStringAppend(dsPtr, ns_inet_ntop((struct sockaddr *)&(wrSockPtr->sockPtr->sa),
                                                                ipString,
                                                                sizeof(ipString)));
                    (void) Ns_DStringPrintf(dsPtr, " %d %" PRIdz " %" TCL_LL_MODIFIER "d ",
                                            wrSockPtr->fd, wrSockPtr->size, wrSockPtr->nsent);
                    (void) Ns_DStringAppendElement(dsPtr,
                                                   (wrSockPtr->clientData != NULL) ? wrSockPtr->clientData : "");
                    (void) Ns_DStringNAppend(dsPtr, "} ", 2);
                    wrSockPtr = wrSockPtr->nextPtr;
                }
                Ns_MutexUnlock(&queuePtr->lock);
                queuePtr = queuePtr->nextPtr;
            }
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterSizeObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer size" command.
 *      Sets or queries lower limit for sending via writer.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterSizeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          intValue = -1, result = TCL_OK;
    Tcl_Obj     *driverObj;
    Ns_ObjvSpec  args[] = {
        {"driver", Ns_ObjvObj, &driverObj, NULL},
        {"?value", Ns_ObjvInt, &intValue, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        DrvWriter *wrPtr = DriverWriterFromObj(driverObj);

        if (unlikely(wrPtr == NULL)) {
            Ns_TclPrintfResult(interp, "no writer configured for a driver with name %s",
                               Tcl_GetString(driverObj));
            result = TCL_ERROR;

        } else if (objc == 4) {
            /*
             * The optional argument was provided.
             */
            if (intValue < 1024) {
                Ns_TclPrintfResult(interp,
                                   "argument is not an integer in valid range: %d (min 1024)",
                                   intValue);
                result = TCL_ERROR;
            } else {
                wrPtr->maxsize = (size_t)intValue;
            }
        }
        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj((int)wrPtr->maxsize));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * WriterStreamingObjCmd - subcommand of NsTclWriterObjCmd --
 *
 *      Implements "ns_writer streaming" command.
 *      Sets or queries streaming state of the writer.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
WriterStreamingObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          boolValue, result = TCL_OK;
    Tcl_Obj     *driverObj;
    Ns_ObjvSpec  args[] = {
        {"driver", Ns_ObjvObj,  &driverObj, NULL},
        {"?value", Ns_ObjvBool, &boolValue, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        DrvWriter *wrPtr = DriverWriterFromObj(driverObj);

        if (unlikely(wrPtr == NULL)) {
            Ns_TclPrintfResult(interp, "no writer configured for a driver with name %s",
                               Tcl_GetString(driverObj));
            result = TCL_ERROR;

        } else if (objc == 4) {
            /*
             * The optional argument was provided.
             */
            wrPtr->doStream = (boolValue == 1 ? NS_WRITER_STREAM_ACTIVE : NS_WRITER_STREAM_NONE);
        }

        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(wrPtr->doStream));
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclWriterObjCmd --
 *
 *      Implements "ns_writer" command for submitting data to the writer
 *      threads and to configure and query the state of the writer threads at
 *      runtime.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
NsTclWriterObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"list",       WriterListObjCmd},
        {"size",       WriterSizeObjCmd},
        {"streaming",  WriterStreamingObjCmd},
        {"submit",     WriterSubmitObjCmd},
        {"submitfile", WriterSubmitFileObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
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
NsAsyncWriterQueueEnable(void)
{
    if (Ns_ConfigBool(NS_CONFIG_PARAMETERS, "asynclogwriter", NS_FALSE) == NS_TRUE) {
        SpoolerQueue  *queuePtr;

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
                asyncWriter = ns_calloc(1u, sizeof(AsyncWriter));
                Ns_MutexUnlock(&reqLock);
                Ns_MutexSetName2(&asyncWriter->lock, "ns:driver", "async-writer");
                /*
                 * Allocate and initialize a Spooler Queue for this thread.
                 */
                queuePtr = ns_calloc(1u, sizeof(SpoolerQueue));
                Ns_MutexSetName2(&queuePtr->lock, "ns:driver:async-writer", "queue");
                asyncWriter->firstPtr = queuePtr;
                /*
                 * Start the spooler queue
                 */
                SpoolerQueueStart(queuePtr, AsyncWriterThread);

            } else {
                Ns_MutexUnlock(&reqLock);
            }
        }


        assert(asyncWriter != NULL);
        queuePtr = asyncWriter->firstPtr;
        assert(queuePtr != NULL);

        Ns_MutexLock(&queuePtr->lock);
        queuePtr->stopped = NS_FALSE;
        Ns_MutexUnlock(&queuePtr->lock);
    }
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
NsAsyncWriterQueueDisable(bool shutdown)
{
    if (asyncWriter != NULL) {
        SpoolerQueue *queuePtr = asyncWriter->firstPtr;
        Ns_Time       timeout;

        assert(queuePtr != NULL);

        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, nsconf.shutdowntimeout, 0);

        Ns_MutexLock(&queuePtr->lock);
        queuePtr->stopped = NS_TRUE;
        queuePtr->shutdown = shutdown;

        /*
         * Trigger the AsyncWriter Thread to drain the spooler queue.
         */
        SockTrigger(queuePtr->pipe[1]);
        (void)Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, &timeout);

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
Ns_ReturnCode
NsAsyncWrite(int fd, const char *buffer, size_t nbyte)
{
    Ns_ReturnCode returnCode = NS_OK;

    NS_NONNULL_ASSERT(buffer != NULL);

    /*
     * If the async writer has not started or is deactivated, behave
     * like a ns_write() command. If the ns_write() fails, we can't do much,
     * since writing of an error message to the log might bring us
     * into an infinte loop.
     */
    if (asyncWriter == NULL || asyncWriter->firstPtr->stopped) {
        ssize_t written = ns_write(fd, buffer, nbyte);

        if (unlikely(written != (ssize_t)nbyte)) {
            WriteError("sync write", fd, nbyte, written);
        }
        returnCode = NS_ERROR;

    } else {
        SpoolerQueue         *queuePtr;
        bool                  trigger = NS_FALSE;
        const AsyncWriteData *wdPtr;
        AsyncWriteData       *newWdPtr;

        /*
         * Allocate a writer cmd and initialize it. In order to provide an
         * interface compatible to ns_write(), we copy the provided data,
         * such it can be freed by the caller. Wen we would give up the
         * interface, we could free the memory block after writing, and
         * save a malloc/free operation on the data.
         */
        newWdPtr = ns_calloc(1u, sizeof(AsyncWriteData));
        newWdPtr->fd = fd;
        newWdPtr->bufsize = nbyte;
        newWdPtr->data = ns_malloc(nbyte + 1u);
        memcpy(newWdPtr->data, buffer, newWdPtr->bufsize);
        newWdPtr->buf  = newWdPtr->data;
        newWdPtr->size = newWdPtr->bufsize;

        /*
         * Now add new writer socket to the writer thread's queue. In most
         * cases, the queue will be empty.
         */
        queuePtr = asyncWriter->firstPtr;
        assert(queuePtr != NULL);

        Ns_MutexLock(&queuePtr->lock);
        wdPtr = queuePtr->sockPtr;
        if (wdPtr != NULL) {
            newWdPtr->nextPtr = queuePtr->sockPtr;
            queuePtr->sockPtr = newWdPtr;
        } else {
            queuePtr->sockPtr = newWdPtr;
            trigger = NS_TRUE;
        }
        Ns_MutexUnlock(&queuePtr->lock);

        /*
         * Wake up writer thread if desired
         */
        if (trigger) {
            SockTrigger(queuePtr->pipe[1]);
        }
    }

    return returnCode;
}

/*
 *----------------------------------------------------------------------
 *
 * AsyncWriterRelease --
 *
 *      Deallocate write data.
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
AsyncWriterRelease(AsyncWriteData *wdPtr)
{
    NS_NONNULL_ASSERT(wdPtr != NULL);

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
    char            charBuffer[1];
    int             pollto;
    Ns_ReturnCode   status;
    bool            stopping;
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
    stopping = NS_FALSE;

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
        (void)PollSet(&pdata, queuePtr->pipe[0], (short)POLLIN, NULL);

        if (writePtr == NULL) {
            pollto = 30 * 1000;
        } else {
            pollto = 0;
        }

        /*
         * Wait for data
         */
        /*n =*/ (void) PollWait(&pdata, pollto);

        /*
         * Select and drain the trigger pipe if necessary.
         */
        if (PollIn(&pdata, 0)) {
            if (ns_recv(queuePtr->pipe[0], charBuffer, 1u, 0) != 1) {
                Ns_Fatal("asynclogwriter: trigger ns_recv() failed: %s",
                         ns_sockstrerror(ns_sockerrno));
            }
            if (queuePtr->stopped) {
                /*
                 * Drain the queue from everything
                 */
                for (curPtr = writePtr; curPtr != NULL;  curPtr = curPtr->nextPtr) {
                    ssize_t written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
                    if (unlikely(written != (ssize_t)curPtr->bufsize)) {
                        WriteError("drain writer", curPtr->fd, curPtr->bufsize, written);
                    }
                }
                writePtr = NULL;

                for (curPtr = queuePtr->sockPtr; curPtr != NULL;  curPtr = curPtr->nextPtr) {
                    ssize_t written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
                    if (unlikely(written != (ssize_t)curPtr->bufsize)) {
                        WriteError("drain queue", curPtr->fd, curPtr->bufsize, written);
                    }
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
            ssize_t written;

            nextPtr = curPtr->nextPtr;
            status = NS_OK;

            /*
             * Write the actual data and allow for partial write operations.
             */
            written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
            if (unlikely(written < 0)) {
                status = NS_ERROR;
            } else {
                curPtr->size -= (size_t)written;
                curPtr->nsent += written;
                curPtr->bufsize -= (size_t)written;
                if (curPtr->data != NULL) {
                    curPtr->buf += written;
                }
            }

            if (unlikely(status != NS_OK)) {
                AsyncWriterRelease(curPtr);
                queuePtr->queuesize--;
            } else {

                /*
                 * The write operation was successful. Check if there
                 * is some remaining data to write. If not we are done
                 * with this request can can release the write buffer.
                 */
                if (curPtr->size > 0u) {
                    Push(curPtr, writePtr);
                } else {
                    AsyncWriterRelease(curPtr);
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
                ssize_t written = ns_write(curPtr->fd, curPtr->buf, curPtr->bufsize);
                if (unlikely(written != (ssize_t)curPtr->bufsize)) {
                    WriteError("shutdown", curPtr->fd, curPtr->bufsize, written);
                }
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

    queuePtr->stopped = NS_TRUE;
    Ns_Log(Notice, "exiting");

}

/*
 *----------------------------------------------------------------------
 *
 * NSDriverClientOpen --
 *
 *      Open an client HTTP connection using the driver interface
 *
 * Results:
 *      Tcl return code.
 *
 * Side effects:
 *      Opening a connection
 *
 *----------------------------------------------------------------------
 */

int
NSDriverClientOpen(Tcl_Interp *interp, const char *driverName,
                   const char *url, const char *httpMethod, const char *version,
                   const Ns_Time *timeoutPtr, Sock **sockPtrPtr)
{
    char   *protocol, *host, *portString, *path, *tail, *url2;
    int     result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(httpMethod != NULL);
    NS_NONNULL_ASSERT(version != NULL);
    NS_NONNULL_ASSERT(sockPtrPtr != NULL);

    url2 = ns_strdup(url);

    if (unlikely(Ns_ParseUrl(url2, &protocol, &host, &portString, &path, &tail) != NS_OK)) {
        result = TCL_ERROR;

    } else {
        Driver        *drvPtr;
        unsigned short portNr = 0u; /* make static checker happy */

        assert(protocol != NULL);
        assert(host != NULL);
        assert(path != NULL);
        assert(tail != NULL);

        /*
         * Find a matching driver for the specified protocol and optionally
         * the specified driver name.
         */
        for (drvPtr = firstDrvPtr; drvPtr != NULL;  drvPtr = drvPtr->nextPtr) {
            Ns_Log(DriverDebug, "... check Driver proto <%s> server %s name %s location %s",
                   drvPtr->protocol, drvPtr->server, drvPtr->threadName, drvPtr->location);
            if (STREQ(drvPtr->protocol, protocol)) {
                if (driverName == NULL) {
                    /*
                     * If there is no driver name given, take the first driver
                     * with the matching protocol.
                     */
                    break;
                } else if (STREQ(drvPtr->moduleName, driverName)) {
                    /*
                     * The driver name (name of the loaded module) is equal
                     */
                    break;
                }
            }
        }
        if (drvPtr == NULL) {
            if (driverName != NULL) {
                Ns_TclPrintfResult(interp, "no driver for protocol '%s' & driver name '%s' found", protocol, driverName);
            } else {
                Ns_TclPrintfResult(interp, "no driver for protocol '%s' found", protocol);
            }
            result = TCL_ERROR;

        } else if (portString != NULL) {
            portNr = (unsigned short) strtol(portString, NULL, 10);
        } else if (STREQ(drvPtr->protocol, "http")) {
            /*
             * The default port should be in the driver structure.
             */
            portNr = 80u;
        } else if (STREQ(drvPtr->protocol, "https")) {
            portNr = 443u;
        } else {
            Ns_TclPrintfResult(interp, "no default port for protocol '%s' defined", protocol);
            result = TCL_ERROR;
        }

        if (result == TCL_OK) {
            NS_SOCKET      sock;

            sock = Ns_SockTimedConnect2(host, portNr, NULL, 0u, timeoutPtr);

            if (sock == NS_INVALID_SOCKET) {
                Ns_TclPrintfResult(interp, "connect to '%s' failed: %s", url,
                                   ns_sockstrerror(ns_sockerrno));
                result = TCL_ERROR;

            } else {
                const char    *query;
                Tcl_DString    ds, *dsPtr = &ds;
                Request       *reqPtr;
                Sock          *sockPtr = SockNew(drvPtr);

                sockPtr->sock = sock;
                sockPtr->servPtr  = drvPtr->servPtr;
                if (sockPtr->servPtr == NULL) {
                    const NsInterp *itPtr = NsGetInterpData(interp);

                    sockPtr->servPtr = itPtr->servPtr;
                }

                RequestNew(sockPtr);

                Ns_GetTime(&sockPtr->acceptTime);
                reqPtr = sockPtr->reqPtr;

                Tcl_DStringInit(dsPtr);
                Ns_DStringAppend(dsPtr, httpMethod);
                Ns_StrToUpper(Ns_DStringValue(dsPtr));
                Tcl_DStringAppend(dsPtr, " /", 2);
                if (*path != '\0') {
                    if (*path == '/') {
                        path ++;
                    }
                    Tcl_DStringAppend(dsPtr, path, -1);
                    Tcl_DStringAppend(dsPtr, "/", 1);
                }
                Tcl_DStringAppend(dsPtr, tail, -1);
                Tcl_DStringAppend(dsPtr, " HTTP/", 6);
                Tcl_DStringAppend(dsPtr, version, -1);

                reqPtr->request.line = Ns_DStringExport(dsPtr);
                reqPtr->request.method = ns_strdup(httpMethod);
                reqPtr->request.protocol = ns_strdup(protocol);
                reqPtr->request.host = ns_strdup(host);
                query = strchr(tail, INTCHAR('?'));
                if (query != NULL) {
                    reqPtr->request.query = ns_strdup(query+1);
                } else {
                    reqPtr->request.query = NULL;
                }
                /*Ns_Log(Notice, "REQUEST LINE <%s> query <%s>", reqPtr->request.line, reqPtr->request.query);*/

                *sockPtrPtr = sockPtr;
            }
        }
    }
    ns_free(url2);

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
