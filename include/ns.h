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
 */

#ifndef NS_H
#define NS_H

#include "nsversion.h"
#include "nsthread.h"

#ifdef HAVE_ZLIB_H
# include <zlib.h>
#endif

#ifdef NSD_EXPORTS
#undef NS_EXTERN
#ifdef __cplusplus
#define NS_EXTERN                  extern "C" NS_EXPORT
#else
#define NS_EXTERN                  extern NS_EXPORT
#endif
#endif

/*
 * Boolean result.
 */

#define NS_TRUE                    1
#define NS_FALSE                   0

/*
 * The following describe various properties of a connection. Used in the
 * public interface in e.g. Ns_ConnWriteVChars() or Ns_ConnWriteData()
 */

#define NS_CONN_CLOSED             0x001U /* The underlying socket is closed */
#define NS_CONN_SKIPHDRS           0x002U /* Client is HTTP/0.9, do not send HTTP headers  */
#define NS_CONN_SKIPBODY           0x004U /* HTTP HEAD request, do not send body */
#define NS_CONN_READHDRS           0x008U /* Unused */
#define NS_CONN_SENTHDRS           0x010U /* Response headers have been sent to client */
#define NS_CONN_WRITE_ENCODED      0x020U /* Character data mode requested mime-type header. */
#define NS_CONN_STREAM             0x040U /* Data is to be streamed when ready.  */
#define NS_CONN_STREAM_CLOSE       0x080U /* Writer Stream should be closed.  */
#define NS_CONN_CHUNK              0x100U /* Streamed data is to be chunked. */
#define NS_CONN_SENT_LAST_CHUNK    0x200U /* Marks that the last chunk was sent in chunked mode */
#define NS_CONN_SENT_VIA_WRITER    0x400U /* Response data has been sent via writer thread */
#define NS_CONN_SOCK_CORKED        0x800U /* underlying socket is corked */
#define NS_CONN_ZIPACCEPTED       0x1000U /* the request accepts zip encoding */
#define NS_CONN_ENTITYTOOLARGE    0x2000U /* the sent Entity was too large */
#define NS_CONN_REQUESTURITOOLONG 0x4000U /* request-URI too long */
#define NS_CONN_LINETOOLONG       0x8000U /* request Header line too long */

/*
 * Coockie creation options.  For NaviServer and the current set of NaviServer
 * modules, these constants would not be needed here. As long we have
 * Ns_ConnSetCookieEx() in the public interface, we these flags here as well.
 */
#define NS_COOKIE_SECURE           0x01U  /* The cookie should only be sent using HTTPS */
#define NS_COOKIE_SCRIPTABLE       0x02U  /* Available to javascript on the client. */
#define NS_COOKIE_DISCARD          0x04U  /* Discard the cookie at the end of the current session. */
#define NS_COOKIE_REPLACE          0x08U  /* Replace the cookie in the output headers. */
#define NS_COOKIE_EXPIRENOW        0x10U  /* Replace the cookie in the output headers. */

/*
 * The following are the valid attributes of a scheduled event. For NaviServer
 * and the current set of NaviServer modules, these constants would not be
 * needed here. As long Ns_ScheduleProcEx() is in the public interface and
 * uses the flags, we these constants here as well.
 */
#define NS_SCHED_THREAD            0x01U /* Ns_SchedProc will run in detached thread */
#define NS_SCHED_ONCE              0x02U /* Call cleanup proc after running once */
#define NS_SCHED_DAILY             0x04U /* Event is scheduled to occur daily */
#define NS_SCHED_WEEKLY            0x08U /* Event is scheduled to occur weekly */
#define NS_SCHED_PAUSED            0x10U /* Event is currently paused */
#define NS_SCHED_RUNNING           0x20U /* Event is currently running, perhaps in detached thread */

/*
 * The following are valid options when manipulating
 * URL specific data.
 */
#define NS_OP_NOINHERIT            0x02U /* Match URL exactly */
#define NS_OP_NODELETE             0x04U /* Do call previous procs Ns_OpDeleteProc */
#define NS_OP_RECURSE              0x08U /* Also destroy registered procs below given URL */



/*
 * The following types of filters may be registered.
 */
typedef enum {
    NS_FILTER_PRE_AUTH =        0x01U, /* Runs before any Ns_UserAuthProc */
    NS_FILTER_POST_AUTH =       0x02U, /* Runs after any Ns_UserAuthProc */
    NS_FILTER_TRACE =           0x04U, /* Runs after Ns_OpProc completes successfully */
    NS_FILTER_VOID_TRACE =      0x08U  /* Run ns_register_trace procs after previous traces */
} Ns_FilterType;


/*
 * The following define socket events for the Ns_Sock* APIs.
 */
typedef enum {
    NS_SOCK_READ =              0x01U, /* Socket is readable */
    NS_SOCK_WRITE =             0x02U, /* Socket is writeable */
    NS_SOCK_EXCEPTION =         0x04U, /* Socket has OOB data */
    NS_SOCK_EXIT =              0x08U, /* The server is shutting down */
    NS_SOCK_DONE =              0x10U, /* Task processing is done */
    NS_SOCK_CANCEL =            0x20U, /* Remove event from sock callback thread */
    NS_SOCK_TIMEOUT =           0x40U, /* Timeout waiting for socket event. */
    NS_SOCK_INIT =              0x80U /* Initialise a Task callback. */
} Ns_SockState;

/*
 * Many of sock-states are just from the Ns_EventQueue or Ns_Task
 * interface. It is probably a good idea to define different types for these
 * interfaces, or to define e.g. SockConditions like the following
 *

typedef enum {
    NS_SOCK_COND_READ =         NS_SOCK_READ,
    NS_SOCK_COND_WRITE =        NS_SOCK_WRITE,
    NS_SOCK_COND_EXCEPTION =    NS_SOCK_EXCEPTION
} Ns_SockCondition;
*/

#define NS_SOCK_ANY                ((unsigned int)NS_SOCK_READ|(unsigned int)NS_SOCK_WRITE|(unsigned int)NS_SOCK_EXCEPTION)

/*
 * The following are valid comm driver options.
 */
#define NS_DRIVER_ASYNC            0x01U /* Use async read-ahead. */
#define NS_DRIVER_SSL              0x02U /* Use SSL port, protocol defaults. */
#define NS_DRIVER_NOPARSE          0x04U /* Do not parse request */
#define NS_DRIVER_UDP              0x08U /* UDP, can't use stream socket options */

#define NS_DRIVER_VERSION_1        1    /* Obsolete. */
#define NS_DRIVER_VERSION_2        2    /* Current version. */

/*
 * The following are valid Tcl interp traces types.
 */

typedef enum {
    NS_TCL_TRACE_NONE         = 0x00u, /* for initializing variable */ 
    NS_TCL_TRACE_CREATE       = 0x01u, /* New interp created */
    NS_TCL_TRACE_DELETE       = 0x02u, /* Interp destroyed */
    NS_TCL_TRACE_ALLOCATE     = 0x04u, /* Interp allocated, possibly from thread cache */
    NS_TCL_TRACE_DEALLOCATE   = 0x08u, /* Interp de-allocated, returned to thread-cache */
    NS_TCL_TRACE_GETCONN      = 0x10u, /* Interp allocated for connection processing (filter, proc) */
    NS_TCL_TRACE_FREECONN     = 0x20u  /* Interp finished connection processing */
} Ns_TclTraceType;

/*
 * The following define some buffer sizes and limits.
 */
#define NS_CONN_MAXCLS             16u /* Max num CLS keys which may be allocated */
#define NS_CONN_MAXBUFS            16  /* Max num buffers which Ns_ConnSend will write */
#define NS_ENCRYPT_BUFSIZE         128 /* Min size of buffer for Ns_Encrypt output */

/*
 * The following flags define how Ns_Set's are managed by Tcl. Used in the
 * public interface by Ns_TclEnterSet()
 */
#define NS_TCL_SET_STATIC          0U /* Ns_Set managed elsewhere, maintain a Tcl reference */
#define NS_TCL_SET_DYNAMIC         1U /* Tcl owns the Ns_Set and will free when finished */

/*
 * C API macros.
 */

#define UCHAR(c)                   ((unsigned char)(c))
#define CHARTYPE(what,c)           (is ## what ((int)((unsigned char)(c))))
#define CHARCONV(what,c)           ((char)to ## what ((int)((unsigned char)(c))))
#define STREQ(a,b)                 (((*(a)) == (*(b))) && (strcmp((a),(b)) == 0))
#define STRIEQ(a,b)                (strcasecmp((a),(b)) == 0)
#define Ns_IndexCount(X)           ((X)->n)
#define Ns_ListPush(elem,list)     ((list)=Ns_ListCons((elem),(list)))
#define Ns_ListFirst(list)         ((list)->first)
#define Ns_ListRest(list)          ((list)->rest)
#define Ns_SetSize(s)              ((s)->size)
#define Ns_SetName(s)              ((s)->name)
#define Ns_SetKey(s,i)             ((s)->fields[(i)].name)
#define Ns_SetValue(s,i)           ((s)->fields[(i)].value)
#define Ns_SetLast(s)              (((s)->size)-1u)

