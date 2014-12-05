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

#include "ns.h"

/*
 * Constants
 */

#define NS_CONFIG_PARAMETERS           "ns/parameters"
#define NS_CONFIG_THREADS              "ns/threads"

/*
 * Various ADP option bits.
 */

#define ADP_SAFE                       0x01U    /* Use Tcl_SafeEval for ADP */
#define ADP_SINGLE                     0x02U    /* Combine blocks into a single script */
#define ADP_DEBUG                      0x04U    /* Enable debugging */
#define ADP_EXPIRE                     0x08U    /* Send Expires: now header on output */
#define ADP_CACHE                      0x10U    /* Enable output caching */
#define ADP_TRACE                      0x20U    /* Trace execution */
#define ADP_DETAIL                     0x80U    /* Log connection details on error */
#define ADP_STRICT                     0x100U   /* Strict error handling */
#define ADP_DISPLAY                    0x200U   /* Display error messages in output stream */
#define ADP_TRIM                       0x400U   /* Display error messages in output stream */
#define ADP_FLUSHED                    0x800U   /* Some output has been sent */
#define ADP_ERRLOGGED                  0x1000U  /* Error message has already been logged */
#define ADP_AUTOABORT                  0x2000U  /* Raise abort on flush error */
#define ADP_ADPFILE                    0x4000U  /* Object to evaluate is a file */
#define ADP_STREAM                     0x8000U  /* Enable ADP streaming */
#define ADP_TCLFILE                    0x10000U /* Object to evaluate is a Tcl file */
#define ADP_OPTIONMAX                  0x1000000U /* watermark for flag values */

typedef enum {
  ADP_OK =                     0,
  ADP_BREAK =                  1,
  ADP_ABORT =                  2,
  ADP_RETURN =                 3,
  ADP_TIMEOUT =                4
} AdpResult;

#define MAX_URLSPACES                  16
#define NS_SET_SIZE                    ((unsigned)TCL_INTEGER_SPACE + 2U)

#define CONN_TCLFORM                   0x01U  /* Query form set is registered for interp */
#define CONN_TCLHDRS                   0x02U  /* Input headers set is registered for interp */
#define CONN_TCLOUTHDRS                0x04U  /* Output headers set is registered for interp */
#define CONN_TCLAUTH                   0x08U  /* 'auth' headers set is registered for interp */
#define CONN_TCLHTTP                   0x10U  /* HTTP headers requested by ns_headers */

/*
 * The following is the default text/html content type
 * sent to the browser for html/adp etc. requests.
 */

#define NSD_TEXTHTML                   "text/html"
/*
 * constants for SockState return and reason codes.
 */

typedef enum {
    SOCK_READY =               0,
    SOCK_MORE =                1,
    SOCK_SPOOL =               2,
    SOCK_ERROR =              -1,
    SOCK_CLOSE =              -2,
    SOCK_CLOSETIMEOUT =       -3,
    SOCK_READTIMEOUT =        -4,
    SOCK_WRITETIMEOUT =       -5,
    SOCK_SERVERREJECT =       -6,
    SOCK_READERROR =          -7,
    SOCK_WRITEERROR =         -8,
    SOCK_SHUTERROR =          -9,
    SOCK_BADREQUEST =         -11,
    SOCK_ENTITYTOOLARGE =     -12,
    SOCK_BADHEADER =          -13,
    SOCK_TOOMANYHEADERS =     -14
} SockState;

/*
 * subset for spooler states
 */
typedef enum {
    SPOOLER_CLOSE =             SOCK_CLOSE,
    SPOOLER_OK =                SOCK_READY,
    SPOOLER_READERROR =         SOCK_READERROR,
    SPOOLER_WRITEERROR =        SOCK_WRITEERROR,
    SPOOLER_CLOSETIMEOUT =      SOCK_CLOSETIMEOUT
} SpoolerState;

typedef struct {
    SpoolerState spoolerState;
    SockState    sockState;
} SpoolerStateMap;

/*
 * Types definitions.
 */

struct Sock;
struct NsServer;

struct nsconf {
    char *argv0;
    char *nsd;
    char *name;
    char *version;
    const char *home;
    const char *tmpDir;
    const char *config;
    char *build;
    pid_t pid;
    time_t boot_t;
    char hostname[255];
    char address[16];
    long shutdowntimeout;  /* same type as seconds in Ns_Time */
    int backlog;

