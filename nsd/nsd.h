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

#ifndef NSD_H
#define NSD_H

/* Needed for SSL support on Windows: */
#if defined(_MSC_VER) && !defined(HAVE_CONFIG_H)
#  include "nsconfig-win32.h"
#endif

#include "ns.h"

/*
 * Constants
 */

#define NS_CONFIG_PARAMETERS           "ns/parameters"
#define NS_CONFIG_THREADS              "ns/threads"

NS_EXTERN const char *NS_EMPTY_STRING;

/*
 * Various ADP option bits.
 */

#define ADP_SAFE                       0x01u    /* Use Tcl_SafeEval for ADP */
#define ADP_SINGLE                     0x02u    /* Combine blocks into a single script */
#define ADP_DEBUG                      0x04u    /* Enable debugging */
#define ADP_EXPIRE                     0x08u    /* Send Expires: now header on output */
#define ADP_CACHE                      0x10u    /* Enable output caching */
#define ADP_TRACE                      0x20u    /* Trace execution */
#define ADP_DETAIL                     0x80u    /* Log connection details on error */
#define ADP_STRICT                     0x100u   /* Strict error handling */
#define ADP_DISPLAY                    0x200u   /* Display error messages in output stream */
#define ADP_TRIM                       0x400u   /* Display error messages in output stream */
#define ADP_FLUSHED                    0x800u   /* Some output has been sent */
#define ADP_ERRLOGGED                  0x1000u  /* Error message has already been logged */
#define ADP_AUTOABORT                  0x2000u  /* Raise abort on flush error */
#define ADP_ADPFILE                    0x4000u  /* Object to evaluate is a file */
#define ADP_STREAM                     0x8000u  /* Enable ADP streaming */
#define ADP_TCLFILE                    0x10000u /* Object to evaluate is a Tcl file */
#define ADP_OPTIONMAX                  0x1000000u /* watermark for flag values */

typedef enum {
    ADP_OK =                     0,
    ADP_BREAK =                  1,
    ADP_ABORT =                  2,
    ADP_RETURN =                 3,
    ADP_TIMEOUT =                4
} AdpResult;

typedef enum {
    NS_URLSPACE_DEFAULT =        0,
    NS_URLSPACE_FAST =           1,
    NS_URLSPACE_EXACT =          2
} NsUrlSpaceOp;


typedef bool (NsUrlSpaceContextFilterProc) (void *contextSpec, void *context);

typedef struct NsUrlSpaceContext {
    const Ns_Set *headers;
    struct sockaddr *saPtr;
} NsUrlSpaceContext;

typedef struct _NsUrlSpaceContextSpec  NsUrlSpaceContextSpec;

/*
 * Managing streaming output via writer
 */
typedef enum {
    NS_WRITER_STREAM_NONE =        0,
    NS_WRITER_STREAM_ACTIVE =      1,
    NS_WRITER_STREAM_FINISH =      2
} NsWriterStreamState;

#define MAX_URLSPACES                  16
#define MAX_LISTEN_ADDR_PER_DRIVER     16

#define NS_SET_SIZE                    ((unsigned)TCL_INTEGER_SPACE + 2u)
#define NS_MAX_RANGES                  32

#define CONN_TCLFORM                   0x01u  /* Query form set is registered for interp */
#define CONN_TCLHDRS                   0x02u  /* Input headers set is registered for interp */
#define CONN_TCLOUTHDRS                0x04u  /* Output headers set is registered for interp */
#define CONN_TCLAUTH                   0x08u  /* 'auth' headers set is registered for interp */
#define CONN_TCLHTTP                   0x10u  /* HTTP headers requested by ns_headers */

/*
 * Flags for 2nd arg of NsConnRequire()
 */
#define NS_CONN_REQUIRE_CONNECTED   0x0001u
#define NS_CONN_REQUIRE_OPEN        0x0002u
#define NS_CONN_REQUIRE_CONFIGURED  0x0004u
#define NS_CONN_REQUIRE_ALL         0x0007u

/*
 * The following is the default text/html content type
 * sent to the browser for html/adp etc. requests.
 */

#define NSD_TEXTHTML                   "text/html"


/*
 * Type definitions.
 */

struct Sock;
struct NsServer;
typedef struct NsWriterSock NsWriterSock;

struct nsconf {
    const char *argv0;
    const char *nsd;
    const char *name;
    const char *version;
    const char *home;
    const char *tmpDir;
    const char *config;
    const char *build;
    pid_t       pid;
    time_t      boot_t;
    char        hostname[255];
    char        address[NS_IPADDR_SIZE];
    Ns_Time     shutdowntimeout;
    int         backlog;
    int         sanitize_logfiles;
    bool        reject_already_closed_connection;

    /*
     * Slot IDs for socket local storage.
     */

    uintptr_t   nextSlsId;

    /*
     * The following table holds the configured virtual servers.
     * The dstring maintains a Tcl list of the names.
     */

    Tcl_HashTable servertable;
    Tcl_DString   servers;
    const char   *defaultServer;

    /*
     * The following table holds config section sets from
     * the configuration file.
     */

    Tcl_HashTable sections;

    /*
     * The following struct maintains server state.
     */

    struct {
        Ns_Mutex lock;
        Ns_Cond cond;
        int pipefd[2];
        bool started;
        bool stopping;
    } state;

    struct {
        Ns_Time maxelapsed;
        int jobsperthread;
    } sched;

#ifdef _WIN32
    struct {
        bool checkexit;
    } exec;
#endif

    struct {
        const char *sharedlibrary;
        const char *version;
        bool        lockoninit;
    } tcl;

    struct {
        Ns_Time timeout;
        Ns_Time logminduration;
        int     jobsperthread;
    } job;
};

NS_EXTERN struct nsconf nsconf;

/*
 * The following structure tracks a memory-mapped file
 * in a platform-neutral way.
 */

typedef struct FileMap {
    void  *addr;           /* Mapped to this virtual address */
    size_t size;                /* Size of the mapped region */
#ifndef _WIN32
    int handle;                 /* OS handle of the opened/mapped file */
#else
    HANDLE handle;              /* OS handle of the opened/mapped file */
    void *mapobj;               /* Mapping object (Win32 only) */
#endif
} FileMap;

/*
 * The following structure maintains a queue of sockets for
 * each writer or spooler thread
 */

typedef struct SpoolerQueue {
    struct SpoolerQueue *nextPtr;
    void                *sockPtr;     /* List of submitted socket structures */
    void                *curPtr;      /* List of processed socket structures */
    NS_SOCKET            pipe[2];     /* Trigger to wakeup WriterThread/SpoolerThread */
    Ns_Mutex             lock;        /* Lock around spooled list */
    Ns_Cond              cond;        /* Cond for stopped flag */
    Ns_Thread            thread;      /* Running WriterThread/Spoolerthread */
    int                  id;          /* Queue id */
    int                  queuesize;   /* Number of active sockets in the queue */
    const char          *threadName;  /* Name of the thread working on this queue */
    bool                 stopped;     /* Flag to indicate thread stopped */
    bool                 shutdown;    /* Flag to indicate shutdown */
} SpoolerQueue;


/*
 * The following structure maintains an ADP call frame.
 */

typedef struct AdpFrame {
    struct AdpFrame   *prevPtr;
    time_t             mtime;
    off_t              size;
    Tcl_Obj          *ident;
    Tcl_Obj          **objv;
    char              *savecwd;
    const char        *file;
    Ns_DString         cwdbuf;
    Tcl_DString       *outputPtr;
    unsigned int       flags;
    unsigned short     line;
    unsigned short     objc;
} AdpFrame;


