/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
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
 * ns.h --
 *
 *      All the public types and function declarations for the core server.
 *
 *	$Header$
 */

#ifndef NS_H
#define NS_H

#include "nsversion.h"
#include "nsthread.h"

#ifdef NSD_EXPORTS
# undef NS_EXTERN
# ifdef __cplusplus
#  define NS_EXTERN extern "C" NS_EXPORT
# else
#  define NS_EXTERN extern NS_EXPORT
# endif
#endif

/*
 * Boolean result.
 */

#define NS_TRUE                    1
#define NS_FALSE                   0

/*
 * The following describe various properties of a connection.
 */

#define NS_CONN_CLOSED             0x001 /* The underlying socket is closed */
#define NS_CONN_SKIPHDRS           0x002 /* Client is HTTP/0.9, do not send HTTP headers  */
#define NS_CONN_SKIPBODY           0x004 /* HTTP HEAD request, do not send body */
#define NS_CONN_READHDRS           0x008 /* Unused */
#define NS_CONN_SENTHDRS           0x010 /* Response headers have been sent to client */
#define NS_CONN_WRITE_ENCODED      0x020 /* Unused */
#define NS_CONN_WRITE_CHUNKED      0x040 /* Client expects or has requested a chunked response */
#define NS_CONN_SENT_LAST_CHUNK    0x080 /* Marks that the last chunk was sent in chunked mode */

/*
 * The following are valid return codes from an Ns_UserAuthorizeProc.
 */

/* NS_OK                                       The user's access is authorized */
#define NS_UNAUTHORIZED            (-2)     /* Bad user/passwd or unauthorized */
#define NS_FORBIDDEN               (-3)     /* Authorization is not possible */
/* NS_ERROR                                    The authorization function failed */

/*
 * The following are valid options when manipulating
 * URL specific data.
 */

#define NS_OP_NOINHERIT            0x02 /* Match URL exactly */
#define NS_OP_NODELETE             0x04 /* Do call previous procs Ns_OpDeleteProc */
#define NS_OP_RECURSE              0x08 /* Also destroy registered procs below given URL */

/*
 * The following types of filters may be registered.
 */

#define NS_FILTER_PRE_AUTH         0x01 /* Runs before any Ns_UserAuthProc */
#define NS_FILTER_POST_AUTH        0x02 /* Runs after any Ns_UserAuthProc */
#define NS_FILTER_TRACE            0x04 /* Runs after Ns_OpProc completes successfully */
#define NS_FILTER_VOID_TRACE       0x08 /* Run ns_register_trace procs after previous traces */
#define NS_FILTER_FIRST            0x10 /* Register filter at head of queue. */

/*
 * The following are valid return codes from an Ns_FilterProc.
 */

/* NS_OK                                    Run next filter */
#define NS_FILTER_BREAK            (-4)  /* Run next stage of connection */
#define NS_FILTER_RETURN           (-5)  /* Close connection */

/*
 * The following are the valid attributes of a scheduled event.
 */

#define NS_SCHED_THREAD            0x01 /* Ns_SchedProc will run in detached thread */
#define NS_SCHED_ONCE              0x02 /* Call cleanup proc after running once */
#define NS_SCHED_DAILY             0x04 /* Event is scheduled to occur daily */
#define NS_SCHED_WEEKLY            0x08 /* Event is scheduled to occur weekly */
#define NS_SCHED_PAUSED            0x10 /* Event is currently paused */
#define NS_SCHED_RUNNING           0x20 /* Event is currently running, perhaps in detached thread */

/*
 * The following define socket events for the Ns_Sock* APIs.
 */

#define NS_SOCK_READ               0x01 /* Socket is readable */
#define NS_SOCK_WRITE              0x02 /* Socket is writeable */
#define NS_SOCK_EXCEPTION          0x04 /* Socket has OOB data */
#define NS_SOCK_EXIT               0x08 /* The server is shutting down */
#define NS_SOCK_DROP               0x10 /* Unused */
#define NS_SOCK_CANCEL             0x20 /* Remove event from sock callback thread */
#define NS_SOCK_TIMEOUT            0x40 /* Timeout waiting for socket event. */
#define NS_SOCK_INIT               0x80 /* Initialise a Task callback. */
#define NS_SOCK_ANY                (NS_SOCK_READ|NS_SOCK_WRITE|NS_SOCK_EXCEPTION)

/*
 * The following are valid comm driver options.
 */

#define NS_DRIVER_ASYNC            0x01 /* Use async read-ahead. */
#define NS_DRIVER_SSL              0x02 /* Use SSL port, protocol defaults. */
#define NS_DRIVER_UDP              0x04 /* Listening on a UDP socket */
#define NS_DRIVER_UNIX             0x08 /* Listening on a Unix domain socket */
#define NS_DRIVER_QUEUE_ONACCEPT   0x10 /* Queue socket on accept */
#define NS_DRIVER_QUEUE_ONREAD     0x20 /* Queue socket on first network read */

#define NS_DRIVER_VERSION_1        1

/*
 * The following are valid Tcl interp traces.
 */

#define NS_TCL_TRACE_CREATE        0x01 /* New interp created */
#define NS_TCL_TRACE_DELETE        0x02 /* Interp destroyed */
#define NS_TCL_TRACE_ALLOCATE      0x04 /* Interp allocated, possibly from thread cache */
#define NS_TCL_TRACE_DEALLOCATE    0x08 /* Interp de-allocated, returned to thread-cache */
#define NS_TCL_TRACE_GETCONN       0x10 /* Interp allocated for connection processing (filter, proc) */
#define NS_TCL_TRACE_FREECONN      0x20 /* Interp finnished connection processing */

/*
 * The following define some buffer sizes and limits.
 */

#define NS_CONN_MAXCLS             16  /* Max num CLS keys which may be allocated */
#define NS_CONN_MAXBUFS            16  /* Max num buffers which Ns_ConnSend will write */
#define NS_ENCRYPT_BUFSIZE         128 /* Min size of buffer for Ns_Encrypt output */


#if defined(__alpha)
typedef long			ns_int64;
typedef unsigned long		ns_uint64;
#define NS_INT_64_FORMAT_STRING "%ld"
#elif defined(_WIN32)
typedef int			mode_t;  /* Bug: #703061 */ 
typedef __int64			ns_int64;
typedef unsigned __int64	ns_uint64;
#define NS_INT_64_FORMAT_STRING "%I64d"
#else
typedef long long 		ns_int64;
typedef unsigned long long	ns_uint64;
#define NS_INT_64_FORMAT_STRING "%lld"

#endif

typedef ns_int64 INT64;

/*
 * The following flags define how Ns_Set's are managed by Tcl.
 */

#define NS_TCL_SET_STATIC          0 /* Ns_Set managed elsewhere, maintain a Tcl reference */
#define NS_TCL_SET_DYNAMIC         1 /* Tcl owns the Ns_Set and will free when finnished */
#define NS_TCL_SET_SHARED          2 /* Ns_Set will be shared with all interps (deprecated, see: nsv) */

/*
 * Backwards compatible (and confusing) names.
 */

#define NS_TCL_SET_PERSISTENT      NS_TCL_SET_SHARED
#define NS_TCL_SET_TEMPORARY       NS_TCL_SET_STATIC

#define NS_CACHE_FREE ns_free

#ifdef _WIN32
NS_EXTERN char *        NsWin32ErrMsg(int err);
NS_EXTERN SOCKET        ns_sockdup(SOCKET sock);
NS_EXTERN int           ns_socknbclose(SOCKET sock);
NS_EXTERN int           truncate(char *file, off_t size);
NS_EXTERN int           link(char *from, char *to);
NS_EXTERN int           symlink(char *from, char *to);
NS_EXTERN int           kill(int pid, int sig);
#define ns_sockclose    closesocket
#define ns_sockioctl    ioctlsocket
#define ns_sockerrno    GetLastError()
#define ns_sockstrerror NsWin32ErrMsg
#define strcasecmp      _stricmp
#define strncasecmp     _strnicmp
#define vsnprintf       _vsnprintf
#define snprintf        _snprintf
#define mkdir(d,m)      _mkdir((d))
#define ftruncate(f,s)  chsize((f),(s))
#define EINPROGRESS     WSAEINPROGRESS
#define EWOULDBLOCK     WSAEWOULDBLOCK
#define F_OK            0
#define W_OK            2
#define R_OK            4
#define X_OK            R_OK

#else

#define O_TEXT          0
#define O_BINARY        0
#define SOCKET          int
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)
#define NS_EXPORT
#define ns_sockclose    close
#define ns_socknbclose  close
#define ns_sockioctl    ioctl
#define ns_sockerrno    errno
#define ns_sockstrerror strerror
#define ns_sockdup      dup
#endif