    /*
     * Slot IDs for socket local storage.
     */

    uintptr_t nextSlsId;

    /*
     * The following table holds the configured virtual servers.
     * The dstring maintains a Tcl list of the names.
     */

    Tcl_HashTable servertable;
    Tcl_DString servers;
    char *defaultServer;

    /*
     * The following table holds config section sets from
     * the config file.
     */

    Tcl_HashTable sections;

    /*
     * The following struct maintains server state.
     */

    struct {
        Ns_Mutex lock;
        Ns_Cond cond;
        int started;
        int stopping;
    } state;

    struct {
        int jobsperthread;
        int maxelapsed;
    } sched;

#ifdef _WIN32
    struct {
        bool checkexit;
    } exec;
#endif

    struct {
        const char *sharedlibrary;
        char *version;
        bool lockoninit;
    } tcl;

    struct {
        int jobsperthread;
        int timeout;
    } job;
};

NS_EXTERN struct nsconf nsconf;

/*
 * The following structure tracks a memory-mapped file
 * in a platform-neutral way.
 */

typedef struct FileMap {
    char *addr;                 /* Mapped to this virtual address */
    size_t size;                /* Size of the mapped region */
    int handle;                 /* OS handle of the opened/mapped file */
    void *mapobj;               /* Mapping object (Win32 only) */
} FileMap;

/*
 * The following structure maintains writer socket
 */
typedef struct WriterSock {
    struct WriterSock   *nextPtr;
    struct Sock         *sockPtr;
    struct SpoolerQueue *queuePtr; 
    struct Conn         *connPtr;
    SpoolerState         status;
    int                  err;
    int                  refCount;
    int                  keep;
    Tcl_WideInt          nsent;
    size_t               size;
    unsigned int         flags;
    int                  doStream;
    int                  fd;
    char                 *headerString;
    
    union {
	struct {
	    struct iovec      *bufs;                 /* incoming bufs to be sent */
	    int                nbufs;
	    int                bufIdx; 
	    struct iovec       sbufs[UIO_SMALLIOV];  /* scratch bufs for handling partial sends */
	    int                nsbufs;
	    int                sbufIdx;
	    struct iovec       preallocated_bufs[UIO_SMALLIOV];
	    struct FileMap     fmap;
	} mem;

	struct {
	    size_t             maxsize;
	    size_t             bufsize;
	    off_t              bufoffset;
	    size_t             toRead;
	    unsigned char     *buf;
	    Ns_Mutex           fdlock;
	} file;
    } c;

    char              *clientData;
    Ns_Time            startTime;

} WriterSock;

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
    bool                 stopped;     /* Flag to indicate thread stopped */
    bool                 shutdown;    /* Flag to indicate shutdown */
    int                  id;          /* Queue id */
    int                  queuesize;   /* Number of active sockets in the queue */
    const char          *threadname;  /* name of the thread working on this queue */
} SpoolerQueue;


/*
 * The following structure maintains an ADP call frame.
 */

typedef struct AdpFrame {
    struct AdpFrame   *prevPtr;
    unsigned short     line;
    unsigned short     objc;
    time_t    	       mtime;
    off_t     	       size;
    Tcl_Obj	      *ident;
    Tcl_Obj          **objv;
    char	      *savecwd;
    const char	      *file;
    unsigned int       flags;
    Ns_DString         cwdbuf;
    Tcl_DString	      *outputPtr;
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
    int		nblocks;
    int		nscripts;
    int	       *len;
    int	       *line;
    Tcl_DString text;
} AdpCode;

#define AdpCodeLen(cp,i)	((cp)->len[(i)])
#define AdpCodeLine(cp,i)	((cp)->line[(i)])
#define AdpCodeText(cp)		((cp)->text.string)
#define AdpCodeBlocks(cp)	((cp)->nblocks)
#define AdpCodeScripts(cp)	((cp)->nscripts)


/*
 * The following structure defines the entire request
 * including HTTP request line, headers, and content.
 */

typedef struct Request {
    struct Request *nextPtr;    /* Next on free list */
    Ns_Request request;         /* Parsed request line */
    Ns_Set *headers;            /* Input headers */
    Ns_Set *auth;               /* Auth user/password and parameters */
    char peer[16];              /* Client peer address */
    int port;                   /* Client peer port */

    /*
     * The following pointers are used to access the
     * buffer contents after the read-ahead is complete.
     */

    char *next;                 /* Next read offset */
    char *content;              /* Start of content */
    size_t length;              /* Length of content */
    size_t contentLength;       /* Provided content length */
    size_t avail;               /* Bytes avail in buffer */
    int leadblanks;             /* Number of leading blank lines read */

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
    Tcl_DString buffer;           /* Request and content buffer */

} Request;