/*
 * The following structure defines blocks of ADP.  The
 * len pointer is an array of ints with positive values
 * indicating text to copy and negative values indicating
 * scripts to evaluate.  The text and script chars are
 * packed together without null char separators starting
 * at base.  The len data is stored at the end of the
 * text dstring when parsing is complete.
 */

typedef struct AdpCode {
    int         nblocks;
    int         nscripts;
    int        *len;
    int        *line;
    Tcl_DString text;
} AdpCode;

#define AdpCodeLen(cp,i)    ((cp)->len[(i)])
#define AdpCodeLine(cp,i)   ((cp)->line[(i)])
#define AdpCodeText(cp)     ((cp)->text.string)
#define AdpCodeBlocks(cp)   ((cp)->nblocks)
#define AdpCodeScripts(cp)  ((cp)->nscripts)

/*
 * Dynamic list structures. These are an alternative to e.g. double linked
 * lists, but are more local in memory pages and are therefore better for
 * cache hits.
 */
typedef struct Ns_DList {
    void   **data;
    size_t   size;
    size_t   avail;
    void    *static_data[30];
} Ns_DList;


/*
 * The following structure defines the entire request
 * including HTTP request line, headers, and content.
 */

typedef struct Request {
    struct Request *nextPtr;     /* Next on free list */
    Ns_Request request;          /* Parsed request line */
    Ns_Set *headers;             /* Input headers */
    Ns_Set *auth;                /* Auth user/password and parameters */
    char peer[NS_IPADDR_SIZE];   /* Client peer address */
    unsigned short port;         /* Client peer port */

    /*
     * The following pointers are used to access the
     * buffer contents after the read-ahead is complete.
     */

    char *next;                 /* Next read offset */
    char *content;              /* Start of content */
    size_t length;              /* Length of content */
    size_t contentLength;       /* Provided content length */
    size_t avail;               /* Bytes avail in buffer */

    /*
     * The following block is for chunked encodings
     */
    size_t expectedLength;      /* Provided expected length */
    size_t chunkStartOff;       /* Offset pointing to start of chunk to be parsed */
    size_t chunkWriteOff;       /* Offset pointing to position were to write chunk */

    /*
     * The following offsets are used to manage
     * the buffer read-ahead process.
     */

    size_t woff;                  /* Next write buffer offset */
    size_t roff;                  /* Next read buffer offset */
    size_t coff;                  /* Content buffer offset */
    size_t leftover;              /* Leftover bytes from earlier requests */
    Tcl_DString buffer;           /* Request and content buffer */
    char   savedChar;             /* Character potentially clobbered by null character */

} Request;

/*
 * The following structure maitains data for each instance of
 * a driver initialized with Ns_DriverInit.
 */

typedef struct {
    Ns_Mutex lock;                      /* Lock around spooler queue */
    SpoolerQueue *firstPtr;             /* Spooler thread queue */
    SpoolerQueue *curPtr;               /* Current spooler thread */
    int threads;                        /* Number of spooler threads to run */
} DrvSpooler;

typedef struct {
    size_t              writersize;     /* Use writer thread above this size */
    size_t              bufsize;        /* Size of the output buffer */
    Ns_Mutex            lock;           /* Lock around writer queues */
    SpoolerQueue       *firstPtr;       /* List of writer threads */
    SpoolerQueue       *curPtr;         /* Current writer thread */
    int                 threads;        /* Number of writer threads to run */
    int                 rateLimit;      /* Limit transmission rate in KB/s for a writer job */
    NsWriterStreamState doStream;       /* Activate writer for HTML streaming */
} DrvWriter;

/*
 * ServerMap maintains Host header to server mappings, but is upaque for nsd.h
 */
struct ServerMap;

/*
 * Driver data structure
 */

typedef struct Driver {

    /*
     * Visible in Ns_Driver.
     */

    void        *arg;                   /* Driver callback data */
    const char  *server;                /* Virtual server name */
    const char  *type;                  /* Type of driver, e.g. "nssock" */
    const char  *moduleName;            /* Module name, e.g. "nssock1" */
    const char  *threadName;            /* Thread name, e.g. "nssock1:1" */
    const char  *location;              /* Location, e.g, "http://foo:9090" */
    const char  *address;               /* Tcl list of IP addresses to bind to */
    const char  *protocol;              /* Protocol in location, e.g, "http" */
    Ns_Time      sendwait;              /* send() I/O timeout */
    Ns_Time      recvwait;              /* recv() I/O timeout */
    size_t       bufsize;               /* Conn bufsize (0 for SSL) */
    const Ns_Set *extraHeaders;         /* Extra header fields added for every request */

    /*
     * Private to Driver.
     */

    struct Driver           *nextPtr;     /* Next in list of drivers */
    struct NsServer         *servPtr;     /* Driver virtual server */
    Ns_DriverListenProc     *listenProc;
    Ns_DriverAcceptProc     *acceptProc;
    Ns_DriverRecvProc       *recvProc;
    Ns_DriverSendProc       *sendProc;
    Ns_DriverSendFileProc   *sendFileProc; /* Optional - optimize direct file send. */
    Ns_DriverKeepProc       *keepProc;
    Ns_DriverRequestProc    *requestProc;
    Ns_DriverCloseProc      *closeProc;
    Ns_DriverClientInitProc *clientInitProc; /* Optional - initialization of client connections */

    const char *defserver;              /* default server, might be NULL */
    Tcl_HashTable hosts;                /* Virtual hosts mapping to server */
    const struct ServerMap *defMapPtr;  /* Default for virtual host entry */
    Ns_Time closewait;                  /* Graceful close timeout */
    Ns_Time keepwait;                   /* Keepalive timeout */
    size_t keepmaxdownloadsize;         /* When set, allow keepalive only for download requests up to this size */
    size_t keepmaxuploadsize;           /* When set, allow keepalive only for upload requests up to this size */
    Ns_Mutex lock;                      /* Lock to protect lists below. */
    NS_SOCKET listenfd[MAX_LISTEN_ADDR_PER_DRIVER];  /* Listening sockets */
    NS_POLL_NFDS_TYPE pidx[MAX_LISTEN_ADDR_PER_DRIVER]; /* poll() index */
    unsigned int opts;                  /* NS_DRIVER_* options */
    int backlog;                        /* listen() backlog */
    Tcl_WideInt maxinput;               /* Maximum request bytes to read */
    Tcl_WideInt maxupload;              /* Uploads that exceed will go into temp file without parsing */
    const char *uploadpath;             /* Path where uploaded files will be spooled */
    int maxline;                        /* Maximum request line size */
    int maxheaders;                     /* Maximum number of request headers */
    Tcl_WideInt readahead;              /* Maximum request size in memory */
    int queuesize;                      /* Current number of sockets in the queue */
    int maxqueuesize;                   /* Maximum number of sockets in the queue */
    int acceptsize;                     /* Number requests to accept at once */
    int driverthreads;                  /* Number of identical driver threads to be created */
    unsigned int loggingFlags;          /* Logging control flags */

    unsigned int flags;                 /* Driver state flags. */
    Ns_Thread thread;                   /* Thread id to join on shutdown. */
    Ns_Cond cond;                       /* Cond to signal reader threads,
                                         * driver query, startup, and shutdown. */
    NS_SOCKET trigger[2];               /* Wakeup trigger pipe. */

    struct Sock *sockPtr;               /* Free list of Sock structures */
    struct Sock *closePtr;              /* First conn ready for graceful close */

    DrvSpooler spooler;                 /* Tracks upload spooler threads */
    DrvWriter  writer;                  /* Tracks writer threads */
    Ns_Time    recvTimeout;             /* recvwait in form of Ns_Time to avoid frequent mappings */

    struct {
        Tcl_WideInt spooled;            /* Spooled incoming requests .. */
        Tcl_WideInt partial;            /* Partial operations */
        Tcl_WideInt received;           /* Received requests */
        Tcl_WideInt errors;             /* Dropped requests due to errors */
    } stats;
    unsigned short port;                /* Port in location */
    unsigned short defport;             /* Default port */
    bool reuseport;                     /* Allow optionally multiple drivers to connect to the same port */

} Driver;