/*
 * C API macros.
 */

#define UCHAR(c)                ((unsigned char)(c))
#define STREQ(a,b)              (((*a) == (*b)) && (strcmp((a),(b)) == 0))
#define STRIEQ(a,b)             (strcasecmp((a),(b)) == 0)
#define Ns_IndexCount(X)        ((X)->n)
#define Ns_ListPush(elem,list)  ((list)=Ns_ListCons((elem),(list)))
#define Ns_ListFirst(list)      ((list)->first)
#define Ns_ListRest(list)       ((list)->rest)
#define Ns_SetSize(s)           ((s)->size)
#define Ns_SetName(s)           ((s)->name)
#define Ns_SetKey(s,i)          ((s)->fields[(i)].name)
#define Ns_SetValue(s,i)        ((s)->fields[(i)].value)
#define Ns_SetLast(s)           (((s)->size)-1)

/*
 * Ns_DString's are now equivalent to Tcl_DString's starting in 4.0.
 */

#define Ns_DString              Tcl_DString
#define Ns_DStringLength        Tcl_DStringLength
#define Ns_DStringValue         Tcl_DStringValue
#define Ns_DStringNAppend       Tcl_DStringAppend
#define Ns_DStringAppend(d,s)   Tcl_DStringAppend((d), (s), -1)
#define Ns_DStringAppendElement Tcl_DStringAppendElement
#define Ns_DStringInit          Tcl_DStringInit
#define Ns_DStringFree          Tcl_DStringFree
#define Ns_DStringTrunc         Tcl_DStringTrunc
#define Ns_DStringSetLength     Tcl_DStringSetLength
#define NS_DSTRING_STATIC_SIZE  TCL_DSTRING_STATIC_SIZE
#define NS_DSTRING_PRINTF_MAX   2048

/*
 * Typedefs of variables
 */

typedef struct Ns_CacheSearch {
    Ns_Time        now;
    Tcl_HashSearch hsearch;
} Ns_CacheSearch;

typedef struct _Ns_Cache        *Ns_Cache;
typedef struct _Ns_Entry        *Ns_Entry;
typedef struct _Ns_Cls          *Ns_Cls;
typedef void                    *Ns_OpContext;
typedef struct _Ns_TaskQueue    *Ns_TaskQueue;
typedef struct _Ns_Task         *Ns_Task;
typedef struct _Ns_EventQueue   *Ns_EventQueue;
typedef struct _Ns_Event        *Ns_Event;

/*
 * This is used for logging messages.
 */

typedef enum {
    Notice,
    Warning,
    Error,
    Fatal,
    Bug,
    Debug,
    Dev
} Ns_LogSeverity;

/*
 * The following enum lists the possible HTTP headers
 * conversion options (default: Preserve).
 */

typedef enum {
    Preserve, ToLower, ToUpper
} Ns_HeaderCaseDisposition;

/*
 * Typedefs of functions
 */

typedef int   (Ns_IndexCmpProc) (const void *, const void *);
typedef int   (Ns_SortProc) (void *, void *);
typedef int   (Ns_EqualProc) (void *, void *);
typedef void  (Ns_ElemVoidProc) (void *);
typedef void *(Ns_ElemValProc) (void *);
typedef int   (Ns_ElemTestProc) (void *);
typedef void  (Ns_Callback) (void *arg);
typedef void  (Ns_ShutdownProc) (Ns_Time *toPtr, void *arg);
typedef int   (Ns_TclInterpInitProc) (Tcl_Interp *interp, void *arg);
typedef int   (Ns_TclTraceProc) (Tcl_Interp *interp, void *arg);
typedef void  (Ns_TclDeferProc) (Tcl_Interp *interp, void *arg);
typedef int   (Ns_SockProc) (SOCKET sock, void *arg, int why);
typedef void  (Ns_TaskProc) (Ns_Task *task, SOCKET sock, void *arg, int why);
typedef void  (Ns_EventProc) (Ns_Event *event, SOCKET sock, void *arg, Ns_Time *now, int why);
typedef void  (Ns_SchedProc) (void *arg, int id);
typedef int   (Ns_ServerInitProc) (char *server);
typedef int   (Ns_ModuleInitProc) (CONST char *server, CONST char *module);
typedef int   (Ns_RequestAuthorizeProc) (char *server, char *method,
			char *url, char *user, char *pass, char *peer);
typedef void  (Ns_AdpParserProc)(Ns_DString *outPtr, char *page);
typedef int   (Ns_UserAuthorizeProc) (char *user, char *passwd);
struct Ns_ObjvSpec;
typedef int   (Ns_ObjvProc) (struct Ns_ObjvSpec *spec, Tcl_Interp *interp,
                             int *objcPtr, Tcl_Obj *CONST objv[]);


/*
 * The field of a key-value data structure.
 */

typedef struct Ns_SetField {
    char *name;
    char *value;
} Ns_SetField;

/*
 * The key-value data structure.
 */

typedef struct Ns_Set {
    char        *name;
    int          size;
    int          maxSize;
    Ns_SetField *fields;
} Ns_Set;

/*
 * The request structure.
 */

typedef struct Ns_Request {
    char           *line;
    char           *method;
    char           *protocol;
    char           *host;
    unsigned short  port;
    char           *url;
    char           *query;
    int             urlc;
    char          **urlv;
    double          version;
    char           *versionstring;
} Ns_Request;

/*
 * The connection structure.
 */

typedef struct Ns_Conn {
    Ns_Request *request;
    Ns_Set     *headers;
    Ns_Set     *outputheaders;
    char       *authUser;
    char       *authPasswd;
    int         contentLength;
    int         flags;		/* Currently, only NS_CONN_CLOSED. */
} Ns_Conn;

/*
 * The index data structure.  This is a linear array of values.
 */

typedef struct Ns_Index {
    void            **el;
    Ns_IndexCmpProc  *CmpEls;
    Ns_IndexCmpProc  *CmpKeyWithEl;
    int               n;
    int               max;
    int               inc;
} Ns_Index;

/*
 * A linked list data structure.
 */

typedef struct Ns_List {
    void           *first;
    float           weight;   /* Between 0 and 1 */
    struct Ns_List *rest;
} Ns_List;

/*
 * The following struct describes how to process an option
 * or argument passed to a Tcl command.
 */

typedef struct Ns_ObjvSpec {
    char            *key;
    Ns_ObjvProc     *proc;
    void            *dest;
    void            *arg;
} Ns_ObjvSpec;

/*
 * The following struct is used to validate options from
 * a choice of values.
 */

typedef struct Ns_ObjvTable {
    char            *key;
    int              value;
} Ns_ObjvTable;

/*
 * The following structure defines the Tcl code to run
 * for a callback function.
 */

typedef struct Ns_TclCallback {
    void           *cbProc;
    CONST char     *server;
    char           *script;
    int             argc;
    char          **argv;
} Ns_TclCallback;

#ifdef _WIN32

/*
 * Windows does not have this declared.
 * The defined value has no meaning.
 * It just have to exist and it needs
 * to be accepted by any strerror() call. 
 */

#ifndef ETIMEDOUT
#define ETIMEDOUT 1
#endif

/*
 * The following structure defines an I/O
 * scatter/gather buffer for WIN32.
 */

struct iovec {
    u_long      iov_len;     /* the length of the buffer */
    char FAR *  iov_base;    /* the pointer to the buffer */
};

/*
 * The following is for supporting
 * our own poll() emulation.
 */

#define POLLIN  0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
struct pollfd {
    int            fd;
    unsigned short events;
    unsigned short revents;
};

#endif /* _WIN32 */

/*
 * The following structure defines a driver.
 */

typedef struct Ns_Driver {
    void    *arg;           /* Driver callback data. */
    char    *server;        /* Virtual server name. */
    char    *module;        /* Driver module. */
    char    *name;          /* Driver name. */
    char    *location;      /* Location, e.g, "http://foo:9090" */
    char    *address;       /* Address in location, e.g. "foo" */
    char    *protocol;      /* Protocol in location, e.g, "http" */
    int      sendwait;      /* send() I/O timeout in seconds */
    int      recvwait;      /* recv() I/O timeout in seconds */
    int      bufsize;       /* Conn bufsize (0 for SSL) */
    int      sndbuf;        /* setsockopt() SNDBUF option. */
    int      rcvbuf;        /* setsockopt() RCVBUF option. */
} Ns_Driver;

/*
 * The following structure defins the public
 * parts of the driver socket connection.
 */

typedef struct Ns_Sock {
    Ns_Driver *driver;
    struct sockaddr_in sa;
    SOCKET sock;
    void  *arg;
} Ns_Sock;

/*
 * The following enum defines the commands which
 * the socket driver proc must handle.
 */