/*
 * Ns_DString's are now equivalent to Tcl_DString's starting in 4.0.
 */

#define Ns_DString                 Tcl_DString
#define Ns_DStringLength           Tcl_DStringLength
#define Ns_DStringValue            Tcl_DStringValue
#define Ns_DStringNAppend          Tcl_DStringAppend
#define Ns_DStringAppend(d,s)      Tcl_DStringAppend((d), (s), -1)
#define Ns_DStringAppendElement    Tcl_DStringAppendElement
#define Ns_DStringInit             Tcl_DStringInit
#define Ns_DStringFree             Tcl_DStringFree
#define Ns_DStringTrunc            Tcl_DStringTrunc
#define Ns_DStringSetLength        Tcl_DStringSetLength
#define NS_DSTRING_STATIC_SIZE     (TCL_DSTRING_STATIC_SIZE)
#define NS_DSTRING_PRINTF_MAX      2048

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
typedef struct _Ns_Sls          *Ns_Sls;
typedef void                    *Ns_OpContext;
typedef struct _Ns_TaskQueue    *Ns_TaskQueue;
typedef struct _Ns_Task         *Ns_Task;
typedef struct _Ns_EventQueue   *Ns_EventQueue;
typedef struct _Ns_Event        *Ns_Event;

typedef int bool;

/*
 * This is used for logging messages.
 */

enum {
    Notice,
    Warning,
    Error,
    Fatal,
    Bug,
    Debug,
    Dev,
    PredefinedLogSeveritiesCount
};
typedef int Ns_LogSeverity;

/*
 * The following enum lists the possible HTTP headers
 * conversion options (default: Preserve).
 */

typedef enum {
    Preserve, ToLower, ToUpper
} Ns_HeaderCaseDisposition;

/*
 * LogSeverity, which can be used from modules (e.g. nsssl)
 */

NS_EXTERN Ns_LogSeverity Ns_LogTaskDebug;    /* Severity at which to log verbose. */

/*
 * Typedefs of functions
 */

typedef int   (Ns_IndexCmpProc) (const void *left, const void *right);
typedef int   (Ns_SortProc) (void *left, void *right);
typedef int   (Ns_EqualProc) (void *left, void *right);
typedef void  (Ns_ElemVoidProc) (void *elem);
typedef void *(Ns_ElemValProc) (void *elem);
typedef int   (Ns_ElemTestProc) (void *elem);
typedef void  (Ns_Callback) (void *arg);
typedef void  (Ns_ShutdownProc) (const Ns_Time *toPtr, void *arg);
typedef int   (Ns_TclInterpInitProc) (Tcl_Interp *interp, const void *arg);
typedef int   (Ns_TclTraceProc) (Tcl_Interp *interp, const void *arg);
typedef void  (Ns_TclDeferProc) (Tcl_Interp *interp, void *arg);
typedef bool  (Ns_SockProc) (NS_SOCKET sock, void *arg, unsigned int why);
typedef void  (Ns_TaskProc) (Ns_Task *task, NS_SOCKET sock, void *arg, Ns_SockState why);
typedef void  (Ns_EventProc) (Ns_Event *event, NS_SOCKET sock, void *arg, Ns_Time *now, Ns_SockState why);
typedef void  (Ns_SchedProc) (void *arg, int id);
typedef int   (Ns_ServerInitProc) (const char *server);
typedef int   (Ns_ModuleInitProc) (const char *server, const char *module);
typedef int   (Ns_RequestAuthorizeProc) (const char *server, const char *method,
			const char *url, const char *user, const char *pass, const char *peer);
typedef void  (Ns_AdpParserProc)(Ns_DString *outPtr, char *page);
typedef int   (Ns_UserAuthorizeProc) (const char *user, const char *passwd);
struct Ns_ObjvSpec;
typedef int   (Ns_ObjvProc) (struct Ns_ObjvSpec *spec, Tcl_Interp *interp,
                             int *objcPtr, Tcl_Obj *CONST* objv);

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
    const char  *name;
    size_t       size;
    size_t       maxSize;
    Ns_SetField *fields;
} Ns_Set;

/*
 * The request structure.
 */

typedef struct Ns_Request {
    const char     *line;
    const char     *method;
    const char     *protocol;
    const char     *host;
    unsigned short  port;
    const char     *url;
    char           *query;
    int             urlc;
    char          **urlv;
    double          version;
} Ns_Request;

/*
 * The connection structure.
 */

typedef struct Ns_Conn {
    Ns_Request  *request;
    Ns_Set      *headers;
    Ns_Set      *outputheaders;
    Ns_Set      *auth;
    size_t       contentLength;
    unsigned int flags;		/* Currently, only NS_CONN_CLOSED. */
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
    const char      *key;
    unsigned int     value;
} Ns_ObjvTable;

/*
 * The following structure defines the Tcl code to run
 * for a callback function.
 */

typedef struct Ns_TclCallback {
    Ns_Callback    *cbProc;
    const char     *server;
    const char     *script;
    int             argc;
    char          **argv;
} Ns_TclCallback;

/*
 * The following structure defines a driver.
 */

typedef struct Ns_Driver {
    void       *arg;           /* Driver callback data. */
    const char *server;        /* Virtual server name. */
    const char *module;        /* Driver module. */
    const char *name;          /* Driver name. */
    const char *location;      /* Location, e.g, "http://foo:9090" */
    const char *address;       /* Address in location, e.g. "foo" */
    const char *protocol;      /* Protocol in location, e.g, "http" */
    long        sendwait;      /* send() I/O timeout in seconds */
    long        recvwait;      /* recv() I/O timeout in seconds */
    size_t      bufsize;       /* Conn bufsize (0 for SSL) */
    const char *extraHeaders;  /* Extra header fields added for every request */
} Ns_Driver;

/*
 * The following structure defins the public
 * parts of the driver socket connection.
 */

typedef struct Ns_Sock {
    Ns_Driver          *driver;
    NS_SOCKET           sock;           /* Connection socket */
    struct sockaddr_in  sa;             /* Actual peer address */
    void               *arg;            /* Driver context. */
} Ns_Sock;

/*
 * The following structure defines a range of bytes to send from a
 * file or memory location. The descriptior fd must be a normal file
 * in the filesystem, not a socket.
 */

typedef struct Ns_FileVec {
    int        fd;      /* File descriptor of file to send, or < 0 for memory. */
    off_t      offset;  /* Offset into file to begin sending, or void *. */
    size_t     length;  /* Number of bytes to send from offset. */
} Ns_FileVec;

/*
 * The following are the valid return values of an Ns_DriverAcceptProc.
 */

typedef enum {
    NS_DRIVER_ACCEPT,
    NS_DRIVER_ACCEPT_DATA,
    NS_DRIVER_ACCEPT_ERROR,
    NS_DRIVER_ACCEPT_QUEUE
} NS_DRIVER_ACCEPT_STATUS;

/*
 * The following typedefs define socket driver callbacks.
 */

typedef NS_SOCKET
(Ns_DriverListenProc)(Ns_Driver *driver, CONST char *address, int port, int backlog)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef NS_DRIVER_ACCEPT_STATUS
(Ns_DriverAcceptProc)(Ns_Sock *sock, NS_SOCKET listensock,
                      struct sockaddr *sockaddr, socklen_t *socklen)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