/*
 * The following structure maintains a socket to a
 * connected client.  The socket is used to maintain state
 * during request read-ahead before connection processing
 * and keepalive after connection processing.
 */

typedef struct Sock {

    /*
     * Visible in Ns_Sock.
     */
    NS_SOCKET                  sock;
    struct Driver             *drvPtr;
    void                      *arg;        /* Driver context. */
    struct NS_SOCKADDR_STORAGE sa;         /* Actual peer address */

    /*
     * Private to Sock.
     */

    struct Sock        *nextPtr;
    struct NsServer    *servPtr;
    struct ConnPool    *poolPtr;

    const char         *location;
    NS_POLL_NFDS_TYPE   pidx;            /* poll() index */
    unsigned int        flags;           /* State flags used by driver */
    Ns_Time             timeout;
    Request            *reqPtr;

    Ns_Time             acceptTime;

    char               *taddr;           /* mmap-ed temporary file */
    size_t              tsize;           /* Size of mmap region */
    char               *tfile;           /* Name of regular temporary file */
    Ns_SockState        recvSockState;   /* Results from the last recv operation */
    int                 tfd;             /* File descriptor with request contents */
    bool                keep;

    void               *sls[1];          /* Slots for sls storage */

} Sock;

/*
 * The following structure maintains data from an
 * updated form file.
 */

typedef struct FormFile {
    Tcl_Obj *hdrObj;
    Tcl_Obj *offObj;
    Tcl_Obj *sizeObj;
} FormFile;

/*
 * The following structure defines per-request limits.
 */

typedef struct NsLimits {
    const char      *name;
    unsigned int     maxrun;    /* Max conns to run at once */
    unsigned int     maxwait;   /* Max conns waiting to run before being dropped */
    size_t         maxupload; /* Max data accepted */
    long             timeout;   /* Seconds allowed for conn to complete */

    Ns_Mutex         lock;      /* Lock for state and stats */

    struct {
        unsigned int nrunning;  /* Conns currently running */
        unsigned int nwaiting;  /* Conns waiting to run */
    } state;

    struct {
        unsigned int ndropped;  /* Drops due to .. */
        unsigned int noverflow; /* Max upload exceeded */
        unsigned int ntimeout;  /* Timeout exceeded */
    } stats;

} NsLimits;

/*
 * The following structure maintains state for a connection
 * being processed.
 */

typedef struct Conn {

    /*
     * Visible in an Ns_Conn:
     */

    Ns_Request request;

    Ns_Set *headers;
    Ns_Set *outputheaders;
    Ns_Set *auth;

    size_t contentLength;
    unsigned int flags;

    /*
     * Visible only in a Conn:
     */

    struct Conn *prevPtr;
    struct Conn *nextPtr;
    struct Sock *sockPtr;

    NsLimits *limitsPtr; /* Per-connection limits */
    Ns_Time   timeout;   /* Absolute timeout (startTime + limit) */

    /*
     * The following are copied from sockPtr so they're valid
     * after the connection is closed (e.g., within traces).
     */

    const char *server;
    const char *location;
    char *clientData;

    struct Request  *reqPtr;
    struct ConnPool *poolPtr;
    struct Driver   *drvPtr;

    uintptr_t id;
    char idstr[TCL_INTEGER_SPACE + 4];

    Ns_Time acceptTime;          /* timestamp, when the request was accepted */
    Ns_Time requestQueueTime;    /* timestamp, when the request was queued */
    Ns_Time requestDequeueTime;  /* timestamp, when the request was dequeued */
    Ns_Time filterDoneTime;      /* timestamp, after filters */
    Ns_Time runDoneTime;         /* timestamp, after running main connection task */

    Ns_Time acceptTimeSpan;
    Ns_Time queueTimeSpan;
    Ns_Time filterTimeSpan;
    Ns_Time runTimeSpan;

    struct NsInterp *itPtr;
    struct stat fileInfo;
    struct FileMap fmap;

    Tcl_Encoding outputEncoding;
    Tcl_Encoding urlEncoding;

    size_t nContentSent;
    ssize_t responseLength;  /* -1 for: not specified */
    int responseStatus;
    int recursionCount;
    int keep;                /* bool or -1 if undefined */

    int fd;
    NsWriterSock *strWriter;
    int rateLimit;          /* -1 undefined, 0 unlimited, otherwise KB/s */

    Ns_CompressStream cStream;
    int requestCompress;
    int compress;

    Ns_Set *query;
    Tcl_HashTable files;
    void *cls[NS_CONN_MAXCLS];

} Conn;


/*
 * The following structure is allocated for each connection thread.
 * The connPtr member is used for connecting threads with the request
 * info. The states if a conn thread are defined via enumeration.
 */
typedef enum {
    connThread_free,
    connThread_initial,
    connThread_warmup,
    connThread_ready,
    connThread_idle,
    connThread_busy,
    connThread_dead
} ConnThreadState;

typedef struct ConnThreadArg {
    struct ConnPool      *poolPtr;
    struct Conn          *connPtr;
    Ns_Cond               cond;        /* Cond for signaling this conn thread */
    Ns_Mutex              lock;
    struct ConnThreadArg *nextPtr;     /* used for the conn thread queue */
    ConnThreadState       state;
} ConnThreadArg;

/*
 * The following structure maintains a connection thread pool.
 */

typedef struct ConnPool {
    const char *pool;
    struct ConnPool *nextPtr;
    struct NsServer *servPtr;

    /*
     * The following struct maintains the active and waiting connection
     * queues, the free conn list, the next conn id, and the number
     * of waiting connects.
     */

    struct {
        Conn *freePtr;
        int maxconns;

        struct {
            Conn *firstPtr;
            Conn *lastPtr;
            int   num;
        } wait;

        Ns_Cond  cond;
        Ns_Mutex lock;
        int      lowwatermark;
        int      highwatermark;
        Ns_Time  retryafter;
        bool     rejectoverrun;
    } wqueue;

    /*
     * The following struct maintains the state of the threads.  Min and max
     * threads are determined at startup and then NsQueueConn ensures the
     * current number of threads remains within that range with individual
     * threads waiting no more than the timeout for a connection to
     * arrive.  The number of idle threads is maintained for the benefit of
     * the ns_server command.
     */

    struct {
        Ns_Mutex  lock;
        Ns_Time   timeout;
        uintptr_t nextid;
        int       min;
        int       max;
        int       current;
        int       idle;
        int       connsperthread;
        int       creating;
    } threads;

    /*
     * The following struct maintains the state of the thread
     * connection queue.  "nextPtr" points to the next available
     * connection thread, "args" keeps the array of all configured
     * connection structs, and "lock" is used for locking this queue.
     */

    struct {
        ConnThreadArg *nextPtr;
        ConnThreadArg *args;
        Ns_Mutex       lock;
    } tqueue;

    /*
     * Track "statistics" such as counts or aggregated times.
     */

    struct {
        unsigned long processed;
        unsigned long spool;
        unsigned long queued;
        unsigned long dropped;
        unsigned long connthreads;
        Ns_Time acceptTime;          /* cumulated accept times */
        Ns_Time queueTime;           /* cumulated queue times */
        Ns_Time filterTime;          /* cumulated file times */
        Ns_Time runTime;             /* cumulated run times */
        Ns_Time traceTime;           /* cumulated trace times */
    } stats;

    struct {
        int defaultConnectionLimit;  /* default rate limit for single connections */
        int poolLimit;               /* rate limit for pool */
        int currentRate;             /* actual rate tor pool */
        Tcl_WideInt bytesSent;
        Ns_Mutex lock;
        Ns_DList writerRates;
    } rate;

} ConnPool;

