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

#ifndef _GNU_SOURCE
 #define _GNU_SOURCE
#endif

#include "ns.h"
#include <assert.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
  #define STDOUT_FILENO 1
  #define STDERR_FILENO 2
  #define S_ISREG(m) ((m)&_S_IFREG)
  #define S_ISDIR(m) ((m)&_S_IFDIR)
  #include <sys/stat.h>
#else
  #include <sys/resource.h>
  #include <sys/wait.h>
  #include <sys/ioctl.h>
  #include <ctype.h>
  #include <grp.h>
  #include <pthread.h>
  #include <sys/mman.h>
#endif  /* WIN32 */

#ifdef HAVE_POLL
  #include <poll.h>
#else
  #define POLLIN   001
  #define POLLPRI  002
  #define POLLOUT  004
  #define POLLNORM POLLIN
  #define POLLERR  010
  #define POLLHUP  020
  #define POLLNVAL 040
  struct pollfd {
      int fd;
      short int events;
      short int revents;
  };
  extern int poll(struct pollfd *, unsigned long, int);
#endif

#ifdef __linux
  #include <sys/prctl.h>
#endif

#ifdef __hp
  #define seteuid(i) setresuid((-1),(i),(-1))
#endif

#ifdef __sun
  #include <sys/filio.h>
  #include <sys/systeminfo.h>
  #define gethostname(b,s) (!(sysinfo(SI_HOSTNAME, b, s) > 0))
#endif

#ifdef __unixware
  #include <sys/filio.h>
#endif

#ifndef F_CLOEXEC
  #define F_CLOEXEC 1
#endif

#ifdef _WIN32
  #define NS_SIGHUP   1
  #define NS_SIGINT   2
  #define NS_SIGTERM 15
#else
  #define NS_SIGHUP  SIGHUP
  #define NS_SIGINT  SIGINT
  #define NS_SIGTERM SIGTERM
#endif

/*
 * Constants
 */

#define NSD_NAME    "NaviServer"
#define NSD_TAG     "$Name$"
#define NSD_VERSION NS_PATCH_LEVEL

#define NS_CONFIG_PARAMETERS "ns/parameters"
#define NS_CONFIG_THREADS    "ns/threads"

#define ADP_OK         0  
#define ADP_BREAK      1
#define ADP_ABORT      2
#define ADP_RETURN     4

#define NSD_STRIP_WWW  1
#define NSD_STRIP_PORT 2

/*
 * The following is the default text/html content type
 * sent to the browsers.  The charset is also used for
 * both input (url query) and output encodings.
 */

#define NSD_TEXTHTML "text/html; charset=iso-8859-1"

/*
 * Types definitions.
 */

typedef int bool;

struct _nsconf {
    char *argv0;
    char *nsd;
    char *name;
    char *version;
    char *home;
    char *config;
    char *build;
    int pid;
    time_t boot_t;
    char hostname[255];
    char address[16];
    int shutdowntimeout;
    int backlog;

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
        int maxelapsed;
    } sched;
    
#ifdef _WIN32    
    struct {
        bool checkexit;
    } exec;
#endif

    struct {
        char *sharedlibrary;
        char *version;
        bool lockoninit;
    } tcl;
    
    struct {
        char *outputCharset;
        Tcl_Encoding  outputEncoding;
        bool hackContentTypeP;
        char *urlCharset;
        Tcl_Encoding urlEncoding;
    } encoding;

};

extern struct _nsconf nsconf;

/*
 * The following structure defines a key for hashing
 * a file by device/inode.
 */

typedef struct FileKey {
    dev_t dev;
    ino_t ino;
} FileKey;

#define FILE_KEYS (sizeof(FileKey)/sizeof(int))

/*
 * The following structure tracks a memory-mapped file
 * in a platform-neutral way.
 */

typedef struct FileMap {
    char *addr;                 /* Mapped to this virtual address */
    int size;                   /* Size of the mapped region */
    int handle;                 /* OS handle of the opened/mapped file */
    void *mapobj;               /* Mapping object (Win32 only) */
} FileMap;