/*
 * The following structure maitains data for each instance of
 * a driver initialized with Ns_DriverInit.
 */

typedef struct {
    int threads;                 /* Number of spooler threads to run */
    Ns_Mutex lock;               /* Lock around spooler queue */
    SpoolerQueue *firstPtr;      /* Spooler thread queue */
    SpoolerQueue *curPtr;        /* Current spooler thread */
} DrvSpooler;

typedef struct {
    int       threads;           /* Number of writer threads to run */
    size_t    maxsize;           /* Max content size to use writer thread */
    size_t    bufsize;           /* Size of the output buffer */
    int       doStream;          /* Activate writer for HTML streaming */
    Ns_Mutex lock;               /* Lock around writer queues */
    SpoolerQueue *firstPtr;      /* List of writer threads */
    SpoolerQueue *curPtr;        /* Current writer thread */
} DrvWriter;

typedef struct Driver {

    /*
     * Visible in Ns_Driver.
     */

    void  *arg;                         /* Driver callback data */
    const char  *server;                /* Virtual server name */
    const char  *module;                /* Driver module */
    const char  *name;                  /* Driver name */
    const char  *location;              /* Location, e.g, "http://foo:9090" */
    const char  *address;               /* Address in location, e.g. "foo" */
    const char  *protocol;              /* Protocol in location, e.g, "http" */
    long   sendwait;                    /* send() I/O timeout */
    long   recvwait;                    /* recv() I/O timeout */
    size_t bufsize;                     /* Conn bufsize (0 for SSL) */
    const char  *extraHeaders;          /* Extra header fields added for every request */

    /*
     * Private to Driver.
     */

    struct Driver         *nextPtr;     /* Next in list of drivers */
    struct NsServer       *servPtr;     /* Driver virtual server */
    Ns_DriverListenProc   *listenProc;
    Ns_DriverAcceptProc   *acceptProc;
    Ns_DriverRecvProc     *recvProc;
    Ns_DriverSendProc     *sendProc;
    Ns_DriverSendFileProc *sendFileProc; /* Optional - optimize direct file send. */
    Ns_DriverKeepProc     *keepProc;
    Ns_DriverRequestProc  *requestProc;
    Ns_DriverCloseProc    *closeProc;
    unsigned int opts;                  /* NS_DRIVER_* options */
    long closewait;                     /* Graceful close timeout */
    long keepwait;                      /* Keepalive timeout */
    size_t keepmaxdownloadsize;         /* When set, allow keepalive only for download requests up to this size */
    size_t keepmaxuploadsize;           /* When set, allow keepalive only for upload requests up to this size */
    NS_SOCKET sock;                     /* Listening socket */
    NS_POLL_NFDS_TYPE pidx;             /* poll() index */
    const char *bindaddr;               /* Numerical listen address */
    int port;                           /* Port in location */
    int backlog;                        /* listen() backlog */
    Tcl_WideInt maxinput;               /* Maximum request bytes to read */
    Tcl_WideInt maxupload;              /* Uploads that exceed will go into temp file without parsing */
    char *uploadpath;                   /* Path where uploaded files will be spooled */
    int maxline;                        /* Maximum request line size */
    int maxheaders;                     /* Maximum number of request headers */
    Tcl_WideInt readahead;              /* Maximum request size in memory */
    int queuesize;                      /* Current number of sockets in the queue */
    int maxqueuesize;                   /* Maximum number of sockets in the queue */
    int acceptsize;                     /* Number requests to accept at once */
    unsigned int loggingFlags;          /* Logging control flags */

    unsigned int flags;                 /* Driver state flags. */
    Ns_Thread thread;                   /* Thread id to join on shutdown. */
    Ns_Mutex lock;                      /* Lock to protect lists below. */
    Ns_Cond cond;                       /* Cond to signal reader threads,
                                         * driver query, startup, and shutdown. */
    NS_SOCKET trigger[2];               /* Wakeup trigger pipe. */

    struct Sock *sockPtr;               /* Free list of Sock structures */
    struct Sock *closePtr;              /* First conn ready for graceful close */

    DrvSpooler spooler;                 /* Tracks upload spooler threads */
    DrvWriter  writer;                  /* Tracks writer threads */

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

    struct Driver      *drvPtr;
    NS_SOCKET           sock;
    struct sockaddr_in  sa;              /* Actual peer address */
    void               *arg;             /* Driver context. */

    /*
     * Private to Sock.
     */

    struct Sock        *nextPtr;
    struct NsServer    *servPtr;

    const char         *location;
    int                 keep;
    int                 pidx;            /* poll() index */
    unsigned int        flags;           /* state flags used by driver */
    Ns_Time             timeout;
    Request            *reqPtr;

    Ns_Time             acceptTime;

    int                 tfd;             /* file descriptor with request contents */
    char               *taddr;           /* mmap-ed temporary file */
    size_t              tsize;           /* size of mmap region */
    char               *tfile;           /* name of regular temporary file */

    void               *sls[1];          /* Slots for sls storage */

} Sock;