typedef enum {
    DriverRecv,
    DriverSend,
    DriverKeep,
    DriverClose,
    DriverQueue
} Ns_DriverCmd;

/*
 * The following typedef defines a socket driver
 * callback.
 */

typedef int (Ns_DriverProc)(Ns_DriverCmd cmd, Ns_Sock *sock,
			    struct iovec *bufs, int nbufs);

/*
 * The following structure defines the values to initialize the driver. This is
 * passed to Ns_DriverInit.
 */

typedef struct Ns_DriverInitData {
    int            version;      /* Currently only version 1 exists */
    char          *name;         /* This will show up in log file entries */
    Ns_DriverProc *proc;
    int            opts;
    void          *arg;          /* Module's driver callback data */
    char          *path;         /* Path to find port, address, etc. */
} Ns_DriverInitData;

/*
 * More typedefs of functions 
 */

typedef void (Ns_ArgProc)
    (Tcl_DString *dsPtr, void *arg);

typedef int (Ns_OpProc)
    (void *arg, Ns_Conn *conn);

typedef void (Ns_TraceProc)
    (void *arg, Ns_Conn *conn);

typedef int (Ns_FilterProc)
    (void *arg, Ns_Conn *conn, int why);

typedef int (Ns_LogFilter)
    (void *arg, Ns_LogSeverity severity, Ns_Time *time, char *msg, int len);

typedef int (Ns_UrlToFileProc)
    (Ns_DString *dsPtr, CONST char *server, CONST char *url);

typedef int (Ns_Url2FileProc) 
    (Ns_DString *dsPtr, CONST char *url, void *arg);

typedef char* (Ns_ServerRootProc)
    (Ns_DString  *dest, CONST char *host, void *arg);

typedef char* (Ns_ConnLocationProc)
    (Ns_Conn *conn, Ns_DString *dest, void *arg);

typedef int (Ns_LogProc)               /* Deprecated */
    (Ns_DString *dsPtr, Ns_LogSeverity severity, CONST char *fmt, va_list ap);

typedef int (Ns_LogFlushProc)          /* Deprecated */
    (CONST char *msg, size_t len);

typedef char *(Ns_LocationProc)        /* Deprecated */
    (Ns_Conn *conn);

/*
 * adpparse.c:
 */

NS_EXTERN int
Ns_AdpRegisterParser(char *extension, Ns_AdpParserProc *proc);

/*
 * adprequest.c:
 */

NS_EXTERN int
Ns_AdpRequest(Ns_Conn *conn, CONST char *file)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * auth.c:
 */

NS_EXTERN int Ns_AuthorizeRequest(char *server, char *method, char *url,
			       char *user, char *passwd, char *peer);
NS_EXTERN void Ns_SetRequestAuthorizeProc(char *server,
    				       Ns_RequestAuthorizeProc *procPtr);
NS_EXTERN void Ns_SetUserAuthorizeProc(Ns_UserAuthorizeProc *procPtr);
NS_EXTERN int  Ns_AuthorizeUser(char *user, char *passwd);

/*
 * cache.c:
 */