/*
 * For the time being, don't try to be very clever
 * and define (platform-neutral) just those two modes
 * for mapping the files.
 * Although the underlying implementation(s) can do 
 * much more, we really need only one (read-maps) now. 
 */

#ifdef _WIN32
  #define NS_MMAP_READ   FILE_MAP_READ
  #define NS_MMAP_WRITE  FILE_MAP_WRITE
#else
  #define NS_MMAP_READ   PROT_READ
  #define NS_MMAP_WRITE  PROT_WRITE
#endif


/*
 * The following structure defines blocks of ADP.  The
 * len pointer is an array of ints with positive values
 * indicating text to copy and negative values indicating
 * scripts to evaluate.  The text and script chars are
 * packed together without null char separators starting
 * at base.  The len and base data are either stored
 * in an AdpParse structure or copied at the end of
 * a cached Page structure.
 */

typedef struct AdpCode {
    int nblocks;
    int nscripts;
    char *base;
    int *len;
} AdpCode;

/*
 * The following structure is used to accumulate the 
 * results of parsing an ADP string.
 */

typedef struct AdpParse {
    AdpCode code;
    Tcl_DString hdr;
    Tcl_DString text;
} AdpParse;

/*
 * The following structure defines the entire request
 * including HTTP request line, headers, and content.
 */

typedef struct Request {
    struct Request *nextPtr;    /* Next on free list. */
    Ns_Request *request;        /* Parsed request line. */
    Ns_Set *headers;            /* Input headers. */
    char peer[16];              /* Client peer address. */
    int port;                   /* Client peer port. */

    /*
     * The following pointers are used to access the
     * buffer contents after the read-ahead is complete.
     */

    char *next;                 /* Next read offset. */
    char *content;              /* Start of content. */
    int length;                 /* Length of content. */
    int avail;                  /* Bytes avail in buffer. */
    int leadblanks;             /* Number of leading blank lines read */

    /*
     * The following offsets are used to manage 
     * the buffer read-ahead process.
     */

    int woff;                   /* Next write buffer offset. */
    int roff;                   /* Next read buffer offset. */
    int coff;                   /* Content buffer offset. */
    Tcl_DString buffer;         /* Request and content buffer. */

} Request;

/*
 * The following structure maitains data for each instance of
 * a driver initialized with Ns_DriverInit.
 */

struct NsServer;

typedef struct Driver {

    /*
     * Visible in Ns_Driver.
     */

    void *arg;                  /* Driver callback data. */
    char *server;               /* Virtual server name. */
    char *module;               /* Driver module. */
    char *name;                 /* Driver name. */
    char *location;             /* Location, e.g, "http://foo:9090" */
    char *address;              /* Address in location, e.g. "foo" */
    char *protocol;             /* Protocol in location, e.g, "http" */
    int sendwait;               /* send() I/O timeout. */
    int recvwait;               /* recv() I/O timeout. */
    int bufsize;                /* Conn bufsize (0 for SSL) */
    int sndbuf;                 /* setsockopt() SNDBUF option. */
    int rcvbuf;                 /* setsockopt() RCVBUF option. */

    /*
     * Private to Driver.
     */

    struct Driver *nextPtr;     /* Next in list of drivers. */
    struct NsServer *servPtr;   /* Driver virtual server. */
    Ns_DriverProc *proc;        /* Driver callback. */
    int opts;                   /* Driver options. */
    int closewait;              /* Graceful close timeout. */
    int keepwait;               /* Keepalive timeout. */
    int keepallmethods;         /* Keepalive all methods or just GET? */
    SOCKET sock;                /* Listening socket. */
    int pidx;                   /* poll() index. */
    char *bindaddr;             /* Numerical listen address. */
    int port;                   /* Port in location. */
    int backlog;                /* listen() backlog. */
    int maxinput;               /* Maximum request bytes to read. */
    int maxline;                /* Maximum request line size. */
    int maxheaders;             /* Maximum number of request headers. */
    int readahead;              /* Maximum request size in memory. */
    int uploadsize;             /* Minimum upload size for statistics tracking. */
    int writersize;             /* Maximum content size when to use writer thread. */
    unsigned int loggingFlags;  /* Logging control flags */
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

    struct Driver *drvPtr;
    SOCKET sock;
    void *arg;

    /*
     * Private to Sock.
     */

    struct Sock *nextPtr;
    struct NsServer *servPtr;
    char *location;
    struct sockaddr_in sa;
    int keep;
    int pidx;                   /* poll() index. */
    Ns_Time timeout;
    Request *reqPtr;
    int tfd;
    char *taddr;
    size_t tsize;

    struct {
      char *url;
      unsigned long size;
      unsigned long length;
    } upload;

} Sock;