/*
 * The following structure maintains data from an
 * updated form file.
 */

typedef struct FormFile {
    Ns_Set *hdrs;
    off_t   off;
    size_t  len;
} FormFile;

/*
 * The following structure defines per-request limits.
 */

typedef struct NsLimits {
    const char      *name;
    unsigned int     maxrun;    /* Max conns to run at once */
    unsigned int     maxwait;   /* Max conns waiting to run before being dropped */
    size_t	     maxupload; /* Max data accepted */
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

    Ns_Request *request;

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

    Ns_Time acceptTime;          /* time stamp, when the request was accepted */
    Ns_Time requestQueueTime;    /* time stamp, when the request was queued */
    Ns_Time requestDequeueTime;  /* time stamp, when the request was dequeued */
    Ns_Time filterDoneTime;      /* time stamp, after filters */

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
    ssize_t responseLength;
    int responseStatus;
    int recursionCount;
    int keep;

    int fd;
    WriterSock *strWriter;

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
    ConnThreadState       state;
    Ns_Cond               cond;        /* Cond for signaling this conn thread */
    Ns_Mutex              lock;
    struct ConnThreadArg *nextPtr;     /* used for the conn thread queue */
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
            int   num;
            Conn *firstPtr;
            Conn *lastPtr;
        } wait;

        Ns_Cond  cond;
        Ns_Mutex lock;
        int      lowwatermark;
        int      highwatermark;

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
        unsigned int nextid;
        int min;
        int max;
        int current;
        int idle;
        long timeout;
        int creating;
	Ns_Mutex lock;
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
        unsigned long spool;
        unsigned long queued;
	unsigned long processed;
        unsigned long connthreads;
        Ns_Time acceptTime;
	Ns_Time queueTime; 
	Ns_Time filterTime; 
	Ns_Time runTime;
    } stats;

} ConnPool;

/*
 * The following structure is allocated for each virtual server.
 */