/*
 * The following structure is allocated for each virtual server.
 */

typedef struct NsServer {

    const char *server;

    /*
     * The following struct maintains the connection pool(s).
     */

    struct {
        Ns_Mutex lock;
        uintptr_t nextconnid;
        ConnPool *firstPtr;
        ConnPool *defaultPtr;
        Ns_Thread joinThread;
        bool shutdown;
    } pools;

    /*
     * The following struct maintains various server options.
     */

    struct {
        const char *realm;
        const Ns_Set *extraHeaders;
        int  errorminsize;
        Ns_HeaderCaseDisposition hdrcase;
        bool flushcontent;
        bool modsince;
        bool noticedetail;
    } opts;

    /*
     * Encoding defaults for the server
     */

    struct {

        const char    *urlCharset;
        Tcl_Encoding   urlEncoding;

        const char    *outputCharset;
        Tcl_Encoding   outputEncoding;

        bool           hackContentTypeP;

    } encoding;

    struct {
        const char *serverdir;  /* Virtual server files path */
        const char *pagedir;    /* Path to public pages */
        const char *pageroot;   /* Absolute path to public pages */
        const char **dirv;
        const char *dirproc;
        const char *diradp;
        Ns_UrlToFileProc *url2file;
        int dirc;
    } fastpath;

    /*
     * The following struct maintains virtual host config.
     */

    struct {
        unsigned int         opts; /* NSD_STRIP_WWW | NSD_STRIP_PORT */
        int                  hosthashlevel;
        const char          *hostprefix;
        Ns_ServerRootProc   *serverRootProc;
        void                *serverRootArg;
        Ns_ConnLocationProc *connLocationProc;
        Ns_TclCallback      *connLocationArg;
        Ns_LocationProc     *locationProc; /* Depreciated */
        bool                 enabled;
    } vhost;

    /*
     * The following struct maintains request tables.
     */

    struct {
        Ns_RequestAuthorizeProc *authProc;
        Tcl_HashTable redirect;
        Tcl_HashTable proxy;
        Ns_Mutex plock;
    } request;

    /*
     * The following struct maintains filters and traces.
     */

    struct {
        struct Filter *firstFilterPtr;
        struct Trace *firstTracePtr;
        struct Trace *firstCleanupPtr;
        Ns_Mutex lock;
    } filter;

    /*
     * The following array maintains url-specific data.
     */

    struct {
        struct Junction *junction[MAX_URLSPACES];
        Ns_Mutex lock;
        Ns_RWLock idlocks[MAX_URLSPACES];
    } urlspace;

    /*
     * The following struct maintains the core Tcl config.
     */

    struct {
        const char       *library;
        struct TclTrace  *firstTracePtr;
        struct TclTrace  *lastTracePtr;
        Tcl_Obj          *initfile;
        Ns_RWLock         lock;
        const char       *script;
        int               length;
        int               epoch;
        Tcl_Obj          *modules;
        Tcl_HashTable     runTable;
        const char      **errorLogHeaders;
        Tcl_HashTable     caches;
        Ns_Mutex          cachelock;
        uintptr_t         transactionEpoch;

        /*
         * The following tracks synchronization
         * objects which are looked up by name.
         */

        struct {
            Ns_Mutex      lock;
            Tcl_HashTable mutexTable, csTable, semaTable, condTable, rwTable;
            unsigned int  mutexId, csId, semaId, condId, rwId;
        } synch;

    } tcl;

    /*
     * The following struct maintains ADP config,
     * registered tags, and read-only page text.
     */
    /*
     * The following struct maintains ADP config,
     * registered tags, and read-only page text.
     */

    struct {
        unsigned int flags;
        int tracesize;
        size_t bufsize;
        size_t cachesize;

        const char *errorpage;
        const char *startpage;
        const char *debuginit;
        const char *defaultExtension;

        Ns_Cond pagecond;
        Ns_Mutex pagelock;
        Tcl_HashTable pages;
        Ns_RWLock taglock;
        Tcl_HashTable tags;

    } adp;

    struct {
        int  level;     /* 1-9 */
        int  minsize;   /* min size of response to compress, in bytes */
        bool enable;    /* on/off */
        bool preinit;   /* initialize the compression stream buffers in advance */
    } compress;

    /*
     * The following struct maintains the arrays
     * for the nsv commands.
     */

    struct {
        struct Bucket *buckets;
        int nbuckets;
    } nsv;

    /*
     * The following struct maintains detached Tcl
     * channels for the benefit of the ns_chan command.
     */

    struct {
        Ns_Mutex lock;
        Tcl_HashTable table;
    } chans;

    /*
     * The following struct maintains detached Tcl
     * channels for the benefit of the ns_connchan command.
     */

    struct {
        Ns_RWLock lock;
        Tcl_HashTable table;
    } connchans;

} NsServer;

/*
 * The following structure is allocated for each interp.
 */

typedef struct NsInterp {

    Tcl_Interp *interp;
    NsServer   *servPtr;
    int         epoch;         /* Run the update script if != to server epoch */
    int         refcnt;        /* Counts recursive allocations of cached interp */

    /*
     * The following pointer maintains the first in
     * a FIFO list of callbacks to invoke at interp
     * de-allocate time.
     */

    struct Defer *firstDeferPtr;

    /*
     * The following pointer maintains the first in
     * a LIFO list of scripts to evaluate when a
     * connection closes.
     */

    struct AtClose *firstAtClosePtr;

    /*
     * The following pointer and struct maintain state for
     * the active connection, if any, and support the ns_conn
     * command.
     */

    Ns_Conn *conn;

    struct {
        unsigned int  flags;
        char auth[NS_SET_SIZE];
        char form[NS_SET_SIZE];
        char hdrs[NS_SET_SIZE];
        char outhdrs[NS_SET_SIZE];
    } nsconn;

    /*
     * The following struct maintains per-interp ADP
     * context including the private pages cache.
     */

    struct adp {
        size_t            bufsize;
        unsigned int      flags;
        AdpResult         exception;
        int               refresh;
        int               errorLevel;
        int               debugLevel;
        int               debugInit;
        const char       *debugFile;
        Ns_Cache         *cache;
        const char       *cwd;
        struct AdpFrame  *framePtr;
        Ns_Conn          *conn;
        Tcl_Channel       chan;
        Tcl_DString       output;
        int               depth;
    } adp;

    /*
     * The following table maintains private Ns_Set's
     * entered into this interp.
     */

    Tcl_HashTable sets;

    /*
     * The following table maintains shared channels
     * register with the ns_chan command.
     */

    Tcl_HashTable chans;

    /*
     * The following table maintains the Tcl HTTP requests.
     */
    Tcl_HashTable httpRequests;

    Ns_CacheTransactionStack cacheTransactionStack;

    Ns_TclTraceType currentTrace;
    bool deleteInterp;  /* Interp should be deleted on next deallocation */

} NsInterp;


/*
 * Structures handling HTTP tasks
 */

struct _NsHttpChunk;