/*
 * The following structure maintains data from an
 * updated form file.
 */

typedef struct FormFile {
    Ns_Set *hdrs;
    off_t off;
    off_t len;
} FormFile;

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

    char *authUser;
    char *authPasswd;

    int contentLength;
    int flags;

    /*
     * Visible only in a Conn:
     */
    
    struct Conn *prevPtr;
    struct Conn *nextPtr;
    struct Sock *sockPtr;

    /*
     * The following are copied from sockPtr so they're valid
     * after the connection is closed (e.g., within traces).
     */

    char *server;
    char *location;

    struct Request  *reqPtr;
    struct NsServer *servPtr;
    struct Driver   *drvPtr;
 
    int id;
    char idstr[16];
    
    Ns_Time startTime;
    struct NsInterp *itPtr;

    Tcl_Encoding encoding;
    Tcl_Encoding urlEncoding;

    int nContentSent;
    int responseStatus;
    int responseLength;
    char *responseVersion;
    int recursionCount;

    Ns_Set *query;
    Tcl_HashTable files;
    Tcl_DString queued;
    void *cls[NS_CONN_MAXCLS];

} Conn;

/*
 * The following structure maintains a connection thread pool.
 */

typedef struct ConnPool {
    char *pool;
    struct ConnPool *nextPtr;
    struct NsServer *servPtr;

    /*
     * The following struct maintains the active and waiting connection
     * queues, the free conn list, the next conn id, and the number
     * of waiting connects.
     */

    struct {
        Conn *freePtr;

        struct {
            int   num;
            Conn *firstPtr;
            Conn *lastPtr;
        } wait;

        struct {
            Conn *firstPtr;
            Conn *lastPtr;
        } active;
        Ns_Cond  cond;

    } queue;
    
    /*
     * The following struct maintins the state of the threads.  Min and max
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
        int timeout;
    } threads;

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
        int nextconnid;
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
        CONST char *realm;
        Ns_HeaderCaseDisposition hdrcase;
    } opts;
    
    /*
     * Encoding defaults for the server
     */

    struct {
        char *outputCharset;
        Tcl_Encoding outputEncoding;
        bool hackContentTypeP;
        char *urlCharset;
        Tcl_Encoding urlEncoding;
    } encoding;

    struct {
        CONST char *serverdir;  /* Virtual server files path */
        CONST char *pagedir;    /* Path to public pages */
        CONST char *pageroot;   /* Absolute path to public pages */
        CONST char **dirv;
        int dirc;
        CONST char *dirproc;
        CONST char *diradp;
        bool mmap;
        int cachemaxentry;
        Ns_UrlToFileProc *url2file;
        Ns_Cache *cache;
    } fastpath;

    /*
     * The following struct maintains virtual host config.
     */

    struct {
        bool enabled;
        int opts; /* NSD_STRIP_WWW | NSD_STRIP_PORT */
        CONST char *hostprefix;
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
    } filter;
    
    /*
     * The following struct maintains the core Tcl config.
     */

    struct {         
        char *library;
        struct Trace *firstTracePtr;
        char *initfile;
        Ns_RWLock lock;
        char *script;
        int length;
        int epoch;
        Tcl_Obj *modules;
        Tcl_HashTable runTable;
        CONST char **errorLogHeaders;
        Tcl_HashTable caches;
        Ns_Mutex cachelock;
        int cacheTimeout;
    } tcl;
    
    /*
     * The following struct maintains ADP config,
     * registered tags, and read-only page text.
     */
    
    struct {
        CONST char *errorpage;
        CONST char *startpage;

        bool enableexpire;
        bool enabledebug;

        CONST char *debuginit;
        CONST char *defaultparser;

        size_t cachesize;

        Ns_Cond pagecond;
        Ns_Mutex pagelock;
        Tcl_HashTable pages;
        Ns_RWLock taglock;
        Tcl_HashTable tags;

        struct {
            bool enable;
            int level;
            int minsize;
        } compress;

    } adp;
    
    /*
     * The following struct maintains the Ns_Set's
     * entered into Tcl with NS_TCL_SET_SHARED.
     */

    struct {
        Ns_Mutex lock;
        Tcl_HashTable table;
    } sets;    
    
    /*
     * The following struct maintains the arrays
     * for the nsv commands.
     */
    
    struct {
        struct Bucket *buckets;
        int nbuckets;
    } nsv;
    
    /*
     * The following struct maintains the vars and
     * lock for the old ns_var command.
     */
    
    struct {
        Ns_Mutex lock;
        Tcl_HashTable table;
    } var;
    
    /*
     * The following struct maintains the init state
     * of ns_share variables, updated with the
     * ns_share -init command.
     */
    
    struct {
        Ns_Cs cs;
        Ns_Mutex lock;
        Ns_Cond cond;
        Tcl_HashTable inits;
        Tcl_HashTable vars;
    } share;

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
    int         delete;  /* Interp should be deleted on next deallocation */
    int         epoch;   /* Run the update script if != to server epoch */
    int         refcnt;  /* Counts recursive allocations of cached interp */

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
    
#define CONN_TCLFORM    1
#define CONN_TCLHDRS    2
#define CONN_TCLOUTHDRS 4
    
    Ns_Conn *conn;
    
    struct {
        int  flags;
        char form[16];
        char hdrs[16];
        char outhdrs[16];
    } nsconn;
    
    /*
     * The following struct maintains per-interp ADP
     * context including the private pages cache.
     */
    
    struct {
        bool stream;
        bool compress;
        int exception;
        int depth;
        int objc;
        Tcl_Obj **objv;
        char *cwd;
        int errorLevel;
        int debugLevel;
        int debugInit;
        char *debugFile;
        Ns_Cache *cache;
        Tcl_DString *outputPtr;
        Tcl_DString *responsePtr;
        Tcl_DString *typePtr;
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
    
    Tcl_HashTable https;
    
} NsInterp;

/*
 * Libnsd initialization routines.
 */

extern void NsInitBinder(void);
extern void NsInitCache(void);
extern void NsInitConf(void);
extern void NsInitEncodings(void);
extern void NsInitListen(void);
extern void NsInitLog(void);
extern void NsInitInfo(void);
extern void NsInitMimeTypes(void);
extern void NsInitModLoad(void);
extern void NsInitProcInfo(void);
extern void NsInitQueue(void);
extern void NsInitDrivers(void);
extern void NsInitSched(void);
extern void NsInitTcl(void);
extern void NsInitUrlSpace(void);
extern void NsInitRequests(void);
extern void NsInitUrl2File(void);

extern void NsConfigLog(void);

extern int  NsQueueConn(Sock *sockPtr, Ns_Time *nowPtr);
extern void NsMapPool(ConnPool *poolPtr, char *map);
extern int  NsSockSend(Sock *sockPtr, struct iovec *bufs, int nbufs);
extern void NsSockClose(Sock *sockPtr, int keep);
extern int NsPoll(struct pollfd *pfds, int nfds, Ns_Time *timeoutPtr);

extern Request *NsGetRequest(Sock *sockPtr);
extern void NsFreeRequest(Request *reqPtr);

extern int NsQueueWriter(Ns_Conn *conn, int nsend, Tcl_Channel chan, FILE *fp, int fd);

extern NsServer *NsGetServer(CONST char *server);
extern NsServer *NsGetInitServer(void);

extern Ns_Cache *NsFastpathCache(CONST char *server, int size);

extern void NsFreeAdp(NsInterp *itPtr);
extern void NsTclRunAtClose(NsInterp *itPtr)
     NS_GNUC_NONNULL(1);

extern int NsUrlToFile(Ns_DString *dsPtr, NsServer *servPtr, CONST char *url);
extern char *NsPageRoot(Ns_DString *dest, NsServer *servPtr, CONST char *host);

/*
 * External callback functions.
 */

extern Ns_ConnLocationProc NsTclConnLocation;
extern Ns_SchedProc NsTclSchedProc;
extern Ns_ServerRootProc NsTclServerRoot;
extern Ns_ThreadProc NsTclThread;
extern Ns_ArgProc NsTclThreadArgProc;
extern Ns_SockProc NsTclSockProc;
extern Ns_ArgProc NsTclSockArgProc;
extern Ns_ThreadProc NsConnThread;
extern Ns_ArgProc NsConnArgProc;
extern Ns_FilterProc NsTclFilterProc;
extern Ns_FilterProc NsShortcutFilterProc;
extern Ns_OpProc NsFastPathProc;
extern Ns_OpProc NsTclRequestProc;
extern Ns_OpProc NsAdpRequestProc;
extern Ns_OpProc NsAdpMapProc;
extern Ns_ArgProc NsTclRequestArgProc;
extern Ns_TclTraceProc NsTclTraceProc;
extern Ns_UrlToFileProc NsUrlToFileProc;
extern Ns_Url2FileProc NsTclUrl2FileProc;
extern Ns_Url2FileProc NsMountUrl2FileProc;
extern Ns_ArgProc NsMountUrl2FileArgProc;

extern void NsGetCallbacks(Tcl_DString *dsPtr);
extern void NsGetSockCallbacks(Tcl_DString *dsPtr);
extern void NsGetScheduled(Tcl_DString *dsPtr);
extern void NsGetTraces(Tcl_DString *dsPtr, char *server);
extern void NsGetFilters(Tcl_DString *dsPtr, char *server);
extern void NsGetRequestProcs(Tcl_DString *dsPtr, CONST char *server);
extern void NsGetUrl2FileProcs(Ns_DString *dsPtr, CONST char *server);

#ifdef _WIN32
extern int NsConnectService(void);
extern int NsInstallService(char *service);
extern int NsRemoveService(char *service);
#endif

extern void NsCreatePidFile(char *service);
extern void NsRemovePidFile(char *service);

extern void NsLogOpen(void);
extern void NsTclInitObjs(void);
extern void NsUpdateMimeTypes(void);
extern void NsUpdateEncodings(void);
extern void NsUpdateUrlEncode(void);
extern void NsRunPreStartupProcs(void);
extern void NsStartServers(void);
extern void NsBlockSignals(int debug);
extern int  NsHandleSignals(void);
extern void NsStopDrivers(void);
extern void NsPreBind(char *bindargs, char *bindfile);
extern void NsClosePreBound(void);
extern void NsInitServer(char *server, Ns_ServerInitProc *initProc);
extern char *NsConfigRead(CONST char *file);
extern void NsConfigEval(CONST char *config, int argc, char **argv, int optind);
extern void NsConfUpdate(void);
extern void NsEnableDNSCache(int timeout, int maxentries);
extern void NsStopServers(Ns_Time *toPtr);
extern void NsStartServer(NsServer *servPtr);
extern void NsStopServer(NsServer *servPtr);
extern void NsWaitServer(NsServer *servPtr, Ns_Time *toPtr);
extern void NsStartDrivers(void);
extern void NsWaitDriversShutdown(Ns_Time *toPtr);
extern void NsStartSchedShutdown(void);
extern void NsWaitSchedShutdown(Ns_Time *toPtr);
extern void NsStartSockShutdown(void); 
extern void NsWaitSockShutdown(Ns_Time *toPtr);
extern void NsStartShutdownProcs(void);
extern void NsWaitShutdownProcs(Ns_Time *toPtr);
extern void NsStartTaskQueueShutdown(void);
extern void NsWaitTaskQueueShutdown(Ns_Time *toPtr);
extern void NsStartJobsShutdown(void);
extern void NsWaitJobsShutdown(Ns_Time *toPtr);

extern Tcl_AppInitProc NsTclAppInit;
extern void NsTclInitServer(CONST char *server)
     NS_GNUC_NONNULL(1);
extern void NsInitStaticModules(CONST char *server);
extern NsInterp *NsGetInterpData(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);
extern void NsFreeConnInterp(Conn *connPtr)
     NS_GNUC_NONNULL(1);

extern struct Bucket *NsTclCreateBuckets(char *server, int nbuckets);

extern void NsClsCleanup(Conn *connPtr);
extern void NsTclAddBasicCmds(NsInterp *itPtr);
extern void NsTclAddServerCmds(NsInterp *itPtr);

extern void NsRestoreSignals(void);
extern void NsSendSignal(int sig);

/*
 * ADP routines.
 */

extern Ns_Cache *NsAdpCache(char *server, int size);

extern void NsAdpSetMimeType(NsInterp *itPtr, CONST char *type);
extern void NsAdpFlush(NsInterp *itPtr);
extern void NsAdpStream(NsInterp *itPtr);
extern void NsAdpCompress(NsInterp *itPtr, int compress);
extern void NsAdpParse(AdpParse *parsePtr, NsServer *servPtr, char *utf, int safe);

extern int NsAdpDebug(NsInterp *itPtr, CONST char *host, CONST char *port,
                      CONST char *procs);
extern int NsAdpEval(NsInterp *itPtr, int objc, Tcl_Obj *objv[], int safe,
                     CONST char *resvar);
extern int NsAdpSource(NsInterp *itPtr, int objc, Tcl_Obj *objv[],
                       CONST char *resvar);
extern int NsAdpInclude(NsInterp *itPtr, CONST char *file, int objc, Tcl_Obj *objv[]);

/*
 * Tcl support routines.
 */

extern void NsTclInitQueueType(void);
extern void NsTclInitAddrType(void);
extern void NsTclInitTimeType(void);
extern void NsTclInitKeylistType(void);
extern void NsTclInitSpecType(void);

/*
 * Callback routines.
 */

extern int NsRunFilters(Ns_Conn *conn, int why);
extern void NsRunCleanups(Ns_Conn *conn);
extern void NsRunTraces(Ns_Conn *conn);
extern void NsRunPreStartupProcs(void);
extern void NsRunSignalProcs(void);
extern void NsRunStartupProcs(void);
extern void NsRunAtReadyProcs(void);
extern void NsRunAtExitProcs(void);


/*
 * Utility functions.
 */

extern int NsCloseAllFiles(int errFd);
extern int NsMemMap(CONST char *path, int size, int mode, FileMap *mapPtr);
extern void NsMemUmap(FileMap *mapPtr);

#ifndef _WIN32
extern int Ns_ConnRunRequest(Ns_Conn *conn);
extern int Ns_GetGid(char *group);
extern int Ns_GetUserGid(char *user);
extern int Ns_TclGetOpenFd(Tcl_Interp *, char *, int write, int *fp);
#endif

extern void NsStopSockCallbacks(void);
extern void NsStopScheduledProcs(void);
extern void NsGetBuf(char **bufPtr, int *sizePtr);
extern Tcl_Encoding NsGetTypeEncodingWithDef(CONST char *type, int *used_default);
extern void NsComputeEncodingFromType(CONST char *type, Tcl_Encoding *enc,
                                      int *new_type, Tcl_DString *type_ds);

extern void NsUrlSpecificWalk(int id, CONST char *server, Ns_ArgProc func, 
                              Tcl_DString *dsPtr);

/*
 * Proxy support
 */

extern int NsConnRunProxyRequest(Ns_Conn *conn);

#endif /* NSD_H */