NS_EXTERN Ns_Cache *
Ns_CacheCreate(CONST char *name, int keys, time_t ttl, Ns_Callback *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Cache *
Ns_CacheCreateSz(CONST char *name, int keys, size_t maxSize, Ns_Callback *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Cache *
Ns_CacheCreateEx(CONST char *name, int keys, time_t ttl, size_t maxSize,
                 Ns_Callback *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheDestroy(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Entry *
Ns_CacheFindEntry(Ns_Cache *cache, CONST char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Entry *
Ns_CacheCreateEntry(Ns_Cache *cache, CONST char *key, int *newPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_Entry *
Ns_CacheWaitCreateEntry(Ns_Cache *cache, CONST char *key, int *newPtr, 
                        Ns_Time *timeoutPtr) NS_GNUC_NONNULL(1) 
     NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN char *
Ns_CacheKey(Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN void *
Ns_CacheGetValue(Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_CacheGetSize(Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Time *
Ns_CacheGetExpirey(Ns_Entry *entry);

NS_EXTERN void
Ns_CacheSetValue(Ns_Entry *entry, void *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CacheSetValueSz(Ns_Entry *entry, void *value, size_t size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CacheSetValueExpires(Ns_Entry *entry, void *value, size_t size, 
                        Ns_Time *timeoutPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheUnsetValue(Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheDeleteEntry(Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheFlushEntry(Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Entry *
Ns_CacheFirstEntry(Ns_Cache *cache, Ns_CacheSearch *search)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Entry *
Ns_CacheNextEntry(Ns_CacheSearch *search)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_CacheFlush(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheLock(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_CacheTryLock(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheUnlock(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheWait(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_CacheTimedWait(Ns_Cache *cache, Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheSignal(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheBroadcast(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN char*
Ns_CacheStats(Ns_Cache *cache, Ns_DString *dest)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CacheResetStats(Ns_Cache *cache);

/*
 * callbacks.c:
 */

NS_EXTERN void *Ns_RegisterAtPreStartup(Ns_Callback *proc, void *arg);
NS_EXTERN void *Ns_RegisterAtStartup(Ns_Callback *proc, void *arg);
NS_EXTERN void *Ns_RegisterAtSignal(Ns_Callback *proc, void *arg);
NS_EXTERN void *Ns_RegisterAtReady(Ns_Callback *proc, void *arg);
NS_EXTERN void *Ns_RegisterAtShutdown(Ns_ShutdownProc *proc, void *arg);
NS_EXTERN void *Ns_RegisterAtExit(Ns_Callback *proc, void *arg);

/*
 * cls.c:
 */

NS_EXTERN void Ns_ClsAlloc(Ns_Cls *clsPtr, Ns_Callback *proc);
NS_EXTERN void *Ns_ClsGet(Ns_Cls *clsPtr, Ns_Conn *conn);
NS_EXTERN void Ns_ClsSet(Ns_Cls *clsPtr, Ns_Conn *conn, void *data);

/*
 * compress.c:
 */

NS_EXTERN int Ns_Compress(const char *buf, int len, Tcl_DString *outPtr, int level)
     NS_GNUC_DEPRECATED;
NS_EXTERN int Ns_CompressGzip(const char *buf, int len, Tcl_DString *outPtr, int level);

/*
 * config.c:
 */

NS_EXTERN CONST char *
Ns_ConfigString(CONST char *section, CONST char *key, CONST char *def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigBool(CONST char *section, CONST char *key, int def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigInt(CONST char *section, CONST char *key, int def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigIntRange(CONST char *section, CONST char *key, int def,
                  int min, int max)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_ConfigGetValue(CONST char *section, CONST char *key)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_ConfigGetValueExact(CONST char *section, CONST char *key)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigGetInt(CONST char *section, CONST char *key, int *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConfigGetInt64(CONST char *section, CONST char *key, ns_int64 *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConfigGetBool(CONST char *section, CONST char *key, int *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN char *
Ns_ConfigGetPath(CONST char *server, CONST char *module, ...)
     NS_GNUC_SENTINEL;

NS_EXTERN Ns_Set **
Ns_ConfigGetSections(void);

NS_EXTERN Ns_Set *
Ns_ConfigGetSection(CONST char *section);

NS_EXTERN void
Ns_GetVersion(int *major, int *minor, int *patch, int *type);

/*
 * conn.c:
 */

NS_EXTERN int Ns_ConnContentFd(Ns_Conn *conn);
NS_EXTERN void Ns_ConnSetEncoding(Ns_Conn *conn, Tcl_Encoding encoding);
NS_EXTERN Tcl_Encoding Ns_ConnGetEncoding(Ns_Conn *conn);
NS_EXTERN void Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding);
NS_EXTERN Tcl_Encoding Ns_ConnGetUrlEncoding(Ns_Conn *conn);
NS_EXTERN int Ns_ConnModifiedSince(Ns_Conn *conn, time_t inTime);
NS_EXTERN int Ns_ParseHeader(Ns_Set *set, char *header, Ns_HeaderCaseDisposition disp);
NS_EXTERN Ns_Set  *Ns_ConnGetQuery(Ns_Conn *conn);
NS_EXTERN void Ns_ConnClearQuery(Ns_Conn *conn);
NS_EXTERN int Ns_QueryToSet(char *query, Ns_Set *qset);
NS_EXTERN Ns_Set *Ns_ConnHeaders(Ns_Conn *conn);
NS_EXTERN Ns_Set *Ns_ConnOutputHeaders(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnAuthUser(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnAuthPasswd(Ns_Conn *conn);
NS_EXTERN int Ns_ConnContentLength(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnContent(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnServer(Ns_Conn *conn);
NS_EXTERN int Ns_ConnResponseStatus(Ns_Conn *conn);
NS_EXTERN void Ns_ConnSetResponseStatus(Ns_Conn *conn, int new_status);
NS_EXTERN char *Ns_ConnResponseVersion(Ns_Conn *conn);
NS_EXTERN void Ns_ConnSetResponseVersion(Ns_Conn *conn, char *new_version);
NS_EXTERN int Ns_ConnContentSent(Ns_Conn *conn);
NS_EXTERN void Ns_ConnSetContentSent(Ns_Conn *conn, int length);
NS_EXTERN int Ns_ConnResponseLength(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnPeer(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnSetPeer(Ns_Conn *conn, struct sockaddr_in *saPtr);
NS_EXTERN int Ns_ConnPeerPort(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnLocation(Ns_Conn *conn) NS_GNUC_DEPRECATED;
NS_EXTERN char *Ns_ConnLocationAppend(Ns_Conn *conn, Ns_DString *dest);
NS_EXTERN char *Ns_ConnHost(Ns_Conn *conn);
NS_EXTERN int Ns_ConnPort(Ns_Conn *conn);
NS_EXTERN int Ns_ConnSock(Ns_Conn *conn);
NS_EXTERN Ns_Sock *Ns_ConnSockPtr(Ns_Conn *conn);
NS_EXTERN Ns_DString *Ns_ConnSockContent(Ns_Conn *conn);
NS_EXTERN char *Ns_ConnDriverName(Ns_Conn *conn);
NS_EXTERN void *Ns_ConnDriverContext(Ns_Conn *conn);
NS_EXTERN int Ns_ConnGetWriteEncodedFlag(Ns_Conn *conn);
NS_EXTERN void Ns_ConnSetWriteEncodedFlag(Ns_Conn *conn, int flag);
NS_EXTERN int Ns_ConnGetChunkedFlag(Ns_Conn *conn);
NS_EXTERN void Ns_ConnSetChunkedFlag(Ns_Conn *conn, int flag);
NS_EXTERN void Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding);
NS_EXTERN int Ns_SetConnLocationProc(Ns_ConnLocationProc *proc, void *arg);
NS_EXTERN void Ns_SetLocationProc(char *server, Ns_LocationProc *proc) NS_GNUC_DEPRECATED;
NS_EXTERN Ns_Time *Ns_ConnStartTime(Ns_Conn *conn);
NS_EXTERN Ns_Time *Ns_ConnTimeout(Ns_Conn *conn) NS_GNUC_NONNULL(1);

/*
 * connio.c:
 */

NS_EXTERN int
Ns_ConnInit(Ns_Conn *connPtr)
    NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_ConnClose(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnSend(Ns_Conn *conn, struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWrite(Ns_Conn *conn, CONST void *buf, int towrite)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteV(Ns_Conn *conn, struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnWriteChars(Ns_Conn *conn, CONST char *buf, int towrite)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteVChars(Ns_Conn *conn, struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_WriteConn(Ns_Conn *conn, CONST char *buf, int towrite)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_WriteCharConn(Ns_Conn *conn, CONST char *buf, int towrite)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnPuts(Ns_Conn *conn, CONST char *string)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendDString(Ns_Conn *conn, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendChannel(Ns_Conn *conn, Tcl_Channel chan, int nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendFp(Ns_Conn *conn, FILE *fp, int nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendFd(Ns_Conn *conn, int fd, int nsend)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnFlushContent(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_ConnGets(char *outBuffer, size_t inSize, Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnRead(Ns_Conn *conn, void *vbuf, int toread)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReadLine(Ns_Conn *conn, Ns_DString *dsPtr, int *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReadHeaders(Ns_Conn *conn, Ns_Set *set, int *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnCopyToDString(Ns_Conn *conn, size_t ncopy, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnCopyToChannel(Ns_Conn *conn, size_t ncopy, Tcl_Channel chan)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnCopyToFile(Ns_Conn *conn, size_t ncopy, FILE *fp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnCopyToFd(Ns_Conn *conn, size_t ncopy, int fd)
    NS_GNUC_NONNULL(1);

/*
 * cookies.c:
 */

NS_EXTERN void Ns_ConnSetCookie(Ns_Conn *conn,  char *name, char *value, int maxage);
NS_EXTERN void Ns_ConnSetSecureCookie(Ns_Conn *conn,  char *name, char *value, int maxage);
NS_EXTERN void Ns_ConnSetCookieEx(Ns_Conn *conn,  char *name, char *value, int maxage,
                                  char *domain, char *path, int secure);
NS_EXTERN void Ns_ConnDeleteCookie(Ns_Conn *conn, char *name, char *domain, char *path);
NS_EXTERN void Ns_ConnDeleteSecureCookie(Ns_Conn *conn, char *name, char *domain, char *path);
NS_EXTERN char *Ns_ConnGetCookie(Ns_DString *dest, Ns_Conn *conn, char *name);

/*
 * crypt.c:
 */

NS_EXTERN char *Ns_Encrypt(char *pw, char *salt, char iobuf[ ]);

/*
 * dns.c:
 */

NS_EXTERN int Ns_GetHostByAddr(Ns_DString *dsPtr, char *addr);
NS_EXTERN int Ns_GetAddrByHost(Ns_DString *dsPtr, char *host);
NS_EXTERN int Ns_GetAllAddrByHost(Ns_DString *dsPtr, char *host);

/*
 * driver.c:
 */

NS_EXTERN int Ns_DriverInit(char *server, char *module, Ns_DriverInitData *init);
NS_EXTERN int Ns_DriverSetRequest(Ns_Sock *sock, char *reqline);

/*
 * dsprintf.c:
 */

NS_EXTERN char *
Ns_DStringVPrintf(Ns_DString *dsPtr, CONST char *fmt, va_list ap)
     NS_GNUC_NONNULL(2);

/*
 * dstring.c:
 */

NS_EXTERN char *
Ns_DStringVarAppend(Ns_DString *dsPtr, ...)
     NS_GNUC_NONNULL(1) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_DStringExport(Ns_DString *dsPtr)
     NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_DStringAppendArg(Ns_DString *dsPtr, CONST char *string)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DStringPrintf(Ns_DString *dsPtr, CONST char *fmt, ...)
     NS_GNUC_NONNULL(1) NS_GNUC_PRINTF(2,3);

NS_EXTERN char **
Ns_DStringAppendArgv(Ns_DString *dsPtr)
     NS_GNUC_NONNULL(1);

NS_EXTERN Ns_DString *
Ns_DStringPop(void)
     NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_DStringPush(Ns_DString *dsPtr)
     NS_GNUC_DEPRECATED;

/*
 * event.c
 */

NS_EXTERN Ns_EventQueue *
Ns_CreateEventQueue(int maxevents);

NS_EXTERN int
Ns_EventEnqueue(Ns_EventQueue *queue, SOCKET sock, Ns_EventProc *proc, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_EventCallback(Ns_Event *event, int when, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_RunEventQueue(Ns_EventQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TriggerEventQueue(Ns_EventQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ExitEventQueue(Ns_EventQueue *queue)
    NS_GNUC_NONNULL(1);
    

/*
 * exec.c:
 */

NS_EXTERN int Ns_ExecProcess(char *exec, char *dir, int fdin, int fdout,
			  char *args, Ns_Set *env);
NS_EXTERN int Ns_ExecProc(char *exec, char **argv);
NS_EXTERN int Ns_ExecArgblk(char *exec, char *dir, int fdin, int fdout,
			 char *args, Ns_Set *env);
NS_EXTERN int Ns_ExecArgv(char *exec, char *dir, int fdin, int fdout, char **argv,
		       Ns_Set *env);
NS_EXTERN int Ns_WaitProcess(int pid);
NS_EXTERN int Ns_WaitForProcess(int pid, int *statusPtr);

/*
 * fastpath.c:
 */

NS_EXTERN int
Ns_ConnReturnFile(Ns_Conn *conn, int status, CONST char *type,
                  CONST char *file);

NS_EXTERN CONST char *
Ns_PageRoot(CONST char *server)
    NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_UrlIsFile(CONST char *server, CONST char *url);

NS_EXTERN int
Ns_UrlIsDir(CONST char *server, CONST char *url);

NS_EXTERN Ns_OpProc Ns_FastPathProc;

/*
 * filter.c:
 */

NS_EXTERN void *Ns_RegisterFilter(char *server, char *method, char *URL,
			       Ns_FilterProc *proc, int when, void *args);
NS_EXTERN void *Ns_RegisterServerTrace(char *server, Ns_TraceProc *proc, void *arg);
NS_EXTERN void *Ns_RegisterConnCleanup(char *server, Ns_TraceProc *proc, void *arg);
NS_EXTERN void *Ns_RegisterCleanup(Ns_TraceProc *proc, void *arg);

/*
 * htuu.c
 */

NS_EXTERN int Ns_HtuuEncode(unsigned char *string, unsigned int bufsize,
			 char *buf);
NS_EXTERN int Ns_HtuuDecode(char *string, unsigned char *buf, int bufsize);

/*
 * index.c:
 */

NS_EXTERN void Ns_IndexInit(Ns_Index *indexPtr, int inc,
			 int (*CmpEls) (const void *, const void *),
			 int (*CmpKeyWithEl) (const void *, const void *));
NS_EXTERN void Ns_IndexTrunc(Ns_Index*indexPtr);
NS_EXTERN void Ns_IndexDestroy(Ns_Index *indexPtr);
NS_EXTERN Ns_Index *Ns_IndexDup(Ns_Index *indexPtr);
NS_EXTERN void *Ns_IndexFind(Ns_Index *indexPtr, void *key);
NS_EXTERN void *Ns_IndexFindInf(Ns_Index *indexPtr, void *key);
NS_EXTERN void **Ns_IndexFindMultiple(Ns_Index *indexPtr, void *key);
NS_EXTERN void Ns_IndexAdd(Ns_Index *indexPtr, void *el);
NS_EXTERN void Ns_IndexDel(Ns_Index *indexPtr, void *el);
NS_EXTERN void *Ns_IndexEl(Ns_Index *indexPtr, int i);
NS_EXTERN void Ns_IndexStringInit(Ns_Index *indexPtr, int inc);
NS_EXTERN Ns_Index *Ns_IndexStringDup(Ns_Index *indexPtr);
NS_EXTERN void Ns_IndexStringAppend(Ns_Index *addtoPtr, Ns_Index *addfromPtr);
NS_EXTERN void Ns_IndexStringDestroy(Ns_Index *indexPtr);
NS_EXTERN void Ns_IndexStringTrunc(Ns_Index *indexPtr);
NS_EXTERN void Ns_IndexIntInit(Ns_Index *indexPtr, int inc);
/*
 * see macros above for:
 *
 * Ns_IndexCount(X) 
 */

/*
 * lisp.c:
 */

NS_EXTERN Ns_List *Ns_ListNconc(Ns_List *l1Ptr, Ns_List *l2Ptr);
NS_EXTERN Ns_List *Ns_ListCons(void *elem, Ns_List *lPtr);
NS_EXTERN Ns_List *Ns_ListNreverse(Ns_List *lPtr);
NS_EXTERN Ns_List *Ns_ListLast(Ns_List *lPtr);
NS_EXTERN void Ns_ListFree(Ns_List *lPtr, Ns_ElemVoidProc *freeProc);
NS_EXTERN void Ns_IntPrint(int d);
NS_EXTERN void Ns_StringPrint(char *s);
NS_EXTERN void Ns_ListPrint(Ns_List *lPtr, Ns_ElemVoidProc *printProc);
NS_EXTERN Ns_List *Ns_ListCopy(Ns_List *lPtr);
NS_EXTERN int Ns_ListLength(Ns_List *lPtr);
NS_EXTERN Ns_List *Ns_ListWeightSort(Ns_List *wPtr);
NS_EXTERN Ns_List *Ns_ListSort(Ns_List *wPtr, Ns_SortProc *sortProc);
NS_EXTERN Ns_List *Ns_ListDeleteLowElements(Ns_List *mPtr, float minweight);
NS_EXTERN Ns_List *Ns_ListDeleteWithTest(void *elem, Ns_List *lPtr,
				      Ns_EqualProc *equalProc);
NS_EXTERN Ns_List *Ns_ListDeleteIf(Ns_List *lPtr, Ns_ElemTestProc *testProc);
NS_EXTERN Ns_List *Ns_ListDeleteDuplicates(Ns_List *lPtr,
				        Ns_EqualProc *equalProc);
NS_EXTERN Ns_List *Ns_ListNmapcar(Ns_List *lPtr, Ns_ElemValProc *valProc);
NS_EXTERN Ns_List *Ns_ListMapcar(Ns_List *lPtr, Ns_ElemValProc *valProc);
/*
 * see macros above for:
 *
 * Ns_ListPush(elem,list)
 * Ns_ListFirst(list)
 * Ns_ListRest(list)
 */

/*
 * rand.c:
 */

NS_EXTERN void Ns_GenSeeds(unsigned long *seedsPtr, int nseeds);
NS_EXTERN double Ns_DRand(void);

/* 
 * task.c: 
 */

NS_EXTERN Ns_TaskQueue *
Ns_CreateTaskQueue(char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_DestroyTaskQueue(Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Task *
Ns_TaskCreate(SOCKET sock, Ns_TaskProc *proc, void *arg)
    NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_TaskEnqueue(Ns_Task *task, Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_TaskRun(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskCallback(Ns_Task *task, int when, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskDone(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TaskCancel(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TaskWait(Ns_Task *task, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN SOCKET
Ns_TaskFree(Ns_Task *task)
    NS_GNUC_NONNULL(1);

/*
 * tclobj.c:
 */

NS_EXTERN void
Ns_TclResetObjType(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr);

NS_EXTERN void
Ns_TclSetTwoPtrValue(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr,
                     void *ptr1, void *ptr2);
NS_EXTERN void
Ns_TclSetOtherValuePtr(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr, void *value);

NS_EXTERN void
Ns_TclSetStringRep(Tcl_Obj *objPtr, char *bytes, int length);

NS_EXTERN int
Ns_TclGetAddrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
                     CONST char *type, void **addrPtrPtr)
     NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_TclSetAddrObj(Tcl_Obj *objPtr, CONST char *type, void *addr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_TclGetOpaqueFromObj(Tcl_Obj *objPtr, CONST char *type, void **addrPtrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_TclSetOpaqueObj(Tcl_Obj *objPtr, CONST char *type, void *addr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Tcl_SetFromAnyProc Ns_TclSetFromAnyError;

/*
 * tclobjv.c
 */

NS_EXTERN int Ns_ParseObjv(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec,
                           Tcl_Interp *interp, int offset, int objc, Tcl_Obj *CONST objv[]);
NS_EXTERN Ns_ObjvProc Ns_ObjvBool;
NS_EXTERN Ns_ObjvProc Ns_ObjvInt;
NS_EXTERN Ns_ObjvProc Ns_ObjvLong;
NS_EXTERN Ns_ObjvProc Ns_ObjvWideInt;
NS_EXTERN Ns_ObjvProc Ns_ObjvDouble;
NS_EXTERN Ns_ObjvProc Ns_ObjvString;
NS_EXTERN Ns_ObjvProc Ns_ObjvByteArray;
NS_EXTERN Ns_ObjvProc Ns_ObjvObj;
NS_EXTERN Ns_ObjvProc Ns_ObjvIndex;
NS_EXTERN Ns_ObjvProc Ns_ObjvFlags;
NS_EXTERN Ns_ObjvProc Ns_ObjvBreak;
NS_EXTERN Ns_ObjvProc Ns_ObjvArgs;
NS_EXTERN Ns_ObjvProc Ns_ObjvTime;


/*
 * tclthread.c:
 */

NS_EXTERN int Ns_TclThread(Tcl_Interp *interp, char *script, Ns_Thread *thrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN int Ns_TclDetachedThread(Tcl_Interp *interp, char *script);

/*
 * tcltime.c
 */

NS_EXTERN void Ns_TclSetTimeObj(Tcl_Obj *objPtr, Ns_Time *timePtr);
NS_EXTERN int Ns_TclGetTimeFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, 
                                   Ns_Time *timePtr);
NS_EXTERN int Ns_TclGetTimePtrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, 
                                      Ns_Time **timePtrPtr);

/*
 * tclxkeylist.c:
 */

NS_EXTERN char *Tcl_DeleteKeyedListField (Tcl_Interp  *interp, CONST char *fieldName,
        CONST char *keyedList);
NS_EXTERN int Tcl_GetKeyedListField (Tcl_Interp  *interp, CONST char *fieldName,
        CONST char *keyedList, char **fieldValuePtr);
NS_EXTERN int Tcl_GetKeyedListKeys (Tcl_Interp  *interp, char CONST *subFieldName,
        CONST char *keyedList, int *keysArgcPtr, char ***keysArgvPtr);
NS_EXTERN char *Tcl_SetKeyedListField (Tcl_Interp  *interp, CONST char *fieldName,
        CONST char *fieldvalue, CONST char *keyedList);

/*
 * listen.c:
 */

NS_EXTERN int Ns_SockListenCallback(char *addr, int port, Ns_SockProc *proc,
				 void *arg);
NS_EXTERN int Ns_SockPortBound(int port);

/*
 * log.c:
 */

NS_EXTERN char *
Ns_InfoErrorLog(void);

NS_EXTERN int
Ns_LogRoll(void);

NS_EXTERN void
Ns_Log(Ns_LogSeverity severity, CONST char *fmt, ...)
     NS_GNUC_PRINTF(2, 3);

NS_EXTERN void
Ns_VALog(Ns_LogSeverity severity, CONST char *fmt, va_list *vaPtr);

NS_EXTERN void
Ns_Fatal(CONST char *fmt, ...)
     NS_GNUC_PRINTF(1, 2) NS_GNUC_NORETURN;

NS_EXTERN char *
Ns_LogTime(char *timeBuf)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_LogTime2(char *timeBuf, int gmt)
    NS_GNUC_NONNULL(1);;

NS_EXTERN void
Ns_SetLogFlushProc(Ns_LogFlushProc *procPtr) NS_GNUC_DEPRECATED; 

NS_EXTERN void
Ns_SetNsLogProc(Ns_LogProc *procPtr)  NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_AddLogFilter(Ns_LogFilter *procPtr, void *arg, Ns_Callback *freePtr);

NS_EXTERN void
Ns_RemoveLogFilter(Ns_LogFilter *procPtr, void *arg);

/*
 * rollfile.c
 */

NS_EXTERN int
Ns_RollFile(CONST char *file, int max)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_PurgeFiles(CONST char *file, int max)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_RollFileByDate(CONST char *file, int max)
    NS_GNUC_NONNULL(1);

/*
 * nsmain.c:
 */

NS_EXTERN int Ns_Main(int argc, char **argv, Ns_ServerInitProc *initProc);
NS_EXTERN int Ns_WaitForStartup(void);

/*
 * info.c:
 */

NS_EXTERN char *Ns_InfoHomePath(void);
NS_EXTERN char *Ns_InfoServerName(void);
NS_EXTERN char *Ns_InfoServerVersion(void);
NS_EXTERN char *Ns_InfoConfigFile(void);
NS_EXTERN int Ns_InfoPid(void);
NS_EXTERN char *Ns_InfoNameOfExecutable(void);
NS_EXTERN char *Ns_InfoPlatform(void);
NS_EXTERN int Ns_InfoUptime(void);
NS_EXTERN int Ns_InfoBootTime(void);
NS_EXTERN char *Ns_InfoHostname(void);
NS_EXTERN char *Ns_InfoAddress(void);
NS_EXTERN char *Ns_InfoBuildDate(void);
NS_EXTERN int Ns_InfoShutdownPending(void);
NS_EXTERN int Ns_InfoStarted(void);
NS_EXTERN int Ns_InfoServersStarted(void);
NS_EXTERN char *Ns_InfoTag(void);

/*
 * mimetypes.c:
 */

NS_EXTERN char *
Ns_GetMimeType(CONST char *file)
    NS_GNUC_NONNULL(1);

/*
 * encoding.c:
 */

NS_EXTERN Tcl_Encoding
Ns_GetEncoding(CONST char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetFileEncoding(CONST char *file)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetTypeEncoding(CONST char *type)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetCharsetEncoding(CONST char *charset)
    NS_GNUC_NONNULL(1);

/*
 * modload.c:
 */

NS_EXTERN void
Ns_RegisterModule(CONST char *name, Ns_ModuleInitProc *proc)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ModuleLoad(CONST char *server, CONST char *module, CONST char *file,
              CONST char *init)
     NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

/*
 * nsthread.c:
 */

NS_EXTERN void Ns_SetThreadServer(char *server);
NS_EXTERN char *Ns_GetThreadServer(void);

/*
 * op.c:
 */

NS_EXTERN void
Ns_RegisterRequest(CONST char *server, CONST char *method, CONST char *url,
                   Ns_OpProc *proc, Ns_Callback *del, void *arg, int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_RegisterProxyRequest(CONST char *server, CONST char *method, CONST char *protocol,
                        Ns_OpProc *proc, Ns_Callback *del, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_GetRequest(CONST char *server, CONST char *method, CONST char *url,
              Ns_OpProc **procPtr, Ns_Callback **deletePtr, void **argPtr,
              int *flagsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6)
    NS_GNUC_NONNULL(7);

NS_EXTERN void
Ns_UnRegisterRequest(CONST char *server, CONST char *method, CONST char *url,
                     int inherit)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterProxyRequest(CONST char *server, CONST char *method,
                          CONST char *protocol)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterRequestEx(CONST char *server, CONST char *method, CONST char *url,
                       int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnRunRequest(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnRedirect(Ns_Conn *conn, CONST char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * pathname.c:
 */

NS_EXTERN int Ns_PathIsAbsolute(CONST char *path);
NS_EXTERN char *Ns_NormalizePath(Ns_DString *dsPtr, CONST char *path);
NS_EXTERN char *Ns_MakePath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;
NS_EXTERN char *Ns_HashPath(Ns_DString *dsPtr, CONST char *string, int levels);
NS_EXTERN char *Ns_LibPath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;
NS_EXTERN char *Ns_BinPath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;
NS_EXTERN char *Ns_HomePath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;
NS_EXTERN char *Ns_ModulePath(Ns_DString *dsPtr, CONST char *server,
                              CONST char *module, ...) NS_GNUC_SENTINEL;
NS_EXTERN char *Ns_ServerPath(Ns_DString *dest, CONST char *server, ...) NS_GNUC_SENTINEL;
NS_EXTERN char *Ns_PagePath(Ns_DString *dest, CONST char *server, ...) NS_GNUC_SENTINEL;
NS_EXTERN int Ns_SetServerRootProc(Ns_ServerRootProc *proc, void *arg);

/*
 * proc.c:
 */

NS_EXTERN void Ns_RegisterProcInfo(void *procAddr, char *desc, Ns_ArgProc *argProc);
NS_EXTERN void Ns_GetProcInfo(Tcl_DString *dsPtr, void *procAddr, void *arg);
NS_EXTERN void Ns_StringArgProc(Tcl_DString *dsPtr, void *arg);

/*
 * queue.c:
 */

NS_EXTERN Ns_Conn *Ns_GetConn(void);

/*
 * quotehtml.c:
 */

NS_EXTERN void Ns_QuoteHtml(Ns_DString *pds, char *string);

/*
 * request.c:
 */

NS_EXTERN void Ns_FreeRequest(Ns_Request *request);
NS_EXTERN Ns_Request *Ns_ParseRequest(CONST char *line);
NS_EXTERN char *Ns_SkipUrl(Ns_Request *request, int n);
NS_EXTERN void Ns_SetRequestUrl(Ns_Request *request, CONST char *url);

/*
 * return.c:
 */

NS_EXTERN void
Ns_RegisterReturn(int status, CONST char *url);

NS_EXTERN void
Ns_ConnConstructHeaders(Ns_Conn *conn, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnQueueHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnFlushHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetHeaders(Ns_Conn *conn, CONST char *field, CONST char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnCondSetHeaders(Ns_Conn *conn, CONST char *field, CONST char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnReplaceHeaders(Ns_Conn *conn, Ns_Set *newheaders)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnPrintfHeaders(Ns_Conn *conn, CONST char *field, CONST char *fmt, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PRINTF(3, 4);

NS_EXTERN void
Ns_ConnSetRequiredHeaders(Ns_Conn *conn, CONST char *type, int length)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetTypeHeader(Ns_Conn *conn, CONST char *type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetLengthHeader(Ns_Conn *conn, int length)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetLastModifiedHeader(Ns_Conn *conn, time_t *mtime)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetExpiresHeader(Ns_Conn *conn, CONST char *expires)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnResetReturn(Ns_Conn *conn)
    NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_ConnReturnAdminNotice(Ns_Conn *conn, int status, CONST char *title,
                         CONST char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnNotice(Ns_Conn *conn, int status, CONST char *title,
                    CONST char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnData(Ns_Conn *conn, int status, CONST char *data, int len,
                  CONST char *type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnCharData(Ns_Conn *conn, int status, CONST char *data, int len,
                      CONST char *type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnHtml(Ns_Conn *conn, int status, CONST char *html, int len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnOk(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNoResponse(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnRedirect(Ns_Conn *conn, CONST char *url)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnBadRequest(Ns_Conn *conn, CONST char *reason)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnUnauthorized(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnForbidden(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNotFound(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNotModified(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNotImplemented(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnInternalError(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnUnavailable(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnStatus(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnOpenChannel(Ns_Conn *conn, int status, CONST char *type,
                         Tcl_Channel chan, int len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnOpenFile(Ns_Conn *conn, int status, CONST char *type,
                      FILE *fp, int len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnOpenFd(Ns_Conn *conn, int status, CONST char *type, int fd, int len)
    NS_GNUC_NONNULL(1);

/*
 * sched.c:
 */

NS_EXTERN int Ns_After(int seconds, Ns_Callback *proc, void *arg,
		    Ns_Callback *deleteProc);
NS_EXTERN int Ns_Cancel(int id);
NS_EXTERN int Ns_Pause(int id);
NS_EXTERN int Ns_Resume(int id);
NS_EXTERN int Ns_ScheduleProc(Ns_Callback *proc, void *arg, int thread,
			   int interval);
NS_EXTERN int Ns_ScheduleDaily(Ns_SchedProc *proc, void *arg, int flags,
			    int hour, int minute, Ns_SchedProc *cleanupProc);
NS_EXTERN int Ns_ScheduleWeekly(Ns_SchedProc *proc, void *arg, int flags,
			     int day, int hour, int minute,
			     Ns_SchedProc *cleanupProc);
NS_EXTERN int Ns_ScheduleProcEx(Ns_SchedProc *proc, void *arg, int flags,
			     int interval, Ns_SchedProc *cleanupProc);
NS_EXTERN void Ns_UnscheduleProc(int id);

/*
 * set.c:
 */

NS_EXTERN void Ns_SetUpdate(Ns_Set *set, CONST char *key, CONST char *value);
NS_EXTERN Ns_Set *Ns_SetCreate(CONST char *name);
NS_EXTERN void Ns_SetFree(Ns_Set *set);
NS_EXTERN int Ns_SetPut(Ns_Set *set, CONST char *key, CONST char *value);
NS_EXTERN int Ns_SetUniqueCmp(Ns_Set *set, CONST char *key,
                              int (*cmp) (CONST char *s1, CONST char *s2));
NS_EXTERN int Ns_SetFindCmp(Ns_Set *set, CONST char *key,
                            int (*cmp) (CONST char *s1, CONST char *s2));
NS_EXTERN char *Ns_SetGetCmp(Ns_Set *set, CONST char *key,
                             int (*cmp) (CONST char *s1, CONST char *s2));
NS_EXTERN int Ns_SetUnique(Ns_Set *set, CONST char *key);
NS_EXTERN int Ns_SetIUnique(Ns_Set *set, CONST char *key);
NS_EXTERN int Ns_SetFind(Ns_Set *set, CONST char *key);
NS_EXTERN int Ns_SetIFind(Ns_Set *set, CONST char *key);
NS_EXTERN char *Ns_SetGet(Ns_Set *set, CONST char *key);
NS_EXTERN char *Ns_SetIGet(Ns_Set *set, CONST char *key);
NS_EXTERN void Ns_SetTrunc(Ns_Set *set, int size);
NS_EXTERN void Ns_SetDelete(Ns_Set *set, int index);
NS_EXTERN void Ns_SetPutValue(Ns_Set *set, int index, CONST char *value);
NS_EXTERN void Ns_SetDeleteKey(Ns_Set *set, CONST char *key);
NS_EXTERN void Ns_SetIDeleteKey(Ns_Set *set, CONST char *key);
NS_EXTERN Ns_Set *Ns_SetListFind(Ns_Set **sets, CONST char *name);
NS_EXTERN Ns_Set **Ns_SetSplit(Ns_Set *set, char sep);
NS_EXTERN void Ns_SetListFree(Ns_Set **sets);
NS_EXTERN void Ns_SetMerge(Ns_Set *high, Ns_Set *low);
NS_EXTERN Ns_Set *Ns_SetCopy(Ns_Set *old);
NS_EXTERN void Ns_SetMove(Ns_Set *to, Ns_Set *from);
NS_EXTERN void Ns_SetPrint(Ns_Set *set);
NS_EXTERN char *Ns_SetGetValue(Ns_Set *set, CONST char *key, CONST char *def);
NS_EXTERN char *Ns_SetIGetValue(Ns_Set *set, CONST char *key, CONST char *def);

/*
 * see macros above for:
 *
 * Ns_SetSize(s)
 * Ns_SetName(s)
 * Ns_SetKey(s,i)
 * Ns_SetValue(s,i)
 * Ns_SetLast(s)
 */

/*
 * binder.c:
 */

NS_EXTERN SOCKET Ns_SockListenEx(char *address, int port, int backlog);
NS_EXTERN SOCKET Ns_SockListenUdp(char *address, int port);
NS_EXTERN SOCKET Ns_SockListenRaw(int proto);
NS_EXTERN SOCKET Ns_SockListenUnix(char *path, int backlog, int mode);

NS_EXTERN SOCKET Ns_SockBindUdp(struct sockaddr_in *saPtr);
NS_EXTERN SOCKET Ns_SockBindRaw(int proto);
NS_EXTERN SOCKET Ns_SockBindUnix(char *path, int socktype, int mode);

NS_EXTERN void NsForkBinder(void);
NS_EXTERN void NsStopBinder(void);
NS_EXTERN SOCKET Ns_SockBinderListen(int type, char *address, int port, int options);

/*
 * sock.c:
 */

NS_EXTERN int Ns_SockWait(SOCKET sock, int what, int timeout);
NS_EXTERN int Ns_SockTimedWait(SOCKET sock, int what, Ns_Time *timeoutPtr);
NS_EXTERN int Ns_SockRecv(SOCKET sock, void *vbuf, size_t nrecv, 
                          Ns_Time *timeoutPtr);
NS_EXTERN int Ns_SockSend(SOCKET sock, void *vbuf, size_t nsend, 
                          Ns_Time *timeoutPtr);
NS_EXTERN int Ns_SockRecvBufs(SOCKET sock, struct iovec *bufs, int nbufs,
                              Ns_Time *timeoutPtr);
NS_EXTERN int Ns_SockSendBufs(SOCKET sock, struct iovec *bufs, int nbufs, 
                              Ns_Time *timeoutPtr);

NS_EXTERN SOCKET Ns_BindSock(struct sockaddr_in *psa) NS_GNUC_DEPRECATED;
NS_EXTERN SOCKET Ns_SockBind(struct sockaddr_in *psa);
NS_EXTERN SOCKET Ns_SockListen(char *address, int port);
NS_EXTERN SOCKET Ns_SockAccept(SOCKET sock, struct sockaddr *psa, int *lenPtr);

NS_EXTERN SOCKET Ns_SockConnect(char *host, int port);
NS_EXTERN SOCKET Ns_SockConnect2(char *host, int port, char *lhost, int lport);
NS_EXTERN SOCKET Ns_SockAsyncConnect(char *host, int port);
NS_EXTERN SOCKET Ns_SockAsyncConnect2(char *host, int port, char *lhost, int lport);
NS_EXTERN SOCKET Ns_SockTimedConnect(char *host, int port, Ns_Time *timeoutPtr);
NS_EXTERN SOCKET Ns_SockTimedConnect2(char *host, int port, char *lhost, int lport, 
                                      Ns_Time *timeoutPtr);

NS_EXTERN int Ns_SockSetNonBlocking(SOCKET sock);
NS_EXTERN int Ns_SockSetBlocking(SOCKET sock);
NS_EXTERN int Ns_GetSockAddr(struct sockaddr_in *psa, char *host, int port);
NS_EXTERN int Ns_SockCloseLater(SOCKET sock);

NS_EXTERN char *Ns_SockError(void);
NS_EXTERN int   Ns_SockErrno(void);
NS_EXTERN void Ns_ClearSockErrno(void);
NS_EXTERN int Ns_GetSockErrno(void);
NS_EXTERN void Ns_SetSockErrno(int err);
NS_EXTERN char *Ns_SockStrError(int err);

/*
 * sockcallback.c:
 */

NS_EXTERN int Ns_SockCallback(SOCKET sock, Ns_SockProc *proc, void *arg, int when);
NS_EXTERN int Ns_SockCallbackEx(SOCKET sock, Ns_SockProc *proc, void *arg, int when, int timeout);
NS_EXTERN void Ns_SockCancelCallback(SOCKET sock);
NS_EXTERN int Ns_SockCancelCallbackEx(SOCKET sock, Ns_SockProc *proc, void *arg);

/*
 * str.c:
 */

NS_EXTERN char *
Ns_StrTrim(char *string);

NS_EXTERN char *
Ns_StrTrimLeft(char *string);

NS_EXTERN char *
Ns_StrTrimRight(char *string);

NS_EXTERN char *
Ns_StrToLower(char *string)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_StrToUpper(char *string)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_StrToInt(CONST char *string, int *intPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN CONST char *
Ns_Match(CONST char *a, CONST char *b);

NS_EXTERN CONST char *
Ns_NextWord(CONST char *line)
    NS_GNUC_NONNULL(1);

NS_EXTERN CONST char *
Ns_StrNStr(CONST char *pattern, CONST char *expression)
    NS_GNUC_NONNULL(1);

NS_EXTERN CONST char *
Ns_StrCaseFind(CONST char *s1, CONST char *s2)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_StrIsHost(CONST char *string)
    NS_GNUC_NONNULL(1);

/*
 * tclcallbacks.c:
 */

NS_EXTERN Ns_TclCallback *Ns_TclNewCallback(Tcl_Interp *interp, void *cbProc,
                                            Tcl_Obj *scriptObjPtr, int objc,
                                            Tcl_Obj *CONST objv[]);

NS_EXTERN int Ns_TclEvalCallback(Tcl_Interp *interp, Ns_TclCallback *cbPtr,
                                 Ns_DString *result, ...) NS_GNUC_SENTINEL;

NS_EXTERN Ns_Callback Ns_TclCallbackProc;
NS_EXTERN Ns_Callback Ns_TclFreeCallback;
NS_EXTERN Ns_ArgProc  Ns_TclCallbackArgProc;

/*
 * tclenv.c:
 */

NS_EXTERN char **Ns_CopyEnviron(Ns_DString *dsPtr);
NS_EXTERN char **Ns_GetEnviron(void);

/*
 * tclfile.c:
 */

NS_EXTERN int Ns_TclGetOpenChannel(Tcl_Interp *interp, char *chanId, int write,
				int check, Tcl_Channel *chanPtr);
NS_EXTERN int Ns_TclGetOpenFd(Tcl_Interp *interp, char *chanId, int write,
			   int *fdPtr);

/*
 * tclinit.c:
 */

NS_EXTERN int
Nsd_Init(Tcl_Interp *interp);

NS_EXTERN Tcl_Interp *
Ns_TclCreateInterp(void);

NS_EXTERN int
Ns_TclInit(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TclEval(Ns_DString *dsPtr, CONST char *server, CONST char *script)
     NS_GNUC_NONNULL(3);

NS_EXTERN Tcl_Interp *
Ns_TclAllocateInterp(CONST char *server);

NS_EXTERN void
Ns_TclDeAllocateInterp(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Interp *
Ns_GetConnInterp(Ns_Conn *conn)
     NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Conn *
Ns_TclGetConn(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TclDestroyInterp(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TclMarkForDelete(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TclRegisterTrace(CONST char *server, Ns_TclTraceProc *proc, void *arg, int when)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_TclLibrary(CONST char *server);

NS_EXTERN char *
Ns_TclInterpServer(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TclInitModule(CONST char *server, CONST char *module)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_FreeConnInterp(Ns_Conn *conn)
     NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_TclRegisterAtCreate(Ns_TclTraceProc *proc, void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_TclRegisterAtCleanup(Ns_TclTraceProc *proc, void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_TclRegisterAtDelete(Ns_TclTraceProc *proc, void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_TclInitInterps(CONST char *server, Ns_TclInterpInitProc *proc, void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_TclRegisterDeferred(Tcl_Interp *interp, Ns_TclDeferProc *proc, void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED;

/*
 * tclmisc.c
 */

NS_EXTERN void Ns_TclPrintfResult(Tcl_Interp *interp, char *fmt, ...)
     NS_GNUC_PRINTF(2, 3);

NS_EXTERN CONST char *
Ns_TclLogErrorInfo(Tcl_Interp *interp, CONST char *info)
    NS_GNUC_NONNULL(1);

NS_EXTERN CONST char *
Ns_TclLogError(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1);

NS_EXTERN CONST char *
Ns_TclLogErrorRequest(Tcl_Interp *interp, Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

/*
 * tclrequest.c:
 */

NS_EXTERN int
Ns_TclRequest(Ns_Conn *conn, CONST char *proc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * tclset.c:
 */

NS_EXTERN int Ns_TclEnterSet(Tcl_Interp *interp, Ns_Set *set, int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_Set *Ns_TclGetSet(Tcl_Interp *interp, char *setId)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN int Ns_TclGetSet2(Tcl_Interp *interp, char *setId, Ns_Set **setPtrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
NS_EXTERN int Ns_TclFreeSet(Tcl_Interp *interp, char *setId)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * time.c:
 */

NS_EXTERN char *Ns_HttpTime(Ns_DString *pds, time_t *when);
NS_EXTERN time_t Ns_ParseHttpTime(char *str);

/*
 * url.c:
 */

NS_EXTERN CONST char *
Ns_RelativeUrl(CONST char *url, CONST char *location);

NS_EXTERN int
Ns_ParseUrl(char *url, char **pprotocol, char **phost, char **pport,
            char **ppath, char **ptail)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4)
    NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

NS_EXTERN int
Ns_AbsoluteUrl(Ns_DString *pds, CONST char *url, CONST char *baseurl)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * url2file.c:
 */

NS_EXTERN void
Ns_RegisterUrl2FileProc(CONST char *server, CONST char *url,
                        Ns_Url2FileProc *proc, Ns_Callback *deletecb,
                        void *arg, int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterUrl2FileProc(CONST char *server, CONST char *url, int inherit)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_UrlToFile(Ns_DString *dsPtr, CONST char *server, CONST char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_SetUrlToFileProc(CONST char *server, Ns_UrlToFileProc *procPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


NS_EXTERN Ns_Url2FileProc Ns_FastUrl2FileProc;


/*
 * urlencode.c:
 */

NS_EXTERN Tcl_Encoding Ns_GetUrlEncoding(char *charset);
NS_EXTERN char *Ns_UrlPathEncode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);
NS_EXTERN char *Ns_UrlPathDecode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);
NS_EXTERN char *Ns_UrlQueryEncode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);
NS_EXTERN char *Ns_UrlQueryDecode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);
NS_EXTERN char *Ns_EncodeUrlWithEncoding(Ns_DString *dsPtr, char *string,
                                         Tcl_Encoding encoding) NS_GNUC_DEPRECATED;
NS_EXTERN char *Ns_DecodeUrlWithEncoding(Ns_DString *dsPtr, char *string,
                                         Tcl_Encoding encoding) NS_GNUC_DEPRECATED;
NS_EXTERN char *Ns_EncodeUrlCharset(Ns_DString *dsPtr, char *string,
                                    char *charset) NS_GNUC_DEPRECATED;
NS_EXTERN char *Ns_DecodeUrlCharset(Ns_DString *dsPtr, char *string,
                                    char *charset) NS_GNUC_DEPRECATED;

/*
 * urlopen.c:
 */

NS_EXTERN int Ns_FetchPage(Ns_DString *pds, char *url, char *server);
NS_EXTERN int Ns_FetchURL(Ns_DString *pds, char *url, Ns_Set *headers);

/*
 * urlspace.c:
 */

NS_EXTERN int Ns_UrlSpecificAlloc(void);

NS_EXTERN void
Ns_UrlSpecificSet(CONST char *server, CONST char *method, CONST char *url, int id,
                  void *data, int flags, void (*deletefunc)(void *))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

NS_EXTERN void *
Ns_UrlSpecificGet(CONST char *server, CONST char *method, CONST char *url, int id)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void *
Ns_UrlSpecificGetFast(CONST char *server, CONST char *method, CONST char *url, int id)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void *
Ns_UrlSpecificGetExact(CONST char *server, CONST char *method, CONST char *url,
                       int id, int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void *
Ns_UrlSpecificDestroy(CONST char *server, CONST char *method, CONST char *url,
                      int id, int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * fd.c:
 */

NS_EXTERN int Ns_CloseOnExec(int fd);
NS_EXTERN int Ns_NoCloseOnExec(int fd);
NS_EXTERN int Ns_DupHigh(int *fdPtr);
NS_EXTERN int Ns_GetTemp(void);
NS_EXTERN void Ns_ReleaseTemp(int fd);

/*
 * unix.c, win32.c:
 */

NS_EXTERN int ns_sockpair(SOCKET *socks);
NS_EXTERN int ns_pipe(int *fds);
NS_EXTERN int ns_poll(struct pollfd *fds, unsigned long int nfds, int timo);
NS_EXTERN int Ns_GetNameForUid(Ns_DString *dsPtr, int uid);
NS_EXTERN int Ns_GetNameForGid(Ns_DString *dsPtr, int gid);
NS_EXTERN int Ns_GetUserHome(Ns_DString *dsPtr, char *user);
NS_EXTERN int Ns_GetUserGid(char *user);
NS_EXTERN int Ns_GetUid(char *user);
NS_EXTERN int Ns_GetGid(char *group);

/*
 * form.c:
 */

NS_EXTERN void
Ns_ConnClearQuery(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

/*
 * Compatibility macros.
 */

#ifdef NS_NOCOMPAT
#  error "No compatibility macros at present"
#endif

#endif /* NS_H */