typedef struct {
    Ns_Task           *task;             /* Task handle */
    NS_SOCKET          sock;             /* socket to the remote peer */
    int                status;           /* HTTP response status */
    const char        *url;              /* request URL */
    const char        *error;            /* holds error string */
    char              *next;             /* write buffer */
    size_t             requestLength;    /* size of the complete request */
    size_t             replyLength;      /* content-length of the reply */
    size_t             requestHeaderSize;/* size of the request header */
    int                replyHeaderSize;  /* size of reply header */
    size_t             sent;             /* total amount of data sent */
    size_t             received;         /* total amount data received */
    size_t             sendBodySize;     /* amount of request body sent */
    size_t             replyBodySize;    /* amount of reply body received */
    size_t             replySize;        /* amount of reply body received */
    Ns_Set            *replyHeaders;     /* ns_set for response headers */
    Tcl_WideInt        spoolLimit;       /* spool content above this limit */
    int                spoolFd;          /* fd of spool file */
    char              *spoolFileName;    /* filename of the spool file */
    Tcl_Channel        spoolChan;        /* channel where to spool */
    Ns_Mutex           lock;             /* sync with task thread */
    unsigned int       flags;            /* */
    Ns_CompressStream *compress;         /* flag, toggle content decompression */
    Ns_Time           *timeout;          /* interval to wait for connect/data */
    Ns_Time            stime;            /* wall-clock task starting time */
    Ns_Time            etime;            /* wall-clock task ending time */
    bool               sendSpoolMode;    /* flag, spool from file/channel */
    bool               recvSpoolMode;    /* flag, spool to file/channel */
    int                bodyFileFd;       /* fd of the file to read the body */
    Tcl_Channel        bodyChan;         /* channel to read the body */
    size_t             bodySize;         /* size of the body to read */
    Ns_SockState       finalSockState;   /* state of the socket at completion */
    Tcl_Obj           *infoObj;          /* ancillary attr/value info */
    char              *doneCallback;     /* Tcl script run at task completion */
    NsServer          *servPtr;          /* Server for doneCallback */
    NS_TLS_SSL_CTX    *ctx;              /* SSL context handle */
    NS_TLS_SSL        *ssl;              /* SSL connection handle */
    Tcl_DString        ds;               /* for assembling request string */
    struct _NsHttpChunk *chunk;          /* for parsing chunked encodings */
} NsHttpTask;

/*
 * Callback for the Http chunked-encoding parse state machine
 */
typedef int (NsHttpParseProc)(NsHttpTask*, char**, size_t*);

typedef struct _NsHttpChunk {
    size_t             length;           /* Length of the chunk */
    size_t             got;              /* Received so many chunk bytes */
    Tcl_DString        ds;               /* For various parsing purposes */
    int                callx;            /* Next state-machine parser */
    NsHttpParseProc  **parsers;          /* Array of chunked encoding parsers */
} NsHttpChunk;

#define NS_HTTP_FLAG_DECOMPRESS    (1<<0)
#define NS_HTTP_FLAG_GZIP_ENCODING (1<<1)
#define NS_HTTP_FLAG_CHUNKED       (1<<2)
#define NS_HTTP_FLAG_CHUNKED_END   (1<<3)

#define NS_HTTP_FLAG_GUNZIP (NS_HTTP_FLAG_DECOMPRESS|NS_HTTP_FLAG_GZIP_ENCODING)


/*
 * Tcl object and string commands.
 */