typedef struct NsServer {

    char *server;

    /*
     * The following struct maintains the connection pool(s).
     */

    struct {
        Ns_Mutex lock;
        uintptr_t nextconnid;
        bool shutdown;
        ConnPool *firstPtr;
        ConnPool *defaultPtr;
        Ns_Thread joinThread;
    } pools;

    /*
     * The following struct maintains various server options.
     */

    struct {
        bool flushcontent;
        bool modsince;
        bool noticedetail;
        int  errorminsize;
        const char *realm;
        Ns_HeaderCaseDisposition hdrcase;
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
        int dirc;
        const char *dirproc;
        const char *diradp;
        Ns_UrlToFileProc *url2file;
    } fastpath;

    /*
     * The following struct maintains virtual host config.
     */

    struct {
        bool enabled;
        unsigned int opts; /* NSD_STRIP_WWW | NSD_STRIP_PORT */
        const char *hostprefix;
        int hosthashlevel;
        Ns_ServerRootProc *serverRootProc;
        void *serverRootArg;
        Ns_ConnLocationProc *connLocationProc;
        void *connLocationArg;
        Ns_LocationProc *locationProc; /* Depreciated */
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

    struct Junction *urlspace[MAX_URLSPACES];

    /*
     * The following struct maintains the core Tcl config.
     */

    struct {
        const char *library;
        struct TclTrace *firstTracePtr;
        struct TclTrace *lastTracePtr;
        const char *initfile;
        Ns_RWLock lock;
        const char *script;
        int length;
        int epoch;
        Tcl_Obj *modules;
        Tcl_HashTable runTable;
        const char **errorLogHeaders;
        Tcl_HashTable caches;
        Ns_Mutex cachelock;

        /*
         * The following tracks synchronization
         * objects which are looked up by name.
         */

        struct {
            Tcl_HashTable mutexTable, csTable, semaTable, condTable, rwTable;
            unsigned int  mutexId, csId, semaId, condId, rwId;
            Ns_Mutex      lock;
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

        Ns_Cond pagecond;
        Ns_Mutex pagelock;
        Tcl_HashTable pages;
        Ns_RWLock taglock;
        Tcl_HashTable tags;

    } adp;

    struct {
        bool enable;    /* on/off */
        int  level;     /* 1-9 */
        int  minsize;   /* min size of response to compress, in bytes */
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

} NsServer;

/*
 * The following structure is allocated for each interp.
 */

typedef struct NsInterp {

    Tcl_Interp *interp;
    NsServer   *servPtr;
    int         deleteInterp;  /* Interp should be deleted on next deallocation */
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
	unsigned int	   flags;
	AdpResult	   exception;
	int		   refresh;
	size_t		   bufsize;
	int                errorLevel;
	int                debugLevel;
	int                debugInit;
	char              *debugFile;
	Ns_Cache	  *cache;
	int                depth;
	const char	  *cwd;
	struct AdpFrame	  *framePtr;
	Ns_Conn		  *conn;
	Tcl_Channel	   chan;
	Tcl_DString	   output;
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

} NsInterp;

/*
 * Tcl object and string commands.
 */

NS_EXTERN Tcl_ObjCmdProc
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
    NsTclAdpTellObjCmd,
    NsTclAdpTruncObjCmd,
    NsTclAfterObjCmd,
    NsTclAtCloseObjCmd,
    NsTclAtExitObjCmd,
    NsTclAtPreStartupObjCmd,
    NsTclAtShutdownObjCmd,
    NsTclAtSignalObjCmd,
    NsTclAtStartupObjCmd,
    NsTclCacheAppendObjCmd,
    NsTclCacheCreateObjCmd,
    NsTclCacheEvalObjCmd,
    NsTclCacheFlushObjCmd,
    NsTclCacheGetObjCmd,
    NsTclCacheIncrObjCmd,
    NsTclCacheKeysObjCmd,
    NsTclCacheLappendObjCmd,
    NsTclCacheNamesObjCmd,
    NsTclCacheStatsObjCmd,
    NsTclCancelObjCmd,
    NsTclChanObjCmd,
    NsTclCharsetsObjCmd,
    NsTclCondObjCmd,
    NsTclConfigObjCmd,
    NsTclConfigSectionObjCmd,
    NsTclConfigSectionsObjCmd,
    NsTclConnObjCmd,
    NsTclConnSendFpObjCmd,
    NsTclCritSecObjCmd,
    NsTclCryptObjCmd,
    NsTclDeleteCookieObjCmd,
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
    NsTclHTUUDecodeObjCmd,
    NsTclHTUUEncodeObjCmd,
    NsTclHashPathObjCmd,
    NsTclHeadersObjCmd,
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
    NsTclListLimitsObjCmd,
    NsTclLocalTimeObjCmd,
    NsTclLocationProcObjCmd,
    NsTclLogCtlObjCmd,
    NsTclLogObjCmd,
    NsTclLogRollObjCmd,
    NsTclMD5ObjCmd,
    NsTclModuleLoadObjCmd,
    NsTclModulePathObjCmd,
    NsTclMutexObjCmd,
    NsTclNormalizePathObjCmd,
    NsTclNsvAppendObjCmd,
    NsTclNsvArrayObjCmd,
    NsTclNsvBucketObjCmd,
    NsTclNsvExistsObjCmd,
    NsTclNsvGetObjCmd,
    NsTclNsvIncrObjCmd,
    NsTclNsvLappendObjCmd,
    NsTclNsvNamesObjCmd,
    NsTclNsvSetObjCmd,
    NsTclNsvUnsetObjCmd,
    NsTclPagePathObjCmd,
    NsTclParseArgsObjCmd,
    NsTclParseHttpTimeObjCmd,
    NsTclParseQueryObjCmd,
    NsTclPauseObjCmd,
    NsTclPngSizeObjCmd,
    NsTclProgressObjCmd,
    NsTclPurgeFilesObjCmd,
    NsTclRWLockObjCmd,
    NsTclRandObjCmd,
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
    NsTclReturnUnauthorizedObjCmd,
    NsTclReturnUnavailableObjCmd,
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
    NsTclSymlinkObjCmd,
    NsTclThreadObjCmd,
    NsTclTimeObjCmd,
    NsTclTmpNamObjCmd,
    NsTclTruncateObjCmd,
    NsTclUnRegisterOpObjCmd,
    NsTclUnRegisterUrl2FileObjCmd,
    NsTclUnscheduleObjCmd,
    NsTclUrl2FileObjCmd,
    NsTclUrlDecodeObjCmd,
    NsTclUrlEncodeObjCmd,
    NsTclWriteContentObjCmd,
    NsTclWriteFpObjCmd,
    NsTclWriteObjCmd,
    NsTclWriterObjCmd,
    TclX_KeylgetObjCmd,
    TclX_KeyldelObjCmd,
    TclX_KeylkeysObjCmd,
    TclX_KeylsetObjCmd;

NS_EXTERN Tcl_CmdProc
    NsTclAdpStatsCmd,
    NsTclHrefsCmd,
    NsTclLibraryCmd,
    NsTclMkTempCmd,
    NsTclParseHeaderCmd,
    NsTclQuoteHtmlCmd,
    NsTclStripHtmlCmd;

/*
 * Libnsd initialization routines.
 */

NS_EXTERN void NsInitBinder(void);
NS_EXTERN void NsInitConf(void);
NS_EXTERN void NsInitFd(void);
NS_EXTERN void NsInitListen(void);
NS_EXTERN void NsInitLog(void);
NS_EXTERN void NsInitInfo(void);
NS_EXTERN void NsInitModLoad(void);
NS_EXTERN void NsInitProcInfo(void);
NS_EXTERN void NsInitQueue(void);
NS_EXTERN void NsInitLimits(void);
NS_EXTERN void NsInitDrivers(void);
NS_EXTERN void NsInitServers(void);
NS_EXTERN void NsInitSched(void);
NS_EXTERN void NsInitSls(void);
NS_EXTERN void NsInitTcl(void);
NS_EXTERN void NsInitRequests(void);
NS_EXTERN void NsInitUrl2File(void);

NS_EXTERN void NsConfigAdp(void);
NS_EXTERN void NsConfigLog(void);
NS_EXTERN void NsConfigFastpath(void);
NS_EXTERN void NsConfigMimeTypes(void);
NS_EXTERN void NsConfigDNS(void);
NS_EXTERN void NsConfigRedirects(void);
NS_EXTERN void NsConfigVhost(void);
NS_EXTERN void NsConfigEncodings(void);

/*
 * Virtual server management routines.
 */

NS_EXTERN void NsInitServer(char *server, Ns_ServerInitProc *initProc)
    NS_GNUC_NONNULL(1);
NS_EXTERN void NsRegisterServerInit(Ns_ServerInitProc *proc)
    NS_GNUC_NONNULL(1);
NS_EXTERN NsServer *NsGetInitServer(void);
NS_EXTERN NsServer *NsGetServer(const char *server);
NS_EXTERN void NsStartServers(void);
NS_EXTERN void NsStopServers(const Ns_Time *toPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsStartServer(const NsServer *servPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsStopServer(NsServer *servPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsWaitServer(NsServer *servPtr, const Ns_Time *toPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void NsWakeupDriver(const Driver *drvPtr) NS_GNUC_NONNULL(1);

/*
 * Url-specific data routines.
 */

NS_EXTERN void *NsUrlSpecificGet(NsServer *servPtr, const char *method,
				 const char *url, int id, int fast)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * Socket driver callbacks.
 */

NS_EXTERN ssize_t NsDriverSend(Sock *sockPtr, const struct iovec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1);
NS_EXTERN ssize_t NsDriverSendFile(Sock *sockPtr, Ns_FileVec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN ssize_t
NsSockSendFileBufsIndirect(Ns_Sock *sock, const Ns_FileVec *bufs, int nbufs,
                           const Ns_Time *timeoutPtr, unsigned int flags,
                           Ns_DriverSendProc *sendProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(6);



NS_EXTERN int  NsQueueConn(Sock *sockPtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void NsEnsureRunningConnectionThreads(const NsServer *servPtr, ConnPool *poolPtr)
    NS_GNUC_NONNULL(1);
NS_EXTERN void NsMapPool(ConnPool *poolPtr, const char *map)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN void NsSockClose(Sock *sockPtr, int keep)
    NS_GNUC_NONNULL(1);
NS_EXTERN int NsPoll(struct pollfd *pfds, int nfds, const Ns_Time *timeoutPtr);

NS_EXTERN Request *NsGetRequest(Sock *sockPtr, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1);
NS_EXTERN void NsFreeRequest(Request *reqPtr);

NS_EXTERN void NsWriterLock(void);
NS_EXTERN void NsWriterUnlock(void);
NS_EXTERN void NsWriterFinish(WriterSock *wrSockPtr);
NS_EXTERN int  NsWriterQueue(Ns_Conn *conn, size_t nsend, Tcl_Channel chan,
			  FILE *fp, int fd, struct iovec *bufs, int nbufs, 
			  int everysize);

NS_EXTERN void NsFreeAdp(NsInterp *itPtr);
NS_EXTERN void NsTclRunAtClose(NsInterp *itPtr)
     NS_GNUC_NONNULL(1);

NS_EXTERN int NsUrlToFile(Ns_DString *dsPtr, NsServer *servPtr, const char *url);
NS_EXTERN char *NsPageRoot(Ns_DString *dest, const NsServer *servPtr, const char *host);

/*
 * External callback functions.
 */

NS_EXTERN Ns_ConnLocationProc NsTclConnLocation;
NS_EXTERN Ns_SchedProc NsTclSchedProc;
NS_EXTERN Ns_ServerRootProc NsTclServerRoot;
NS_EXTERN Ns_ThreadProc NsTclThread;
NS_EXTERN Ns_ArgProc NsTclThreadArgProc;
NS_EXTERN Ns_SockProc NsTclSockProc;
NS_EXTERN Ns_ArgProc NsTclSockArgProc;
NS_EXTERN Ns_ThreadProc NsConnThread;
NS_EXTERN Ns_ArgProc NsConnArgProc;
NS_EXTERN Ns_FilterProc NsTclFilterProc;
NS_EXTERN Ns_FilterProc NsShortcutFilterProc;
NS_EXTERN Ns_OpProc NsTclRequestProc;
NS_EXTERN Ns_OpProc NsAdpPageProc;
NS_EXTERN Ns_ArgProc NsAdpPageArgProc;
NS_EXTERN Ns_TclTraceProc NsTclTraceProc;
NS_EXTERN Ns_UrlToFileProc NsUrlToFileProc;
NS_EXTERN Ns_Url2FileProc NsTclUrl2FileProc;
NS_EXTERN Ns_Url2FileProc NsMountUrl2FileProc;
NS_EXTERN Ns_ArgProc NsMountUrl2FileArgProc;

NS_EXTERN void NsGetCallbacks(Tcl_DString *dsPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void NsGetSockCallbacks(Tcl_DString *dsPtr);
NS_EXTERN void NsGetScheduled(Tcl_DString *dsPtr);
NS_EXTERN void NsGetMimeTypes(Tcl_DString *dsPtr);
NS_EXTERN void NsGetTraces(Tcl_DString *dsPtr, const char *server);
NS_EXTERN void NsGetFilters(Tcl_DString *dsPtr, const char *server);
NS_EXTERN void NsGetRequestProcs(Tcl_DString *dsPtr, CONST char *server);
NS_EXTERN void NsGetUrl2FileProcs(Ns_DString *dsPtr, CONST char *server);

#ifdef _WIN32
NS_EXTERN int NsConnectService(void);
NS_EXTERN int NsInstallService(char *service);
NS_EXTERN int NsRemoveService(char *service);
#endif

NS_EXTERN void NsCreatePidFile(void);
NS_EXTERN void NsRemovePidFile(void);

NS_EXTERN void NsLogOpen(void);
NS_EXTERN void NsTclInitObjs(void);
NS_EXTERN void NsBlockSignals(int debug);
NS_EXTERN void NsBlockSignal(int sig);
NS_EXTERN void NsUnblockSignal(int sig);
NS_EXTERN int  NsHandleSignals(void);
NS_EXTERN void NsStopDrivers(void);
NS_EXTERN void NsStopSpoolers(void);
NS_EXTERN void NsPreBind(const char *args, const char *file);
NS_EXTERN void NsClosePreBound(void);
NS_EXTERN char *NsConfigRead(const char *file);
NS_EXTERN void NsConfigEval(const char *config, int argc, char *const*argv, int optind);
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
NS_EXTERN NsInterp *NsGetInterpData(Tcl_Interp *interp)  NS_GNUC_NONNULL(1);
NS_EXTERN void NsFreeConnInterp(Conn *connPtr)           NS_GNUC_NONNULL(1);

NS_EXTERN struct Bucket *NsTclCreateBuckets(const char *server, int nbuckets) NS_GNUC_NONNULL(1);

NS_EXTERN void NsSlsCleanup(Sock *sockPtr);
NS_EXTERN void NsClsCleanup(Conn *connPtr);
NS_EXTERN void NsTclAddBasicCmds(NsInterp *itPtr);
NS_EXTERN void NsTclAddServerCmds(NsInterp *itPtr);

NS_EXTERN void NsRestoreSignals(void);
NS_EXTERN void NsSendSignal(int sig);

/*
 * limits.c
 */
NS_EXTERN NsLimits *NsGetRequestLimits(NsServer *servPtr, const char *method, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * range.c
 */
NS_EXTERN int NsMatchRange(const Ns_Conn *conn, time_t mtime)
    NS_GNUC_NONNULL(1);

NS_EXTERN int NsConnParseRange(Ns_Conn *conn, const char *type,
			       int fd, const void *data, size_t objLength,
			       Ns_FileVec *bufs, int *nbufsPtr, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_NONNULL(7) NS_GNUC_NONNULL(8);

/*
 * conn.c
 */
NS_EXTERN const char * NsConnIdStr(const Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

/*
 * request parsing
 */
NS_EXTERN int NsParseAcceptEncoding(double version, const char *hdr);


/*
 * ADP routines.
 */

NS_EXTERN int NsAdpAppend(NsInterp *itPtr, const char *buf, int len) 
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int NsAdpFlush(NsInterp *itPtr, int doStream) 
  NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpDebug(NsInterp *itPtr, const char *host, const char *port, const char *procs)
  NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpEval(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *resvar)
  NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpSource(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv, const char *resvar)
  NS_GNUC_NONNULL(1);

NS_EXTERN int NsAdpInclude(NsInterp *itPtr, int objc, Tcl_Obj *CONST* objv,
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

/*
 * Callback routines.
 */

NS_EXTERN int  NsRunFilters(Ns_Conn *conn, Ns_FilterType why) NS_GNUC_NONNULL(1);
NS_EXTERN void NsRunCleanups(Ns_Conn *conn)                   NS_GNUC_NONNULL(1);
NS_EXTERN void NsRunTraces(Ns_Conn *conn)                     NS_GNUC_NONNULL(1);
NS_EXTERN void NsRunPreStartupProcs(void);
NS_EXTERN void NsRunSignalProcs(void);
NS_EXTERN void NsRunStartupProcs(void);
NS_EXTERN void NsRunAtReadyProcs(void);
NS_EXTERN void NsRunAtExitProcs(void);

/*
 * Upload progress routines.
 */

NS_EXTERN void NsConfigProgress(void);
NS_EXTERN void NsUpdateProgress(Ns_Sock *sock) NS_GNUC_NONNULL(1);

/*
 * watchdog.c
 */

NS_EXTERN int NsForkWatchedProcess(void);

/*
 * Utility functions.
 */

NS_EXTERN int NsCloseAllFiles(int errFd);
NS_EXTERN int NsMemMap(const char *path, size_t size, int mode, FileMap *mapPtr);
NS_EXTERN void NsMemUmap(const FileMap *mapPtr);

NS_EXTERN void NsStopSockCallbacks(void);
NS_EXTERN void NsStopScheduledProcs(void);
NS_EXTERN void NsGetBuf(char **bufPtr, int *sizePtr);

NS_EXTERN const char *NsFindCharset(const char *mimetype, size_t *lenPtr);
NS_EXTERN int NsEncodingIsUtf8(const Tcl_Encoding encoding);

NS_EXTERN void NsUrlSpecificWalk(int id, const char *server, Ns_ArgProc func,
				 Tcl_DString *dsPtr);

NS_EXTERN void NsParseAuth(Conn *connPtr, char *auth)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool NsTclObjIsByteArray(const Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool NsTclTimeoutException(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1);


/*
 * Proxy support
 */

NS_EXTERN int NsConnRunProxyRequest(Ns_Conn *conn);

#endif /* NSD_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