typedef ssize_t
(Ns_DriverRecvProc)(Ns_Sock *sock, struct iovec *bufs, int nbufs,
                    Ns_Time *timeoutPtr, unsigned int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef ssize_t
(Ns_DriverSendProc)(Ns_Sock *sock, const struct iovec *bufs, int nbufs,
                    const Ns_Time *timeoutPtr, unsigned int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef ssize_t
(Ns_DriverSendFileProc)(Ns_Sock *sock, Ns_FileVec *bufs, int nbufs,
                        Ns_Time *timeoutPtr, unsigned int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef int
(Ns_DriverRequestProc)(void *arg, Ns_Conn *conn)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef bool
(Ns_DriverKeepProc)(Ns_Sock *sock)
     NS_GNUC_NONNULL(1);

typedef void
(Ns_DriverCloseProc)(Ns_Sock *sock)
     NS_GNUC_NONNULL(1);

/*
 * The following structure defines the values to initialize the driver. This is
 * passed to Ns_DriverInit.
 */

typedef struct Ns_DriverInitData {
    int                   version;       /* Version 2. */
    char                  *name;         /* This will show up in log file entries */
    Ns_DriverListenProc   *listenProc;   /* Open listening socket for conns. */
    Ns_DriverAcceptProc   *acceptProc;   /* Accept a new non-blocking socket. */
    Ns_DriverRecvProc     *recvProc;     /* Read bytes from conn into iovec. */
    Ns_DriverSendProc     *sendProc;     /* Write bytes to conn from iovec. */
    Ns_DriverSendFileProc *sendFileProc; /* Optional: write bytes from files/buffers. */
    Ns_DriverKeepProc     *keepProc;     /* Keep a socket open after conn done? */
    Ns_DriverRequestProc  *requestProc;  /* First proc to be called by a connection thread. */
    Ns_DriverCloseProc    *closeProc;    /* Close a connection socket. */
    unsigned int           opts;         /* NS_DRIVER_ASYNC | NS_DRIVER_SSL  */
    void                  *arg;          /* Module's driver callback data */
    const char            *path;         /* Path to find port, address, etc. */
} Ns_DriverInitData;



/*
 * MD5 digest implementation
 */

typedef struct Ns_CtxMD5 {
    uint32_t buf[4];
    uint32_t bits[2];
    unsigned char in[64];
} Ns_CtxMD5;

/*
 * SHA1 digest implementation
 */

#define SHA_HASHWORDS  5U
#define SHA_BLOCKWORDS 16U

typedef struct Ns_CtxSHA1 {
    unsigned int key[SHA_BLOCKWORDS];
    uint32_t iv[SHA_HASHWORDS];
#if defined(HAVE_64BIT)
    uint64_t bytes;
#else
    uint32_t bytesHi, bytesLo;
#endif
} Ns_CtxSHA1;

/*
 * More typedefs of functions
 */

typedef void (Ns_ArgProc)
    (Tcl_DString *dsPtr, const void *arg);

typedef int (Ns_OpProc)
    (void *arg, Ns_Conn *conn);

typedef void (Ns_TraceProc)
    (void *arg, Ns_Conn *conn);

typedef int (Ns_FilterProc)
    (void *arg, Ns_Conn *conn, Ns_FilterType why);

typedef int (Ns_LogFilter)
    (void *arg, Ns_LogSeverity severity, const Ns_Time *stamp, const char *msg, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

typedef int (Ns_UrlToFileProc)
    (Ns_DString *dsPtr, const char *server, const char *url);

typedef int (Ns_Url2FileProc)
    (Ns_DString *dsPtr, const char *url, void *arg);

typedef char* (Ns_ServerRootProc)
    (Ns_DString  *dest, const char *host, void *arg);

typedef char* (Ns_ConnLocationProc)
    (Ns_Conn *conn, Ns_DString *dest, void *arg);

typedef int (Ns_LogProc)               /* Deprecated */
    (Ns_DString *dsPtr, Ns_LogSeverity severity, const char *fmt, va_list ap);

typedef int (Ns_LogFlushProc)          /* Deprecated */
    (const char *msg, size_t len);

typedef char *(Ns_LocationProc)        /* Deprecated */
    (Ns_Conn *conn);

/*
 * adpcmds.c:
 */

NS_EXTERN int
Ns_AdpAppend(Tcl_Interp *interp, const char *buf, int len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpGetOutput(Tcl_Interp *interp, Tcl_DString **dsPtrPtr,
                int *doStreamPtr, size_t *maxBufferPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * adprequest.c:
 */

NS_EXTERN int
Ns_AdpRequest(Ns_Conn *conn, const char *file)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpRequestEx(Ns_Conn *conn, const char *file, const Ns_Time *expiresPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpFlush(Tcl_Interp *interp, int isStreaming)
    NS_GNUC_NONNULL(1);

/*
 * auth.c:
 */

NS_EXTERN int
Ns_AuthorizeRequest(const char *server, const char *method, const char *url,
		    const char *user, const char *passwd, const char *peer)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_SetRequestAuthorizeProc(const char *server, Ns_RequestAuthorizeProc *procPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetUserAuthorizeProc(Ns_UserAuthorizeProc *procPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_AuthorizeUser(const char *user, const char *passwd)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * cache.c:
 */

NS_EXTERN Ns_Cache *
Ns_CacheCreate(const char *name, int keys, time_t ttl, Ns_Callback *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Cache *
Ns_CacheCreateSz(const char *name, int keys, size_t maxSize, Ns_Callback *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Cache *
Ns_CacheCreateEx(const char *name, int keys, time_t ttl, size_t maxSize,
                 Ns_Callback *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheDestroy(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Entry *
Ns_CacheFindEntry(Ns_Cache *cache, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Entry *
Ns_CacheCreateEntry(Ns_Cache *cache, const char *key, int *newPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_Entry *
Ns_CacheWaitCreateEntry(Ns_Cache *cache, const char *key, int *newPtr,
                        const Ns_Time *timeoutPtr) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN const char *
Ns_CacheKey(Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN void *
Ns_CacheGetValue(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_CacheGetSize(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1);

NS_EXTERN const Ns_Time *
Ns_CacheGetExpirey(const Ns_Entry *entry);

NS_EXTERN void
Ns_CacheSetValue(Ns_Entry *entry, void *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CacheSetValueSz(Ns_Entry *entry, void *value, size_t size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CacheSetValueExpires(Ns_Entry *entry, void *value, size_t size,
                        const Ns_Time *timeoutPtr, int cost) 
    NS_GNUC_NONNULL(1);

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

NS_EXTERN int
Ns_CacheWait(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_CacheTimedWait(Ns_Cache *cache, const Ns_Time *timePtr)
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
Ns_CacheResetStats(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

/*
 * callbacks.c:
 */

NS_EXTERN void *Ns_RegisterAtPreStartup(Ns_Callback *proc, void *arg) NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtStartup(Ns_Callback *proc, void *arg)    NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtSignal(Ns_Callback *proc, void *arg)     NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtReady(Ns_Callback *proc, void *arg)      NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtShutdown(Ns_ShutdownProc *proc, void *arg) NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtExit(Ns_Callback *proc, void *arg)       NS_GNUC_NONNULL(1);

/*
 * cls.c:
 */

NS_EXTERN void Ns_ClsAlloc(Ns_Cls *clsPtr, Ns_Callback *cleanupProc);
NS_EXTERN void *Ns_ClsGet(const Ns_Cls *clsPtr, Ns_Conn *conn);
NS_EXTERN void Ns_ClsSet(const Ns_Cls *clsPtr, Ns_Conn *conn, void *value);

/*
 * compress.c:
 */

typedef struct Ns_CompressStream {

#ifdef HAVE_ZLIB_H
    z_stream   z;
#endif
    unsigned int flags;

} Ns_CompressStream;


NS_EXTERN int
Ns_CompressInit(Ns_CompressStream *cStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CompressFree(Ns_CompressStream *cStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_CompressBufsGzip(Ns_CompressStream *cStream, struct iovec *bufs, int nbufs, 
		    Ns_DString *dsPtr, int level, int flush)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_CompressGzip(const char *buf, int len, Tcl_DString *outPtr, int level)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int 
Ns_InflateInit(Ns_CompressStream *cStream) 
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_InflateBufferInit(Ns_CompressStream *cStream, const char *buffer, size_t inSize) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_InflateBuffer(Ns_CompressStream *cStream, const char *buffer, size_t outSize, size_t *nrBytes) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_InflateEnd(Ns_CompressStream *cStream) 
    NS_GNUC_NONNULL(1);

/*
 * For HTTP tasks (in ns_http and ns_https)
 */
typedef struct {
    Ns_Task    *task;
    NS_SOCKET   sock;
    int         status;
    const char *url;
    char       *error;
    char       *next;             /* write to client */
    size_t      len;              /* size of request */
    int         replyHeaderSize;
    Ns_Set     *replyHeaders;     /* ns_set for header fields of the reply */
    int         spoolLimit;       /* spool to file, when this body > this size */
    int         spoolFd;          /* fd of spool file */
    char       *spoolFileName;    /* filename of spoolfile */
    Ns_Mutex    lock;             /* needed for switching modes (spooling to file/memory) */
    unsigned int       flags;
    Ns_CompressStream *compress;
    Ns_Time     timeout;
    Ns_Time     stime;
    Ns_Time     etime;
    Tcl_DString ds;
} Ns_HttpTask;

#define NS_HTTP_FLAG_DECOMPRESS    0x0001U
#define NS_HTTP_FLAG_GZIP_ENCODING 0x0002U
#define NS_HTTP_FLAG_GUNZIP        (NS_HTTP_FLAG_DECOMPRESS|NS_HTTP_FLAG_GZIP_ENCODING)



/*
 * config.c:
 */

NS_EXTERN const char *
Ns_ConfigString(const char *section, const char *key, const char *def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigBool(const char *section, const char *key, int def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigFlag(const char *section, const char *key, unsigned int flag, int def,
              unsigned int *flagsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

NS_EXTERN int
Ns_ConfigInt(const char *section, const char *key, int def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigIntRange(const char *section, const char *key, int def,
                  int min, int max)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_ConfigWideInt(const char *section, const char *key, Tcl_WideInt def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_ConfigWideIntRange(const char *section, const char *key, Tcl_WideInt def,
                  Tcl_WideInt min, Tcl_WideInt max)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_ConfigGetValue(const char *section, const char *key)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_ConfigGetValueExact(const char *section, const char *key)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigGetInt(const char *section, const char *key, int *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConfigGetInt64(const char *section, const char *key, int64_t *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConfigGetBool(const char *section, const char *key, int *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN const char *
Ns_ConfigGetPath(const char *server, const char *module, ...)
     NS_GNUC_SENTINEL;

NS_EXTERN Ns_Set **
Ns_ConfigGetSections(void);

NS_EXTERN Ns_Set *
Ns_ConfigGetSection(const char *section)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Set *
Ns_ConfigCreateSection(const char *section)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_GetVersion(int *majorV, int *minorV, int *patchLevelV, int *type);

/*
 * conn.c:
 */

NS_EXTERN uintptr_t
Ns_ConnId(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnContentFd(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_ConnContentSize(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnContentFile(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_ConnGetEncoding(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetEncoding(Ns_Conn *conn, Tcl_Encoding encoding) NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_ConnGetUrlEncoding(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding) NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnGetCompression(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetCompression(Ns_Conn *conn, int level) NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_ConnModifiedSince(const Ns_Conn *conn, time_t since) NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_ConnUnmodifiedSince(const Ns_Conn *conn, time_t since) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Set *
Ns_ConnHeaders(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Set *
Ns_ConnOutputHeaders(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Set *
Ns_ConnAuth(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnAuthUser(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnAuthPasswd(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_ConnContentLength(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnContent(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnServer(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnResponseStatus(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetResponseStatus(Ns_Conn *conn, int newStatus) NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_ConnContentSent(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetContentSent(Ns_Conn *conn, size_t length) NS_GNUC_NONNULL(1);

NS_EXTERN ssize_t
Ns_ConnResponseLength(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnPeer(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_ConnSetPeer(Ns_Conn *conn, const struct sockaddr_in *saPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnPeerPort(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnLocation(Ns_Conn *conn) 
    NS_GNUC_DEPRECATED_FOR(Ns_ConnLocationAppend);

NS_EXTERN char *
Ns_ConnLocationAppend(Ns_Conn *conn, Ns_DString *dest) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_ConnHost(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnPort(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_ConnSock(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Sock *
Ns_ConnSockPtr(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_DString *
Ns_ConnSockContent(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ConnDriverName(const Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding) NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_SetConnLocationProc(Ns_ConnLocationProc *proc, void *arg) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetLocationProc(const char *server, Ns_LocationProc *proc) NS_GNUC_DEPRECATED_FOR(Ns_SetConnLocationProc);

NS_EXTERN Ns_Time *
Ns_ConnStartTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Time *
Ns_ConnAcceptTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Time *
Ns_ConnQueueTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Time *
Ns_ConnDequeueTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Time *
Ns_ConnFilterTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN void
Ns_ConnTimeSpans(const Ns_Conn *conn, Ns_Time *acceptTimeSpanPtr, Ns_Time *queueTimeSpanPtr, 
		 Ns_Time *filterTimeSpanPtr, Ns_Time *runTimeSpanPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

NS_EXTERN void
Ns_ConnTimeStats(Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Time *
Ns_ConnTimeout(Ns_Conn *conn) NS_GNUC_NONNULL(1);


/*
 * connio.c:
 */

NS_EXTERN int
Ns_ConnWriteChars(Ns_Conn *conn, const char *buf, size_t toWrite, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteVChars(Ns_Conn *conn, struct iovec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteData(Ns_Conn *conn, const void *buf, size_t toWrite, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteVData(Ns_Conn *conn, struct iovec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnSendFd(Ns_Conn *conn, int fd, size_t nsend)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnSendFp(Ns_Conn *conn, FILE *fp, size_t nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendChannel(Ns_Conn *conn, Tcl_Channel chan, size_t nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendFileVec(Ns_Conn *conn, Ns_FileVec *bufs, int nbufs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendDString(Ns_Conn *conn, const Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnPuts(Ns_Conn *conn, const char *s)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN ssize_t
Ns_ConnSend(Ns_Conn *conn, struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnClose(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnFlushContent(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_ConnGets(char *buf, size_t bufsize, Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN size_t
Ns_ConnRead(Ns_Conn *conn, void *vbuf, size_t toRead)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReadLine(Ns_Conn *conn, Ns_DString *dsPtr, size_t *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReadHeaders(Ns_Conn *conn, Ns_Set *set, size_t *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnCopyToDString(Ns_Conn *conn, size_t toCopy, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnCopyToFd(Ns_Conn *conn, size_t ncopy, int fd)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnCopyToFile(Ns_Conn *conn, size_t ncopy, FILE *fp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnCopyToChannel(Ns_Conn *conn, size_t ncopy, Tcl_Channel chan)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);


NS_EXTERN int
Ns_ConnWrite(Ns_Conn *conn, const void *buf, size_t toWrite)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_WriteConn(Ns_Conn *conn, const char *buf, size_t toWrite)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(Ns_ConnWriteVData);

NS_EXTERN int
Ns_WriteCharConn(Ns_Conn *conn, const char *buf, size_t toWrite)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED_FOR(Ns_ConnWriteVChars);

NS_EXTERN bool
Ns_CompleteHeaders(Ns_Conn *conn, size_t dataLength, unsigned int flags, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

/*
 * cookies.c:
 */

NS_EXTERN void
Ns_ConnSetCookie(const Ns_Conn *conn,  const char *name, const char *value, time_t maxage)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetSecureCookie(const Ns_Conn *conn, const char *name, const char *value, time_t maxage)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetCookieEx(const Ns_Conn *conn, const char *name, const char *value, time_t maxage,
		   const char *domain, const char *path, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnDeleteCookie(const Ns_Conn *conn, const char *name, const char *domain, const char *path)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnDeleteSecureCookie(const Ns_Conn *conn, const char *name, const char *domain, const char *path)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_ConnGetCookie(Ns_DString *dest, const Ns_Conn *conn, const char *name)
        NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * crypt.c:
 */

NS_EXTERN char *
Ns_Encrypt(const char *pw, const char *salt, char iobuf[])
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * dns.c:
 */

NS_EXTERN int
Ns_GetHostByAddr(Ns_DString *dsPtr, const char *addr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_GetAddrByHost(Ns_DString *dsPtr, const char *host)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_GetAllAddrByHost(Ns_DString *dsPtr, const char *host)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * driver.c:
 */

NS_EXTERN int
Ns_DriverInit(const char *server, const char *module, const Ns_DriverInitData *init)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int 
NsAsyncWrite(int fd, const char *buffer, size_t nbyte)
    NS_GNUC_NONNULL(2);

NS_EXTERN void
NsAsyncWriterQueueDisable(int shutdown);

NS_EXTERN void
NsAsyncWriterQueueEnable(void);


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
Ns_DStringAppendArg(Ns_DString *dsPtr, const char *bytes)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DStringPrintf(Ns_DString *dsPtr, const char *fmt, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_PRINTF(2,3);

NS_EXTERN char *
Ns_DStringVPrintf(Ns_DString *dsPtr, const char *fmt, va_list apSrc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

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
Ns_EventEnqueue(Ns_EventQueue *queue, NS_SOCKET sock, Ns_EventProc *proc, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_EventCallback(Ns_Event *event, Ns_SockState when, const Ns_Time *timeoutPtr)
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

NS_EXTERN pid_t
Ns_ExecProcess(const char *exec, const char *dir, int fdin, int fdout,
	       char *args, const Ns_Set *env)
    NS_GNUC_NONNULL(1);

NS_EXTERN pid_t
Ns_ExecProc(const char *exec, char **argv)
    NS_GNUC_NONNULL(1);

NS_EXTERN pid_t
Ns_ExecArgblk(const char *exec, const char *dir, int fdin, int fdout,
	      char *args, const Ns_Set *env)
    NS_GNUC_NONNULL(1);

NS_EXTERN pid_t
Ns_ExecArgv(const char *exec, const char *dir, int fdin, int fdout, char **argv, const Ns_Set *env)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_WaitProcess(pid_t pid);

NS_EXTERN int
Ns_WaitForProcess(pid_t pid, int *exitcodePtr);

/*
 * fastpath.c:
 */

NS_EXTERN int
Ns_ConnReturnFile(Ns_Conn *conn, int status, const char *mimeType, const char *file)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN const char *
Ns_PageRoot(const char *server)
    NS_GNUC_DEPRECATED_FOR(Ns_PagePath);

NS_EXTERN bool
Ns_UrlIsFile(const char *server, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_UrlIsDir(const char *server, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_OpProc Ns_FastPathProc;

/*
 * filter.c:
 */

NS_EXTERN void *
Ns_RegisterFilter(const char *server, const char *method, const char *url,
		  Ns_FilterProc *proc, Ns_FilterType when, void *arg, int first)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN void *
Ns_RegisterServerTrace(const char *server, Ns_TraceProc *proc, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN void *
Ns_RegisterConnCleanup(const char *server, Ns_TraceProc *proc, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void *
Ns_RegisterCleanup(Ns_TraceProc *proc, void *arg)
    NS_GNUC_NONNULL(1);

/*
 * uuencode.c
 */

NS_EXTERN size_t
Ns_HtuuEncode(const unsigned char *input, size_t inputSize, char *buf)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN size_t
Ns_HtuuDecode(const char *input, unsigned char *buf, size_t bufSize)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * index.c:
 */

NS_EXTERN void
Ns_IndexInit(Ns_Index *indexPtr, int inc, int (*CmpEls) (const void *left, const void *right),
     			         int (*CmpKeyWithEl) (const void *left, const void *right))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_IndexTrunc(Ns_Index*indexPtr) NS_GNUC_NONNULL(1);
    
NS_EXTERN void
Ns_IndexDestroy(Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Index *
Ns_IndexDup(const Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void *
Ns_IndexFind(const Ns_Index *indexPtr, const void *key)  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void *
Ns_IndexFindInf(const Ns_Index *indexPtr, const void *key) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void **
Ns_IndexFindMultiple(const Ns_Index *indexPtr, const void *key) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_IndexAdd(Ns_Index *indexPtr, void *el) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_IndexDel(Ns_Index *indexPtr, const void *el) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void *
Ns_IndexEl(const Ns_Index *indexPtr, int i) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexStringInit(Ns_Index *indexPtr, int inc) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Index *
Ns_IndexStringDup(const Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexStringAppend(Ns_Index *addtoPtr, const Ns_Index *addfromPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_IndexStringDestroy(Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexStringTrunc(Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexIntInit(Ns_Index *indexPtr, int inc) NS_GNUC_NONNULL(1);

/*
 * see macros above for:
 *
 * Ns_IndexCount(X)
 */

/*
 * lisp.c:
 */

NS_EXTERN Ns_List *
Ns_ListNconc(Ns_List *l1Ptr, Ns_List *l2Ptr);

NS_EXTERN Ns_List *
Ns_ListCons(void *elem, Ns_List *lPtr)
    NS_GNUC_RETURNS_NONNULL
    NS_GNUC_WARN_UNUSED_RESULT;

NS_EXTERN Ns_List *
Ns_ListNreverse(Ns_List *lPtr);

NS_EXTERN Ns_List *
Ns_ListLast(Ns_List *lPtr);

NS_EXTERN void
Ns_ListFree(Ns_List *lPtr, Ns_ElemVoidProc *freeProc);

NS_EXTERN void
Ns_IntPrint(int d);

NS_EXTERN void
Ns_StringPrint(const char *s) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ListPrint(const Ns_List *lPtr, Ns_ElemVoidProc *printProc);

NS_EXTERN Ns_List *
Ns_ListCopy(const Ns_List *lPtr);

NS_EXTERN int
Ns_ListLength(const Ns_List *lPtr);

NS_EXTERN Ns_List *
Ns_ListWeightSort(Ns_List *wPtr);

NS_EXTERN Ns_List *
Ns_ListSort(Ns_List *wPtr, Ns_SortProc *sortProc);

NS_EXTERN Ns_List *
Ns_ListDeleteLowElements(Ns_List *mPtr, float minweight);

NS_EXTERN Ns_List *
Ns_ListDeleteWithTest(void *elem, Ns_List *lPtr,
				      Ns_EqualProc *equalProc);

NS_EXTERN Ns_List *
Ns_ListDeleteIf(Ns_List *lPtr, Ns_ElemTestProc *testProc);

NS_EXTERN Ns_List *
Ns_ListDeleteDuplicates(Ns_List *lPtr, Ns_EqualProc *equalProc);

NS_EXTERN Ns_List *
Ns_ListNmapcar(Ns_List *lPtr, Ns_ElemValProc *valProc);

NS_EXTERN Ns_List *
Ns_ListMapcar(const Ns_List *lPtr, Ns_ElemValProc *valProc);
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

NS_EXTERN void
Ns_GenSeeds(unsigned long *seedsPtr, int nseeds);

NS_EXTERN double
Ns_DRand(void);

/*
 * task.c:
 */

NS_EXTERN Ns_TaskQueue *
Ns_CreateTaskQueue(const char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_DestroyTaskQueue(Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Task *
Ns_TaskCreate(NS_SOCKET sock, Ns_TaskProc *proc, void *arg)
    NS_GNUC_NONNULL(2)
    NS_GNUC_RETURNS_NONNULL
    NS_GNUC_WARN_UNUSED_RESULT;

NS_EXTERN int
Ns_TaskEnqueue(Ns_Task *task, Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_TaskRun(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskCallback(Ns_Task *task, Ns_SockState when, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskDone(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_TaskCompleted(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TaskCancel(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TaskWait(Ns_Task *task, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_TaskFree(Ns_Task *task)
    NS_GNUC_NONNULL(1);

/*
 * tclobj.c:
 */

NS_EXTERN void
Ns_TclResetObjType(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TclSetTwoPtrValue(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr,
                     void *ptr1, void *ptr2)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TclSetOtherValuePtr(Tcl_Obj *objPtr, Tcl_ObjType *newTypePtr, void *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_TclSetStringRep(Tcl_Obj *objPtr, const char *bytes, int length)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_TclGetAddrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
                     const char *type, void **addrPtrPtr)
     NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_TclSetAddrObj(Tcl_Obj *objPtr, const char *type, void *addr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_TclGetOpaqueFromObj(const Tcl_Obj *objPtr, const char *type, void **addrPtrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_TclSetOpaqueObj(Tcl_Obj *objPtr, const char *type, void *addr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_SetFromAnyProc Ns_TclSetFromAnyError;

/*
 * tclobjv.c
 */

NS_EXTERN int
Ns_ParseObjv(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec,
             Tcl_Interp *interp, int offset, int objc, Tcl_Obj *CONST* objv)
    NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ObjvProc Ns_ObjvArgs;
NS_EXTERN Ns_ObjvProc Ns_ObjvBool;
NS_EXTERN Ns_ObjvProc Ns_ObjvBreak;
NS_EXTERN Ns_ObjvProc Ns_ObjvByteArray;
NS_EXTERN Ns_ObjvProc Ns_ObjvDouble;
NS_EXTERN Ns_ObjvProc Ns_ObjvEval;
NS_EXTERN Ns_ObjvProc Ns_ObjvFlags;
NS_EXTERN Ns_ObjvProc Ns_ObjvIndex;
NS_EXTERN Ns_ObjvProc Ns_ObjvInt;
NS_EXTERN Ns_ObjvProc Ns_ObjvLong;
NS_EXTERN Ns_ObjvProc Ns_ObjvObj;
NS_EXTERN Ns_ObjvProc Ns_ObjvServer;
NS_EXTERN Ns_ObjvProc Ns_ObjvSet;
NS_EXTERN Ns_ObjvProc Ns_ObjvString;
NS_EXTERN Ns_ObjvProc Ns_ObjvTime;
NS_EXTERN Ns_ObjvProc Ns_ObjvWideInt;

#define Ns_NrElements(arr)  ((int) (sizeof(arr) / sizeof((arr)[0])))

/*
 * tclthread.c:
 */

NS_EXTERN int
Ns_TclThread(Tcl_Interp *interp, const char *script, Ns_Thread *thrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_TclDetachedThread(Tcl_Interp *interp, const char *script)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * tcltime.c
 */

NS_EXTERN Tcl_Obj*
Ns_TclNewTimeObj(const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN void
Ns_TclSetTimeObj(Tcl_Obj *objPtr, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_TclGetTimeFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Ns_Time *timePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_TclGetTimePtrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Ns_Time **timePtrPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * tclxkeylist.c:
 */

NS_EXTERN char *
Tcl_DeleteKeyedListField(Tcl_Interp  *interp, const char *fieldName,
			 const char *keyedList);

NS_EXTERN int
Tcl_GetKeyedListField(Tcl_Interp  *interp, const char *fieldName,
		      const char *keyedList, char **fieldValuePtr);

NS_EXTERN int
Tcl_GetKeyedListKeys(Tcl_Interp  *interp, char const *subFieldName,
		     const char *keyedList, int *keysArgcPtr, char ***keysArgvPtr);

NS_EXTERN char *
Tcl_SetKeyedListField(Tcl_Interp  *interp, const char *fieldName,
		      const char *fieldValue, const char *keyedList);

/*
 * listen.c:
 */

NS_EXTERN int
Ns_SockListenCallback(const char *addr, int port, Ns_SockProc *proc, void *arg)
    NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);


NS_EXTERN int
Ns_SockPortBound(int port);

/*
 * log.c:
 */

NS_EXTERN const char *
Ns_InfoErrorLog(void);

NS_EXTERN int
Ns_LogRoll(void);

NS_EXTERN void
Ns_Log(Ns_LogSeverity severity, const char *fmt, ...)
    NS_GNUC_NONNULL(2)
    NS_GNUC_PRINTF(2, 3);

NS_EXTERN void
Ns_VALog(Ns_LogSeverity severity, const char *fmt, va_list *const vaPtr)
    NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_Fatal(const char *fmt, ...)
    NS_GNUC_NONNULL(1)
    NS_GNUC_PRINTF(1, 2) NS_GNUC_NORETURN;

NS_EXTERN char *
Ns_LogTime(char *timeBuf)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_LogTime2(char *timeBuf, int gmt)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetLogFlushProc(Ns_LogFlushProc *procPtr) 
    NS_GNUC_DEPRECATED_FOR(Ns_AddLogFilter);

NS_EXTERN void
Ns_SetNsLogProc(Ns_LogProc *procPtr)  
    NS_GNUC_DEPRECATED_FOR(Ns_AddLogFilter);

NS_EXTERN void
Ns_AddLogFilter(Ns_LogFilter *procPtr, void *arg, Ns_Callback *freeProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_RemoveLogFilter(Ns_LogFilter *procPtr, void *const arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_LogSeverity
Ns_CreateLogSeverity(const char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_LogSeverityName(Ns_LogSeverity severity)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN bool
Ns_LogSeverityEnabled(Ns_LogSeverity severity);

NS_EXTERN bool
Ns_LogSeveritySetEnabled(Ns_LogSeverity severity, bool enabled);


/*
 * rollfile.c
 */

NS_EXTERN int
Ns_RollFile(const char *file, int max)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_PurgeFiles(const char *file, int max)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_RollFileByDate(const char *file, int max)
    NS_GNUC_NONNULL(1);

/*
 * nsmain.c:
 */

NS_EXTERN void
Nsd_LibInit(void);

NS_EXTERN int
Ns_Main(int argc, char *const*argv, Ns_ServerInitProc *initProc);

NS_EXTERN int
Ns_WaitForStartup(void);

NS_EXTERN void
Ns_StopServer(char *server);

/*
 * info.c:
 */

NS_EXTERN const char *
Ns_InfoHomePath(void);

NS_EXTERN char *
Ns_InfoServerName(void);

NS_EXTERN char *
Ns_InfoServerVersion(void);

NS_EXTERN const char *
Ns_InfoConfigFile(void);

NS_EXTERN pid_t
Ns_InfoPid(void);

NS_EXTERN char *
Ns_InfoNameOfExecutable(void);

NS_EXTERN char *
Ns_InfoPlatform(void);

NS_EXTERN long
Ns_InfoUptime(void);

NS_EXTERN time_t
Ns_InfoBootTime(void);

NS_EXTERN char *
Ns_InfoHostname(void);

NS_EXTERN char *
Ns_InfoAddress(void);

NS_EXTERN char *
Ns_InfoBuildDate(void);

NS_EXTERN int
Ns_InfoShutdownPending(void);

NS_EXTERN int
Ns_InfoStarted(void);

NS_EXTERN int
Ns_InfoServersStarted(void);

NS_EXTERN char *
Ns_InfoTag(void);

/*
 * mimetypes.c:
 */

NS_EXTERN char *
Ns_GetMimeType(const char *file)
    NS_GNUC_NONNULL(1)
    NS_GNUC_RETURNS_NONNULL;

/*
 * encoding.c:
 */

NS_EXTERN Tcl_Encoding
Ns_GetCharsetEncoding(const char *charset)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetCharsetEncodingEx(const char *charset, int len)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_GetEncodingCharset(Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetTypeEncoding(const char *mimeType)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetFileEncoding(const char *file)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetEncoding(const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(Ns_GetCharsetEncodingEx);


/*
 * modload.c:
 */

NS_EXTERN void
Ns_RegisterModule(const char *name, Ns_ModuleInitProc *proc)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ModuleLoad(Tcl_Interp *interp, const char *server, const char *module, const char *file,
              const char *init)
    NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

/*
 * nsthread.c:
 */

NS_EXTERN void
Ns_SetThreadServer(const char *server);

NS_EXTERN const char *
Ns_GetThreadServer(void);

/*
 * op.c:
 */

NS_EXTERN void
Ns_RegisterRequest(const char *server, const char *method, const char *url,
                   Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg, 
		   int unsigned flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_RegisterProxyRequest(const char *server, const char *method, const char *protocol,
                        Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_GetRequest(const char *server, const char *method, const char *url,
              Ns_OpProc **procPtr, Ns_Callback **deletePtr, void **argPtr,
              unsigned int *flagsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6)
    NS_GNUC_NONNULL(7);

NS_EXTERN void
Ns_UnRegisterRequest(const char *server, const char *method, const char *url,
                     int inherit)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterProxyRequest(const char *server, const char *method,
                          const char *protocol)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterRequestEx(const char *server, const char *method, const char *url,
                       unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnRunRequest(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnRedirect(Ns_Conn *conn, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * pathname.c:
 */

NS_EXTERN bool
Ns_PathIsAbsolute(const char *path)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_NormalizePath(Ns_DString *dsPtr, const char *path)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_MakePath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_HashPath(Ns_DString *dsPtr, const char *path, int levels)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_LibPath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_BinPath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_HomePath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_HomePathExists(const char *path, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_ModulePath(Ns_DString *dsPtr, const char *server, const char *module, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_ServerPath(Ns_DString *dsPtr, const char *server, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_PagePath(Ns_DString *dsPtr, const char *server, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_SetServerRootProc(Ns_ServerRootProc *proc, void *arg);

/*
 * proc.c:
 */

NS_EXTERN void
Ns_RegisterProcInfo(Ns_Callback procAddr, const char *desc, Ns_ArgProc *argProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_GetProcInfo(Tcl_DString *dsPtr, Ns_Callback procAddr, const void *arg)
    NS_GNUC_NONNULL(1);


NS_EXTERN void
Ns_StringArgProc(Tcl_DString *dsPtr, void *arg)
    NS_GNUC_NONNULL(1);


/*
 * queue.c:
 */

NS_EXTERN Ns_Conn *
Ns_GetConn(void);

/*
 * quotehtml.c:
 */

NS_EXTERN void
Ns_QuoteHtml(Ns_DString *dsPtr, const char *htmlString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * request.c:
 */

NS_EXTERN void
Ns_FreeRequest(Ns_Request *request);

NS_EXTERN void
Ns_ResetRequest(Ns_Request *request);

NS_EXTERN int
Ns_ParseRequest(Ns_Request *request, const char *line)
    NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_SkipUrl(const Ns_Request *request, int n)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetRequestUrl(Ns_Request *request, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ParseHeader(Ns_Set *set, const char *line, Ns_HeaderCaseDisposition disp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * return.c:
 */

NS_EXTERN void
Ns_ConnSetHeaders(const Ns_Conn *conn, const char *field, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnUpdateHeaders(const Ns_Conn *conn, const char *field, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnCondSetHeaders(const Ns_Conn *conn, const char *field, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnReplaceHeaders(Ns_Conn *conn, const Ns_Set *newheaders)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnPrintfHeaders(const Ns_Conn *conn, const char *field, const char *fmt, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PRINTF(3, 4);

NS_EXTERN void
Ns_ConnSetTypeHeader(const Ns_Conn *conn, const char *mimeType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetEncodedTypeHeader(Ns_Conn *conn, const char *mimeType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetLengthHeader(Ns_Conn *conn, size_t length, int doStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetLastModifiedHeader(const Ns_Conn *conn, const time_t *mtime)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetExpiresHeader(const Ns_Conn *conn, const char *expires)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnConstructHeaders(Ns_Conn *conn, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReturnNotice(Ns_Conn *conn, int status, const char *title,
                    const char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnAdminNotice(Ns_Conn *conn, int status, const char *title,
                         const char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnHtml(Ns_Conn *conn, int status, const char *html, ssize_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnCharData(Ns_Conn *conn, int status, const char *data, 
		      ssize_t len, const char *mimeType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnData(Ns_Conn *conn, int status, const char *data, 
		  ssize_t len, const char *mimeType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

NS_EXTERN int
Ns_ConnReturnOpenChannel(Ns_Conn *conn, int status, const char *mimeType,
                         Tcl_Channel chan, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnOpenFile(Ns_Conn *conn, int status, const char *mimeType,
                      FILE *fp, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnOpenFd(Ns_Conn *conn, int status, const char *mimeType, int fd, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnSetRequiredHeaders(Ns_Conn *conn, const char *mimeType, size_t length)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_ConnQueueHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN size_t
Ns_ConnFlushHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_ConnResetReturn(Ns_Conn *conn)
    NS_GNUC_DEPRECATED;

/*
 * returnresp.c:
 */

NS_EXTERN void
Ns_RegisterReturn(int status, const char *url);

NS_EXTERN int
Ns_ConnReturnStatus(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnOk(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNoResponse(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnRedirect(Ns_Conn *conn, const char *url)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnBadRequest(Ns_Conn *conn, const char *reason)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnHeaderLineTooLong(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnUnauthorized(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnForbidden(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnMoved(Ns_Conn *conn, const char *url)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNotFound(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNotModified(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnEntityTooLarge(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnNotImplemented(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnInternalError(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnRequestURITooLong(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnUnavailable(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

/*
 * tclvar.c
 */

NS_EXTERN int
Ns_VarGet(const char *server, const char *array, const char *key, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_VarExists(const char *server, const char *array, const char *key)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_VarSet(const char *server, const char *array, const char *key,
          const char *value, ssize_t len)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_VarUnset(const char *server, const char *array, const char *key)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Tcl_WideInt
Ns_VarIncr(const char *server, const char *array, const char *key, int incr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_VarAppend(const char *server, const char *array, const char *key,
             const char *value, ssize_t len)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

/*
 * sched.c:
 */

NS_EXTERN int
Ns_After(int delay, Ns_Callback *proc, void *arg, Ns_Callback *deleteProc)
    NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_Cancel(int id);

NS_EXTERN int
Ns_Pause(int id);

NS_EXTERN int
Ns_Resume(int id);

NS_EXTERN int
Ns_ScheduleProc(Ns_Callback *proc, void *arg, int thread, int interval)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ScheduleDaily(Ns_SchedProc *proc, void *clientData, unsigned int flags,
		 int hour, int minute, Ns_SchedProc *cleanupProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ScheduleWeekly(Ns_SchedProc *proc, void *clientData, unsigned int flags,
		  int day, int hour, int minute,
		  Ns_SchedProc *cleanupProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ScheduleProcEx(Ns_SchedProc *proc, void *clientData, unsigned int flags,
		  int interval, Ns_SchedProc *cleanupProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_UnscheduleProc(int id);

/*
 * set.c:
 */

NS_EXTERN void
Ns_SetUpdate(Ns_Set *set, const char *key, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_Set *
Ns_SetCreate(const char *name)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN void
Ns_SetFree(Ns_Set *set);

NS_EXTERN size_t
Ns_SetPut(Ns_Set *set, const char *key, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN size_t
Ns_SetPutSz(Ns_Set *set, const char *key, const char *value, ssize_t size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_SetUniqueCmp(const Ns_Set *set, const char *key,
                              int (*cmp) (CONST char *s1, CONST char *s2))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_SetFindCmp(const Ns_Set *set, const char *key,
	      int (*cmp) (const char *s1, const char *s2))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN char *
Ns_SetGetCmp(const Ns_Set *set, const char *key,
	     int (*cmp) (const char *s1, const char *s2))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_SetUnique(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_SetIUnique(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_SetFind(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_SetIFind(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_SetGet(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_SetIGet(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetTrunc(Ns_Set *set, size_t size)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetDelete(Ns_Set *set, int index)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetPutValue(const Ns_Set *set, size_t index, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_SetDeleteKey(Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetIDeleteKey(Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *
Ns_SetListFind(Ns_Set *const*sets, const char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Set **
Ns_SetSplit(const Ns_Set *set, char sep)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetListFree(Ns_Set **sets)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetMerge(Ns_Set *high, const Ns_Set *low)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *
Ns_SetCopy(const Ns_Set *old);

NS_EXTERN void
Ns_SetMove(Ns_Set *to, Ns_Set *from)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetPrint(const Ns_Set *set)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_SetGetValue(const Ns_Set *set, const char *key, const char *def)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_SetIGetValue(const Ns_Set *set, const char *key, const char *def)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


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

NS_EXTERN NS_SOCKET
Ns_SockListenEx(const char *address, int port, int backlog);

NS_EXTERN NS_SOCKET
Ns_SockListenUdp(const char *address, int port);

NS_EXTERN NS_SOCKET
Ns_SockListenRaw(int proto);

NS_EXTERN NS_SOCKET
Ns_SockListenUnix(const char *path, int backlog, int mode)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockBindUdp(const struct sockaddr_in *saPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockBindRaw(int proto);

NS_EXTERN NS_SOCKET
Ns_SockBindUnix(const char *path, int socktype, int mode)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
NsForkBinder(void);

NS_EXTERN void
NsStopBinder(void);

NS_EXTERN NS_SOCKET
Ns_SockBinderListen(int type, const char *address, int port, int options);

/*
 * sls.c
 */

NS_EXTERN void
Ns_SlsAlloc(Ns_Sls *slsPtr, Ns_Callback *cleanup)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SlsSet(const Ns_Sls *slsPtr, Ns_Sock *sock, void *data)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void *
Ns_SlsGet(const Ns_Sls *slsPtr, Ns_Sock *sock)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SlsSetKeyed(Ns_Sock *sock, const char *key, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN const char *
Ns_SlsGetKeyed(Ns_Sock *sock, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_SlsAppendKeyed(Ns_DString *dest, Ns_Sock *sock)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SlsUnsetKeyed(Ns_Sock *sock, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * sockfile.c:
 */

NS_EXTERN size_t
Ns_SetFileVec(Ns_FileVec *bufs, int i,  int fd, const void *data,
              off_t offset, size_t length)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ResetFileVec(Ns_FileVec *bufs, int nbufs, size_t sent)
    NS_GNUC_NONNULL(1);

NS_EXTERN ssize_t
Ns_SockSendFileBufs(Ns_Sock *sock, const Ns_FileVec *bufs, int nbufs,
                    const Ns_Time *timeoutPtr, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_SockCork(Ns_Sock *sock, bool cork)
    NS_GNUC_NONNULL(1);

/*
 * sock.c:
 */

NS_EXTERN size_t
Ns_SetVec(struct iovec *bufs, int i, const void *data, size_t len)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ResetVec(struct iovec *bufs, int nbufs, size_t sent)
    NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_SumVec(const struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_SockPipe(NS_SOCKET socks[2])
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_SockWait(NS_SOCKET sock, unsigned int what, int timeout);

NS_EXTERN int
Ns_SockTimedWait(NS_SOCKET sock, unsigned int what, const Ns_Time *timeoutPtr);

NS_EXTERN ssize_t
Ns_SockRecv(NS_SOCKET sock, void *buffer, size_t length, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(2);

NS_EXTERN ssize_t
Ns_SockSend(NS_SOCKET sock, const void *buffer, size_t length, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(2);

NS_EXTERN ssize_t
Ns_SockRecvBufs(NS_SOCKET sock, struct iovec *bufs, int nbufs,
		const Ns_Time *timeoutPtr, unsigned int flags);

NS_EXTERN ssize_t
Ns_SockSendBufs(Ns_Sock *sockPtr, const struct iovec *bufs, int nbufs,
		const Ns_Time *timeoutPtr, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_BindSock(const struct sockaddr_in *saPtr) 
    NS_GNUC_DEPRECATED_FOR(Ns_SockBind);

NS_EXTERN NS_SOCKET
Ns_SockBind(const struct sockaddr_in *saPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockListen(const char *address, int port);

NS_EXTERN NS_SOCKET
Ns_SockAccept(NS_SOCKET sock, struct sockaddr *saPtr, socklen_t *lenPtr);

NS_EXTERN NS_SOCKET
Ns_SockConnect(const char *host, int port)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockConnect2(const char *host, int port, const char *lhost, int lport)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockAsyncConnect(const char *host, int port)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockAsyncConnect2(const char *host, int port, const char *lhost, int lport)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockTimedConnect(const char *host, int port, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN NS_SOCKET
Ns_SockTimedConnect2(const char *host, int port, const char *lhost, int lport,
		     const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(5);

NS_EXTERN int
Ns_SockSetNonBlocking(NS_SOCKET sock);

NS_EXTERN int
Ns_SockSetBlocking(NS_SOCKET sock);

NS_EXTERN void
Ns_SockSetDeferAccept(NS_SOCKET sock, int secs);

NS_EXTERN int
Ns_GetSockAddr(struct sockaddr_in *saPtr, const char *host, int port)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_SockCloseLater(NS_SOCKET sock);

NS_EXTERN char *
Ns_SockError(void);

NS_EXTERN int
Ns_SockErrno(void);

NS_EXTERN void
Ns_ClearSockErrno(void);

NS_EXTERN int
Ns_GetSockErrno(void);

NS_EXTERN void
Ns_SetSockErrno(int err);

NS_EXTERN char *
Ns_SockStrError(int err);

#ifdef _WIN32
NS_EXTERN char *
NsWin32ErrMsg(DWORD err);

NS_EXTERN NS_SOCKET
ns_sockdup(NS_SOCKET sock);

NS_EXTERN int
ns_socknbclose(NS_SOCKET sock);
#endif

/*
 * sockcallback.c:
 */

NS_EXTERN int
Ns_SockCallback(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when);

NS_EXTERN int
Ns_SockCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when,
                  const Ns_Time *timeout, char const**threadNamePtr);

NS_EXTERN void
Ns_SockCancelCallback(NS_SOCKET sock);

NS_EXTERN int
Ns_SockCancelCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, char const**threadNamePtr);

/*
 * str.c:
 */

NS_EXTERN char *
Ns_StrTrim(char *chars)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_StrTrimLeft(char *chars)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_StrTrimRight(char *chars)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_StrToLower(char *chars)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_StrToUpper(char *chars)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_StrToInt(const char *chars, int *intPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_StrToWideInt(const char *chars, Tcl_WideInt *intPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_Match(const char *a, const char *b);

NS_EXTERN const char *
Ns_NextWord(const char *line)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_StrNStr(const char *chars, const char *subString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
     NS_GNUC_DEPRECATED_FOR(Ns_StrCaseFind);

NS_EXTERN const char *
Ns_StrCaseFind(const char *chars, const char *subString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_StrIsHost(const char *chars)
    NS_GNUC_NONNULL(1);

/*
 * tclcallbacks.c:
 */

NS_EXTERN Ns_TclCallback *
Ns_TclNewCallback(Tcl_Interp *interp, Ns_Callback *cbProc, Tcl_Obj *scriptObjPtr, int objc,
		  Tcl_Obj *CONST* objv)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_TclEvalCallback(Tcl_Interp *interp, const Ns_TclCallback *cbPtr,
		   Ns_DString *result, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(2);


NS_EXTERN Ns_Callback Ns_TclCallbackProc;
NS_EXTERN Ns_Callback Ns_TclFreeCallback;
NS_EXTERN Ns_ArgProc  Ns_TclCallbackArgProc;

/*
 * tclenv.c:
 */

NS_EXTERN char **
Ns_CopyEnviron(Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN char **
Ns_GetEnviron(void);

/*
 * tclfile.c:
 */

NS_EXTERN int
Ns_TclGetOpenChannel(Tcl_Interp *interp, const char *chanId, int write,
                     int check, Tcl_Channel *chanPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

NS_EXTERN int
Ns_TclGetOpenFd(Tcl_Interp *interp, const char *chanId, int write, int *fdPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);


/*
 * tclinit.c:
 */


NS_EXTERN int
Nsd_Init(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Interp *
Ns_TclCreateInterp(void);

NS_EXTERN int
Ns_TclInit(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TclEval(Ns_DString *dsPtr, const char *server, const char *script)
     NS_GNUC_NONNULL(3);

NS_EXTERN Tcl_Interp *
Ns_TclAllocateInterp(const char *server);

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
Ns_TclRegisterTrace(const char *server, Ns_TclTraceProc *proc, const void *arg, Ns_TclTraceType when)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_TclLibrary(const char *server);

NS_EXTERN const char *
Ns_TclInterpServer(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TclInitModule(const char *server, const char *module)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_FreeConnInterp(Ns_Conn *conn)
     NS_GNUC_DEPRECATED_FOR(NsFreeConnInterp);

NS_EXTERN int
Ns_TclRegisterAtCreate(Ns_TclTraceProc *proc, const void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(RegisterAt);

NS_EXTERN int
Ns_TclRegisterAtCleanup(Ns_TclTraceProc *proc, const void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(RegisterAt);

NS_EXTERN int
Ns_TclRegisterAtDelete(Ns_TclTraceProc *proc, const void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(RegisterAt);

NS_EXTERN int
Ns_TclInitInterps(const char *server, Ns_TclInterpInitProc *proc, const void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED_FOR(Ns_TclRegisterTrace);

NS_EXTERN void
Ns_TclRegisterDeferred(Tcl_Interp *interp, Ns_TclDeferProc *proc, void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED;


/*
 * tclhttp.c
 */
NS_EXTERN void
Ns_HttpCheckHeader(Ns_HttpTask *httpPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_HttpCheckSpool(Ns_HttpTask *httpPtr)
    NS_GNUC_NONNULL(1); 

NS_EXTERN int
Ns_HttpAppendBuffer(Ns_HttpTask *httpPtr, const char *buffer, size_t inSize) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2); 

/*
 * tclmisc.c
 */

NS_EXTERN int
Ns_SetNamedVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);


NS_EXTERN void Ns_TclPrintfResult(Tcl_Interp *interp, const char *fmt, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_PRINTF(2, 3);

NS_EXTERN const char *
Ns_TclLogErrorInfo(Tcl_Interp *interp, const char *extraInfo)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_TclLogError(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1)
    NS_GNUC_DEPRECATED_FOR(Ns_TclLoggErrorInfo);

NS_EXTERN const char *
Ns_TclLogErrorRequest(Tcl_Interp *interp, Ns_Conn *conn)
    NS_GNUC_NONNULL(1) 
    NS_GNUC_DEPRECATED_FOR(Ns_TclLoggErrorInfo);

NS_EXTERN void
Ns_LogDeprecated(Tcl_Obj *CONST* objv, int objc, const char *alternative, const char *explanation)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxMD5Init(Ns_CtxMD5 *ctx)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxMD5Update(Ns_CtxMD5 *ctx, unsigned const char *buf, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CtxMD5Final(Ns_CtxMD5 *ctx, unsigned char digest[16])
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CtxSHAInit(Ns_CtxSHA1 *ctx)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxSHAUpdate(Ns_CtxSHA1 *ctx, const unsigned char *buf, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CtxSHAFinal(Ns_CtxSHA1 *ctx, unsigned char digest[20])
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxString(const unsigned char *digest, char *buf, int size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * tclrequest.c:
 */

NS_EXTERN int
Ns_TclRequest(Ns_Conn *conn, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * tclset.c:
 */

NS_EXTERN int Ns_TclEnterSet(Tcl_Interp *interp, Ns_Set *set, unsigned int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *Ns_TclGetSet(Tcl_Interp *interp, const char *setId)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int Ns_TclGetSet2(Tcl_Interp *interp, const char *setId, Ns_Set **setPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int Ns_TclFreeSet(Tcl_Interp *interp, const char *setId)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * httptime.c:
 */

NS_EXTERN char *
Ns_HttpTime(Ns_DString *dsPtr, const time_t *when)
    NS_GNUC_NONNULL(1);

NS_EXTERN time_t
Ns_ParseHttpTime(char *chars)
    NS_GNUC_NONNULL(1);

/*
 * url.c:
 */

NS_EXTERN const char *
Ns_RelativeUrl(const char *url, const char *location);

NS_EXTERN int
Ns_ParseUrl(char *url, char **pprotocol, char **phost, char **pport,
            char **ppath, char **ptail)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4)
    NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

NS_EXTERN int
Ns_AbsoluteUrl(Ns_DString *dsPtr, const char *url, const char *base)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * url2file.c:
 */

NS_EXTERN void
Ns_RegisterUrl2FileProc(const char *server, const char *url,
                        Ns_Url2FileProc *proc, Ns_Callback *deleteCallback,
                        void *arg, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterUrl2FileProc(const char *server, const char *url, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_UrlToFile(Ns_DString *dsPtr, const char *server, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_SetUrlToFileProc(const char *server, Ns_UrlToFileProc *procPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


NS_EXTERN Ns_Url2FileProc Ns_FastUrl2FileProc;


/*
 * urlencode.c:
 */

NS_EXTERN Tcl_Encoding
Ns_GetUrlEncoding(const char *charset);

NS_EXTERN char *
Ns_UrlPathEncode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_UrlPathDecode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_UrlQueryEncode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_UrlQueryDecode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_EncodeUrlWithEncoding(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding) 
    NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryEncode)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DecodeUrlWithEncoding(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding) 
     NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryDecode)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_EncodeUrlCharset(Ns_DString *dsPtr, const char *urlSegment, const char *charset) 
     NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryEncode)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DecodeUrlCharset(Ns_DString *dsPtr, const char *urlSegment, const char *charset) 
     NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryDecode)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * urlopen.c:
 */

NS_EXTERN int
Ns_FetchPage(Ns_DString *dsPtr, const char *url, const char *server)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_FetchURL(Ns_DString *dsPtr, const char *url, Ns_Set *headers)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * urlspace.c:
 */

NS_EXTERN int
Ns_UrlSpecificAlloc(void);

NS_EXTERN void
Ns_UrlSpecificWalk(int id, const char *server, Ns_ArgProc func, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_UrlSpecificSet(const char *server, const char *method, const char *url, int id,
                  void *data, unsigned int flags, void (*deletefunc)(void *data))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

NS_EXTERN void *
Ns_UrlSpecificGet(const char *server, const char *method, const char *url, int id)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void *
Ns_UrlSpecificGetFast(const char *server, const char *method, const char *url, int id)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void *
Ns_UrlSpecificGetExact(const char *server, const char *method, const char *url,
                       int id, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void *
Ns_UrlSpecificDestroy(const char *server, const char *method, const char *url,
                      int id, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * fd.c:
 */

NS_EXTERN int
Ns_CloseOnExec(int fd);

NS_EXTERN int
Ns_NoCloseOnExec(int fd);

NS_EXTERN int
Ns_DupHigh(int *fdPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_GetTemp(void);

NS_EXTERN void
Ns_ReleaseTemp(int fd);

/*
 * unix.c, win32.c:
 */

NS_EXTERN int
ns_sockpair(NS_SOCKET *socks)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
ns_sock_set_blocking(NS_SOCKET sock, bool blocking);

NS_EXTERN int
ns_pipe(int *fds)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
ns_mkstemp(char *template);

NS_EXTERN int
ns_poll(struct pollfd *fds, NS_POLL_NFDS_TYPE nfds, int timo)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_GetNameForUid(Ns_DString *dsPtr, int uid)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_GetNameForGid(Ns_DString *dsPtr, int gid);

NS_EXTERN bool
Ns_GetUserHome(Ns_DString *dsPtr, const char *user)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_GetUserGid(const char *user)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_GetUid(const char *user)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_GetGid(const char *group)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_SetUser(const char *user);

NS_EXTERN int
Ns_SetGroup(const char *group);

/*
 * form.c:
 */
NS_EXTERN Ns_Set  *
Ns_ConnGetQuery(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnClearQuery(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_QueryToSet(char *query, Ns_Set *set)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#endif /* NS_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