NS_EXTERN Tcl_ObjCmdProc
    NsTclAbsoluteUrlObjCmd,
    NsTclAdpAbortObjCmd,
    NsTclAdpAppendObjCmd,
    NsTclAdpArgcObjCmd,
    NsTclAdpArgvObjCmd,
    NsTclAdpBindArgsObjCmd,
    NsTclAdpBreakObjCmd,
    NsTclAdpCloseObjCmd,
    NsTclAdpCtlObjCmd,
    NsTclAdpDebugObjCmd,
    NsTclAdpDirObjCmd,
    NsTclAdpDumpObjCmd,
    NsTclAdpEvalObjCmd,
    NsTclAdpExceptionObjCmd,
    NsTclAdpFlushObjCmd,
    NsTclAdpIdentObjCmd,
    NsTclAdpIncludeObjCmd,
    NsTclAdpInfoObjCmd,
    NsTclAdpMimeTypeObjCmd,
    NsTclAdpParseObjCmd,
    NsTclAdpPutsObjCmd,
    NsTclAdpRegisterAdpObjCmd,
    NsTclAdpRegisterAdptagObjCmd,
    NsTclAdpRegisterProcObjCmd,
    NsTclAdpRegisterScriptObjCmd,
    NsTclAdpRegisterTagObjCmd,
    NsTclAdpReturnObjCmd,
    NsTclAdpSafeEvalObjCmd,
    NsTclAdpStatsObjCmd,
    NsTclAdpTellObjCmd,
    NsTclAdpTruncObjCmd,
    NsTclAfterObjCmd,
    NsTclAsyncLogfileObjCmd,
    NsTclAtCloseObjCmd,
    NsTclAtExitObjCmd,
    NsTclAtPreStartupObjCmd,
    NsTclAtShutdownObjCmd,
    NsTclAtSignalObjCmd,
    NsTclAtStartupObjCmd,
    NsTclBase64DecodeObjCmd,
    NsTclBase64EncodeObjCmd,
    NsTclBase64UrlDecodeObjCmd,
    NsTclBase64UrlEncodeObjCmd,
    NsTclCacheAppendObjCmd,
    NsTclCacheConfigureObjCmd,
    NsTclCacheCreateObjCmd,
    NsTclCacheEvalObjCmd,
    NsTclCacheExistsObjCmd,
    NsTclCacheFlushObjCmd,
    NsTclCacheGetObjCmd,
    NsTclCacheIncrObjCmd,
    NsTclCacheKeysObjCmd,
    NsTclCacheLappendObjCmd,
    NsTclCacheNamesObjCmd,
    NsTclCacheStatsObjCmd,
    NsTclCacheTransactionBeginObjCmd,
    NsTclCacheTransactionCommitObjCmd,
    NsTclCacheTransactionRollbackObjCmd,
    NsTclCancelObjCmd,
    NsTclChanObjCmd,
    NsTclCharsetsObjCmd,
    NsTclCondObjCmd,
    NsTclConfigObjCmd,
    NsTclConfigSectionObjCmd,
    NsTclConfigSectionsObjCmd,
    NsTclConnChanObjCmd,
    NsTclConnObjCmd,
    NsTclConnSendFpObjCmd,
    NsTclCrashObjCmd,
    NsTclCritSecObjCmd,
    NsTclCryptObjCmd,
    NsTclCryptoAeadDecryptObjCmd,
    NsTclCryptoAeadEncryptObjCmd,
    NsTclCryptoEckeyObjCmd,
    NsTclCryptoHmacObjCmd,
    NsTclCryptoMdObjCmd,
    NsTclCryptoRandomBytesObjCmd,
    NsCryptoScryptObjCmd,
    NsTclDeleteCookieObjCmd,
    NsTclDriverObjCmd,
    NsTclEncodingForCharsetObjCmd,
    NsTclEnvObjCmd,
    NsTclFTruncateObjCmd,
    NsTclFastPathCacheStatsObjCmd,
    NsTclFileStatObjCmd,
    NsTclGetAddrObjCmd,
    NsTclGetCookieObjCmd,
    NsTclGetHostObjCmd,
    NsTclGetLimitsObjCmd,
    NsTclGetUrlObjCmd,
    NsTclGifSizeObjCmd,
    NsTclGmTimeObjCmd,
    NsTclGuessTypeObjCmd,
    NsTclHashObjCmd,
    NsTclHashPathObjCmd,
    NsTclHeadersObjCmd,
    NsTclHrefsObjCmd,
    NsTclHttpObjCmd,
    NsTclHttpTimeObjCmd,
    NsTclICtlObjCmd,
    NsTclImgMimeObjCmd,
    NsTclImgSizeObjCmd,
    NsTclImgTypeObjCmd,
    NsTclInfoObjCmd,
    NsTclInternalRedirectObjCmd,
    NsTclJobObjCmd,
    NsTclJpegSizeObjCmd,
    NsTclKillObjCmd,
    NsTclLibraryObjCmd,
    NsTclListLimitsObjCmd,
    NsTclLocalTimeObjCmd,
    NsTclLocationProcObjCmd,
    NsTclLogCtlObjCmd,
    NsTclLogObjCmd,
    NsTclLogRollObjCmd,
    NsTclMD5ObjCmd,
    NsTclMkTempObjCmd,
    NsTclModuleLoadObjCmd,
    NsTclModulePathObjCmd,
    NsTclMutexObjCmd,
    NsTclNormalizePathObjCmd,
    NsTclNsvAppendObjCmd,
    NsTclNsvArrayObjCmd,
    NsTclNsvBucketObjCmd,
    NsTclNsvDictObjCmd,
    NsTclNsvExistsObjCmd,
    NsTclNsvGetObjCmd,
    NsTclNsvIncrObjCmd,
    NsTclNsvLappendObjCmd,
    NsTclNsvNamesObjCmd,
    NsTclNsvSetObjCmd,
    NsTclNsvUnsetObjCmd,
    NsTclPagePathObjCmd,
    NsTclParseArgsObjCmd,
    NsTclParseFieldvalue,
    NsTclParseHeaderObjCmd,
    NsTclParseHttpTimeObjCmd,
    NsTclParseQueryObjCmd,
    NsTclParseUrlObjCmd,
    NsTclPauseObjCmd,
    NsTclPngSizeObjCmd,
    NsTclProgressObjCmd,
    NsTclPurgeFilesObjCmd,
    NsTclQuoteHtmlObjCmd,
    NsTclRWLockObjCmd,
    NsTclRandObjCmd,
    NsTclReflowTextObjCmd,
    NsTclRegisterAdpObjCmd,
    NsTclRegisterFastPathObjCmd,
    NsTclRegisterFastUrl2FileObjCmd,
    NsTclRegisterFilterObjCmd,
    NsTclRegisterLimitsObjCmd,
    NsTclRegisterProcObjCmd,
    NsTclRegisterProxyObjCmd,
    NsTclRegisterTclObjCmd,
    NsTclRegisterTraceObjCmd,
    NsTclRegisterUrl2FileObjCmd,
    NsTclRequestAuthorizeObjCmd,
    NsTclRespondObjCmd,
    NsTclResumeObjCmd,
    NsTclReturnBadRequestObjCmd,
    NsTclReturnErrorObjCmd,
    NsTclReturnFileObjCmd,
    NsTclReturnForbiddenObjCmd,
    NsTclReturnFpObjCmd,
    NsTclReturnMovedObjCmd,
    NsTclReturnNotFoundObjCmd,
    NsTclReturnNoticeObjCmd,
    NsTclReturnObjCmd,
    NsTclReturnRedirectObjCmd,
    NsTclReturnTooLargeObjCmd,
    NsTclReturnUnauthorizedObjCmd,
    NsTclReturnUnavailableObjCmd,
    NsTclRlimitObjCmd,
    NsTclRollFileObjCmd,
    NsTclRunOnceObjCmd,
    NsTclSHA1ObjCmd,
    NsTclSchedDailyObjCmd,
    NsTclSchedObjCmd,
    NsTclSchedWeeklyObjCmd,
    NsTclSelectObjCmd,
    NsTclSemaObjCmd,
    NsTclServerObjCmd,
    NsTclServerPathObjCmd,
    NsTclServerRootProcObjCmd,
    NsTclSetCookieObjCmd,
    NsTclSetGroupObjCmd,
    NsTclSetLimitsObjCmd,
    NsTclSetObjCmd,
    NsTclSetUserObjCmd,
    NsTclShortcutFilterObjCmd,
    NsTclShutdownObjCmd,
    NsTclSleepObjCmd,
    NsTclSlsObjCmd,
    NsTclSockAcceptObjCmd,
    NsTclSockCallbackObjCmd,
    NsTclSockCheckObjCmd,
    NsTclSockListenCallbackObjCmd,
    NsTclSockListenObjCmd,
    NsTclSockNReadObjCmd,
    NsTclSockOpenObjCmd,
    NsTclSockSetBlockingObjCmd,
    NsTclSockSetNonBlockingObjCmd,
    NsTclSocketPairObjCmd,
    NsTclStartContentObjCmd,
    NsTclStrftimeObjCmd,
    NsTclStripHtmlObjCmd,
    NsTclSymlinkObjCmd,
    NsTclThreadObjCmd,
    NsTclTimeObjCmd,
    NsTclTruncateObjCmd,
    NsTclUnRegisterOpObjCmd,
    NsTclUnRegisterUrl2FileObjCmd,
    NsTclUnscheduleObjCmd,
    NsTclUrl2FileObjCmd,
    NsTclUrlDecodeObjCmd,
    NsTclUrlEncodeObjCmd,
    NsTclUrlSpaceObjCmd,
    NsTclWriteContentObjCmd,
    NsTclWriteFpObjCmd,
    NsTclWriteObjCmd,
    NsTclWriterObjCmd,
    TclX_KeyldelObjCmd,
    TclX_KeylgetObjCmd,
    TclX_KeylkeysObjCmd,
    TclX_KeylsetObjCmd;

NS_EXTERN Ns_LogSeverity Ns_LogRequestDebug;
NS_EXTERN Ns_LogSeverity Ns_LogConnchanDebug;
NS_EXTERN Ns_LogSeverity Ns_LogUrlspaceDebug;
NS_EXTERN Ns_LogSeverity Ns_LogTimeoutDebug;
NS_EXTERN bool NsWriterBandwidthManagement;

NS_EXTERN const char *nsBuildDate;

/*
 * Libnsd initialization routines.
 */
NS_EXTERN void NsInitBinder(void);
NS_EXTERN void NsInitConf(void);
NS_EXTERN void NsInitDrivers(void);
NS_EXTERN void NsInitFd(void);
NS_EXTERN void NsInitInfo(void);
NS_EXTERN void NsInitLimits(void);
NS_EXTERN void NsInitListen(void);
NS_EXTERN void NsInitLog(void);
NS_EXTERN void NsInitModLoad(void);
NS_EXTERN void NsInitOpenSSL(void);
NS_EXTERN void NsInitProcInfo(void);
NS_EXTERN void NsInitQueue(void);
NS_EXTERN void NsInitRequests(void);
NS_EXTERN void NsInitSched(void);
NS_EXTERN void NsInitServers(void);
NS_EXTERN void NsInitSls(void);
NS_EXTERN void NsInitSockCallback(void);
NS_EXTERN void NsInitTask(void);
NS_EXTERN void NsInitTcl(void);
NS_EXTERN void NsInitTclEnv(void);
NS_EXTERN void NsInitUrl2File(void);

NS_EXTERN void NsConfigAdp(void);
NS_EXTERN void NsConfigLog(void);
NS_EXTERN void NsConfigFastpath(void);
NS_EXTERN void NsConfigMimeTypes(void);
NS_EXTERN void NsConfigDNS(void);
NS_EXTERN void NsConfigRedirects(void);
NS_EXTERN void NsConfigVhost(void);
NS_EXTERN void NsConfigEncodings(void);
NS_EXTERN void NsConfigTcl(void);

/*
 * Virtual server management routines.
 */

NS_EXTERN void NsInitServer(const char *server, Ns_ServerInitProc *initProc)
    NS_GNUC_NONNULL(1);
NS_EXTERN void NsRegisterServerInit(Ns_ServerInitProc *proc)
    NS_GNUC_NONNULL(1);
NS_EXTERN NsServer *NsGetInitServer(void) NS_GNUC_PURE;
NS_EXTERN NsServer *NsGetServer(const char *server);
NS_EXTERN void NsStartServers(void);
NS_EXTERN void NsStopServers(const Ns_Time *toPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsStartServer(const NsServer *servPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsStopServer(NsServer *servPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsWaitServer(NsServer *servPtr, const Ns_Time *toPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void NsWakeupDriver(const Driver *drvPtr) NS_GNUC_NONNULL(1);

/*
 * URL-specific data routines.
 */
NS_EXTERN void *
NsUrlSpecificGet(NsServer *servPtr, const char *method,
                 const char *url, int id, unsigned int flags, NsUrlSpaceOp op,
                 NsUrlSpaceContextFilterProc proc, void *context)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN NsUrlSpaceContextSpec *
NsUrlSpaceContextSpecNew(const char *field, const char *patternString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
NsUrlSpaceContextSpecAppend(Tcl_DString *dsPtr, NsUrlSpaceContextSpec *spec)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN NsUrlSpaceContextFilterProc NsUrlSpaceContextFilter;

/*
 * Socket driver callbacks.
 */

NS_EXTERN ssize_t NsDriverRecv(Sock *sockPtr, struct iovec *bufs, int nbufs, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN ssize_t NsDriverSend(Sock *sockPtr, const struct iovec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1);
NS_EXTERN ssize_t NsDriverSendFile(Sock *sockPtr, Ns_FileVec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN int NSDriverClientOpen(Tcl_Interp *interp, const char *driverName,
                                 const char *url, const char *httpMethod, const char *version,
                                 const Ns_Time *timeoutPtr, Sock **sockPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5)
    NS_GNUC_NONNULL(6) NS_GNUC_NONNULL(7);

NS_EXTERN int NSDriverSockNew(Tcl_Interp *interp, NS_SOCKET sock,
                              const char *protocol, const char *driverName, const char *methodName,
                              Sock **sockPtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

NS_EXTERN Ns_ReturnCode NsQueueConn(Sock *sockPtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void NsEnsureRunningConnectionThreads(const NsServer *servPtr, ConnPool *poolPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsMapPool(ConnPool *poolPtr, const char *mapString, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *NsPoolName(const char *poolName)
        NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN size_t NsPoolAllocateThreadSlot(ConnPool *poolPtr, uintptr_t threadID)
    NS_GNUC_NONNULL(1);

NS_EXTERN int NsPoolTotalRate(ConnPool *poolPtr, size_t slot, int rate, int *writerThreadCount)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);
NS_EXTERN void NsPoolAddBytesSent(ConnPool *poolPtr, Tcl_WideInt bytesSent)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsSockClose(Sock *sockPtr, int keep)
    NS_GNUC_NONNULL(1);

NS_EXTERN int NsPoll(struct pollfd *pfds, NS_POLL_NFDS_TYPE nfds, const Ns_Time *timeoutPtr);

NS_EXTERN Request *NsGetRequest(Sock *sockPtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsWriterLock(void);
NS_EXTERN void NsWriterUnlock(void);

NS_EXTERN void NsWriterFinish(NsWriterSock *wrSockPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode NsWriterQueue(
    Ns_Conn *conn,
    size_t nsend,
    Tcl_Channel chan,
    FILE *fp,
    int fd,
    struct iovec *bufs,
    int nbufs,
    const Ns_FileVec *filebufs,
    int nfilebufs,
    bool everysize
) NS_GNUC_NONNULL(1);

/*
 * External callback functions.
 */

NS_EXTERN Ns_ConnLocationProc NsTclConnLocation;
NS_EXTERN Ns_SchedProc NsTclSchedProc;
NS_EXTERN Ns_ServerRootProc NsTclServerRoot;
NS_EXTERN Ns_ThreadProc NsTclThread NS_GNUC_NORETURN;
NS_EXTERN Ns_ArgProc NsTclThreadArgProc;
NS_EXTERN Ns_SockProc NsTclSockProc;
NS_EXTERN Ns_ArgProc NsTclSockArgProc;
NS_EXTERN Ns_ThreadProc NsConnThread NS_GNUC_NORETURN;
NS_EXTERN Ns_ArgProc NsConnArgProc;
NS_EXTERN Ns_FilterProc NsTclFilterProc;
NS_EXTERN Ns_FilterProc NsShortcutFilterProc NS_GNUC_PURE;
NS_EXTERN Ns_OpProc NsTclRequestProc;
NS_EXTERN Ns_OpProc NsAdpPageProc;
NS_EXTERN Ns_ArgProc NsAdpPageArgProc;
NS_EXTERN Ns_TclTraceProc NsTclTraceProc;
NS_EXTERN Ns_UrlToFileProc NsUrlToFileProc;
NS_EXTERN Ns_Url2FileProc NsTclUrl2FileProc;
NS_EXTERN Ns_Url2FileProc NsMountUrl2FileProc;
NS_EXTERN Ns_ArgProc NsMountUrl2FileArgProc;

NS_EXTERN void NsGetCallbacks(Tcl_DString *dsPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsGetSockCallbacks(Tcl_DString *dsPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsGetScheduled(Tcl_DString *dsPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsGetMimeTypes(Tcl_DString *dsPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsGetTraces(Tcl_DString *dsPtr, const char *server) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void NsGetFilters(Tcl_DString *dsPtr, const char *server) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void NsGetRequestProcs(Tcl_DString *dsPtr, const char *server) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void NsGetUrl2FileProcs(Ns_DString *dsPtr, const char *server) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#ifdef _WIN32
NS_EXTERN Ns_ReturnCode NsConnectService(void);
NS_EXTERN Ns_ReturnCode NsInstallService(char *service) NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode NsRemoveService(char *service) NS_GNUC_NONNULL(1);
#endif

NS_EXTERN void NsCreatePidFile(void);
NS_EXTERN void NsRemovePidFile(void);

NS_EXTERN void NsLogOpen(void);
NS_EXTERN void NsTclInitObjs(void);
NS_EXTERN void NsBlockSignals(int debug);
NS_EXTERN void NsBlockSignal(int signal);
NS_EXTERN void NsUnblockSignal(int signal);
NS_EXTERN int  NsHandleSignals(void);
NS_EXTERN void NsStopDrivers(void);
NS_EXTERN void NsStopSpoolers(void);
NS_EXTERN Ns_ReturnCode NsPreBind(const char *args, const char *file);
NS_EXTERN void NsClosePreBound(void);
NS_EXTERN const char *NsConfigRead(const char *file) NS_GNUC_NONNULL(1);
NS_EXTERN void NsConfigEval(const char *config, int argc, char *const *argv, int optionIndex) NS_GNUC_NONNULL(1);
NS_EXTERN void NsConfUpdate(void);
NS_EXTERN void NsEnableDNSCache(int maxsize, int ttl, int timeout);
NS_EXTERN void NsStartDrivers(void);
NS_EXTERN void NsWaitDriversShutdown(const Ns_Time *toPtr);
NS_EXTERN void NsStartSchedShutdown(void);
NS_EXTERN void NsWaitSchedShutdown(const Ns_Time *toPtr);
NS_EXTERN void NsStartSockShutdown(void);
NS_EXTERN void NsWaitSockShutdown(const Ns_Time *toPtr);
NS_EXTERN void NsStartShutdownProcs(void);
NS_EXTERN void NsWaitShutdownProcs(const Ns_Time *toPtr);
NS_EXTERN void NsStartTaskQueueShutdown(void);
NS_EXTERN void NsWaitTaskQueueShutdown(const Ns_Time *toPtr);
NS_EXTERN void NsStartJobsShutdown(void);
NS_EXTERN void NsWaitJobsShutdown(const Ns_Time *toPtr);

NS_EXTERN Tcl_AppInitProc NsTclAppInit;
NS_EXTERN void NsTclInitServer(const char *server)       NS_GNUC_NONNULL(1);
NS_EXTERN void NsInitStaticModules(const char *server);

NS_EXTERN Tcl_Interp *NsTclCreateInterp(void)            NS_GNUC_RETURNS_NONNULL;
NS_EXTERN Tcl_Interp *NsTclAllocateInterp(NsServer *servPtr) NS_GNUC_RETURNS_NONNULL;
NS_EXTERN NsInterp *NsGetInterpData(Tcl_Interp *interp)  NS_GNUC_NONNULL(1);
NS_EXTERN void NsFreeConnInterp(Conn *connPtr)           NS_GNUC_NONNULL(1);

NS_EXTERN void NsIdleCallback(NsServer *servPtr)        NS_GNUC_NONNULL(1);


NS_EXTERN struct Bucket *NsTclCreateBuckets(const char *server, int nbuckets) NS_GNUC_NONNULL(1);

NS_EXTERN void NsSlsCleanup(Sock *sockPtr)               NS_GNUC_NONNULL(1);
NS_EXTERN void NsClsCleanup(Conn *connPtr)               NS_GNUC_NONNULL(1);
NS_EXTERN void NsTclAddBasicCmds(NsInterp *itPtr)        NS_GNUC_NONNULL(1);
NS_EXTERN void NsTclAddServerCmds(NsInterp *itPtr)       NS_GNUC_NONNULL(1);

NS_EXTERN void NsRestoreSignals(void);
NS_EXTERN void NsSendSignal(int sig);

NS_EXTERN Tcl_Obj * NsDriverStats(Tcl_Interp *interp) NS_GNUC_NONNULL(1);
NS_EXTERN void NsDriverMapVirtualServers(void);
NS_EXTERN NS_TLS_SSL_CTX *NsDriverLookupHostCtx(char *host, const Ns_Driver *drvPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * limits.c
 */
NS_EXTERN NsLimits *NsGetRequestLimits(NsServer *servPtr, const char *method, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * url2file.c
 */
NS_EXTERN Ns_ReturnCode NsUrlToFile(Ns_DString *dsPtr, NsServer *servPtr, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * pathname.c
 */
NS_EXTERN char *NsPageRoot(Ns_DString *dsPtr, const NsServer *servPtr, const char *host)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * range.c
 */
NS_EXTERN int NsConnParseRange(Ns_Conn *conn, const char *type,
                               int fd, const void *data, size_t objLength,
                               Ns_FileVec *bufs, int *nbufsPtr, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_NONNULL(7) NS_GNUC_NONNULL(8);

/*
 * conn.c
 */
NS_EXTERN const char * NsConnIdStr(const Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN void NsConnTimeStatsUpdate(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsConnTimeStatsFinalize(const Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode NsConnRequire(Tcl_Interp *interp, unsigned int flags, Ns_Conn **connPtr)
    NS_GNUC_NONNULL(1);

/*
 * request parsing
 */
NS_EXTERN void NsParseAcceptEncoding(double version, const char *hdr, bool *gzipAcceptPtr, bool *brotliAcceptPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

/*
 * encoding.c
 */

NS_EXTERN const char *NsFindCharset(const char *mimetype, size_t *lenPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool NsEncodingIsUtf8(const Tcl_Encoding encoding)
    NS_GNUC_PURE;


/*
 * ADP routines.
 */

NS_EXTERN int NsAdpAppend(NsInterp *itPtr, const char *buf, int len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int NsAdpFlush(NsInterp *itPtr, bool doStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpDebug(NsInterp *itPtr, const char *host, const char *port, const char *procs)
    NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpEval(NsInterp *itPtr, int objc, Tcl_Obj *const* objv, const char *resvar)
    NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpSource(NsInterp *itPtr, int objc, Tcl_Obj *const* objv, const char *resvar)
    NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpInclude(NsInterp *itPtr, int objc, Tcl_Obj *const* objv,
                           const char *file, const Ns_Time *expiresPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN void NsAdpParse(AdpCode *codePtr, NsServer *servPtr, char *adp,
                          unsigned int flags, const char* file)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void NsAdpFreeCode(AdpCode *codePtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsAdpLogError(NsInterp *itPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsAdpInit(NsInterp *itPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsAdpReset(NsInterp *itPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsAdpFree(NsInterp *itPtr)
    NS_GNUC_NONNULL(1);

/*
 * Tcl support routines.
 */

NS_EXTERN void NsTclInitQueueType(void);
NS_EXTERN void NsTclInitAddrType(void);
NS_EXTERN void NsTclInitTimeType(void);
NS_EXTERN void NsTclInitKeylistType(void);
NS_EXTERN void NsTclInitSpecType(void);
NS_EXTERN void NsTclInitMemUnitType(void);

/*
 * Callback routines.
 */

NS_EXTERN Ns_ReturnCode NsRunFilters(Ns_Conn *conn, Ns_FilterType why) NS_GNUC_NONNULL(1);
NS_EXTERN void NsRunCleanups(Ns_Conn *conn)                   NS_GNUC_NONNULL(1);
NS_EXTERN void NsRunTraces(Ns_Conn *conn)                     NS_GNUC_NONNULL(1);
NS_EXTERN void NsRunPreStartupProcs(void);
NS_EXTERN void NsRunSignalProcs(void);
NS_EXTERN void NsRunStartupProcs(void);
NS_EXTERN void NsRunAtReadyProcs(void);
NS_EXTERN void NsRunAtExitProcs(void);
NS_EXTERN void NsTclRunAtClose(NsInterp *itPtr)              NS_GNUC_NONNULL(1);

/*
 * Upload progress routines.
 */

NS_EXTERN void NsConfigProgress(void);
NS_EXTERN void NsUpdateProgress(Ns_Sock *sock) NS_GNUC_NONNULL(1);

/*
 * dlist.c
 */

NS_EXTERN void Ns_DListInit(Ns_DList *dlPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void Ns_DListAppend(Ns_DList *dlPtr, void *element)
    NS_GNUC_NONNULL(1);

NS_EXTERN void Ns_DListFree(Ns_DList *dlPtr)
    NS_GNUC_NONNULL(1);

/*
 * watchdog.c
 */

NS_EXTERN int NsForkWatchedProcess(void);

/*
 * Utility functions.
 */

NS_EXTERN Ns_ReturnCode NsMemMap(const char *path, size_t size, int mode, FileMap *mapPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN void NsMemUmap(const FileMap *mapPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void NsParseAuth(Conn *connPtr, char *auth)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool NsTclObjIsByteArray(const Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN bool NsTclObjIsEncodedByteArray(const Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN bool NsTclTimeoutException(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1);


/*
 * (HTTP) Proxy support
 */

NS_EXTERN Ns_ReturnCode NsConnRunProxyRequest(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

#endif /* NSD_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
