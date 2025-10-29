/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * The Initial Developer of the Original Code and related documentation
 * is America Online, Inc. Portions created by AOL are Copyright (C) 1999
 * America Online, Inc. All Rights Reserved.
 *
 */

/*
 * ns.h --
 *
 *      All the public types and function declarations for the core server.
 *
 */

#ifndef NS_H
#define NS_H

#if defined(_MSC_VER) && !defined(HAVE_CONFIG_H)
# include "nsversion-win32.h"
#else
# include "nsversion.h"
#endif

#include "nsthread.h"

#ifdef HAVE_ZLIB_H
# include <zlib.h>
#endif

#ifdef HAVE_OPENSSL_EVP_H
# include <openssl/ssl.h>
# define NS_TLS_SSL_CTX SSL_CTX
# define NS_TLS_SSL SSL
#else
# define NS_TLS_SSL_CTX void*
# define NS_TLS_SSL void*
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
 * The following describe various properties of a connection. Used in the
 * public interface in e.g. Ns_ConnWriteVChars() or Ns_ConnWriteData()
 */

#define NS_CONN_CLOSED               0x001u /* The underlying socket is closed */

/*
 * This is defined as a macro for cases where we need to check
 * if connection is still writable (was not closed implicitly
 * by some call to Ns_ConnSendData et al.).
 */

#define Ns_ConnIsClosed(conn) (((conn)->flags & NS_CONN_CLOSED) != 0u)

#define NS_CONN_SKIPHDRS              0x002u /* Client is HTTP/0.9, do not send HTTP headers  */
#define NS_CONN_SKIPBODY              0x004u /* HTTP HEAD request, do not send body */
#define NS_CONN_READHDRS              0x008u /* Unused */
#define NS_CONN_SENTHDRS              0x010u /* Response headers have been sent to client */
#define NS_CONN_WRITE_ENCODED         0x020u /* Character data mode requested mime-type header. */
#define NS_CONN_STREAM                0x040u /* Data is to be streamed when ready.  */
#define NS_CONN_STREAM_CLOSE          0x080u /* Writer Stream should be closed.  */
#define NS_CONN_CHUNK                 0x100u /* Streamed data is to be chunked. */
#define NS_CONN_SENT_LAST_CHUNK       0x200u /* Marks that the last chunk was sent in chunked mode */
#define NS_CONN_SENT_VIA_WRITER       0x400u /* Response data has been sent via writer thread */
#define NS_CONN_SOCK_CORKED           0x800u /* Underlying socket is corked */
#define NS_CONN_SOCK_WAITING        0x01000u /* Connection pushed to waiting list */
#define NS_CONN_ZIPACCEPTED         0x10000u /* The request accepts zip compression */
#define NS_CONN_BROTLIACCEPTED      0x20000u /* The request accept brotli compression */
#define NS_CONN_CONTINUE            0x40000u /* The request got "Expect: 100-continue" */
#define NS_CONN_ENTITYTOOLARGE    0x0100000u /* The sent entity was too large */
#define NS_CONN_REQUESTURITOOLONG 0x0200000u /* Request-URI too long */
#define NS_CONN_LINETOOLONG       0x0400000u /* Request header line too long */
#define NS_CONN_CONFIGURED        0x1000000u /* The connection is fully configured */
#define NS_CONN_SSL_WANT_WRITE    0x2000000u /* Flag SSL_ERROR_WANT_WRITE */


/*
 * Cookie creation options.  For NaviServer and the current set of NaviServer
 * modules, these constants would not be needed here. As long we have
 * Ns_ConnSetCookieEx() in the public interface, we these flags here as well.
 */
#define NS_COOKIE_SECURE           0x01u  /* The cookie should only be sent using HTTPS */
#define NS_COOKIE_SCRIPTABLE       0x02u  /* Available to JavaScript on the client. */
#define NS_COOKIE_DISCARD          0x04u  /* Discard the cookie at the end of the current session. */
#define NS_COOKIE_REPLACE          0x08u  /* Replace the cookie in the output headers. */
#define NS_COOKIE_EXPIRENOW        0x10u  /* Used for deletion of cookies. */
#define NS_COOKIE_SAMESITE_STRICT  0x20u  /* Cookies sent in a first-party context  */
#define NS_COOKIE_SAMESITE_LAX     0x40u  /* Cookies are sent with top-level navigations */
#define NS_COOKIE_SAMESITE_NONE    0x80u  /* Cookies sent in all contexts */

/*
 * The following are the valid attributes of a scheduled event. For NaviServer
 * and the current set of NaviServer modules, these constants would not be
 * needed here. As long Ns_ScheduleProcEx() is in the public interface and
 * uses the flags, we need these constants here as well.
 */
#define NS_SCHED_THREAD            0x01u /* Ns_SchedProc will run in detached thread */
#define NS_SCHED_ONCE              0x02u /* Call cleanup proc after running once */
#define NS_SCHED_DAILY             0x04u /* Event is scheduled to occur daily */
#define NS_SCHED_WEEKLY            0x08u /* Event is scheduled to occur weekly */
#define NS_SCHED_PAUSED            0x10u /* Event is currently paused */
#define NS_SCHED_RUNNING           0x20u /* Event is currently running, perhaps in detached thread */

/*
 * The following are valid options when manipulating
 * URL specific data.
 */
#define NS_OP_NOINHERIT            0x02u /* Match URL exactly */
#define NS_OP_NODELETE             0x04u /* Do call previous procs Ns_OpDeleteProc */
#define NS_OP_RECURSE              0x08u /* Also destroy registered procs below given URL */
#define NS_OP_ALLCONSTRAINTS       0x10u /* Also destroy all filters for this node */
#define NS_OP_SEGMENT_MATCH        0x20u /* Also destroy all filters for this node */


/*
 * The following types of filters may be registered.
 */
typedef enum {
    NS_FILTER_PRE_AUTH =        0x01u, /* Runs before any Ns_AuthRequestProc */
    NS_FILTER_POST_AUTH =       0x02u, /* Runs after any Ns_AuthRequestProc */
    NS_FILTER_TRACE =           0x04u, /* Runs after Ns_OpProc completes successfully */
    NS_FILTER_VOID_TRACE =      0x08u  /* Run ns_register_trace procs after previous traces */
} Ns_FilterType;


/*
 * The following define socket events for the Ns_Sock* APIs.
 */
typedef enum {
    NS_SOCK_NONE =            0x0000u, /* No value provided */
    NS_SOCK_READ =            0x0001u, /* Socket is readable */
    NS_SOCK_WRITE =           0x0002u, /* Socket is writable */
    NS_SOCK_EXCEPTION =       0x0004u, /* Socket is in an error state */
    NS_SOCK_EXIT =            0x0008u, /* The server is shutting down */
    NS_SOCK_DONE =            0x0010u, /* Task processing is done */
    NS_SOCK_CANCEL =          0x0020u, /* Remove event from sock callback thread */
    NS_SOCK_TIMEOUT =         0x0040u, /* Timeout waiting for socket event. */
    NS_SOCK_AGAIN =           0x0080u, /* Try AGAIN */
    NS_SOCK_INIT =            0x0100u  /* Initialise a Task callback. */
} Ns_SockState;

/*
 * Many of sock-states are just from the Ns_EventQueue or Ns_Task
 * interface. It is probably a good idea to define different types for these
 * interfaces, or to define e.g. SockConditions like the following
 *

typedef enum {
    NS_SOCK_COND_READ =         NS_SOCK_READ,
    NS_SOCK_COND_WRITE =        NS_SOCK_WRITE,
    NS_SOCK_COND_EXCEPTION =    NS_SOCK_EXCEPTION,
    NS_SOCK_COND_EXIT =         NS_SOCK_EXIT
} Ns_SockCondition;
*/

#define NS_SOCK_ANY                ((unsigned int)NS_SOCK_READ|(unsigned int)NS_SOCK_WRITE|(unsigned int)NS_SOCK_EXCEPTION)

/*
 * The following are valid network driver options.
 */
#define NS_DRIVER_ASYNC            0x01u /* Use async read-ahead. */
#define NS_DRIVER_SSL              0x02u /* Use SSL port, protocol defaults. */
#define NS_DRIVER_NOPARSE          0x04u /* Do not parse request */
#define NS_DRIVER_UDP              0x08u /* UDP, can't use stream socket options */
#define NS_DRIVER_CAN_USE_SENDFILE 0x10u /* Allow to send clear text via sendfile */
#define NS_DRIVER_SNI              0x20u /* SNI - just used when NS_DRIVER_SSL is set as well */
#define NS_DRIVER_QUIC             0x40u /* Use OSSL_QUIC_server_method */

#define NS_DRIVER_VERSION_1        1    /* Obsolete. */
#define NS_DRIVER_VERSION_2        2    /* IPv4 only */
#define NS_DRIVER_VERSION_3        3    /* IPv4 and IPv6 */
#define NS_DRIVER_VERSION_4        4    /* Client support, current version */
#define NS_DRIVER_VERSION_5        5    /* Library info, current connection info */
#define NS_DRIVER_VERSION_6        6    /* driverThreadProc, headersEncodeProc */

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
    NS_TCL_TRACE_FREECONN     = 0x20u, /* Interp finished connection processing */
    NS_TCL_TRACE_IDLE         = 0x40u  /* Interp (connthread) idle */
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
typedef enum {
    NS_TCL_SET_STATIC,          /* The Ns_Set is deleted, when the interp is freed */
    NS_TCL_SET_DYNAMIC          /* The Ns_Set is deleted at the end of a request (or via "ns_set free|cleanup") */
} Ns_TclSetType;

/*
 * C API macros.
 */

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
 * Ns_DStrings are now equivalent to Tcl_DStrings starting in 4.0.
 */
#define Ns_DString                 Tcl_DString
#define NS_DSTRING_STATIC_SIZE     (TCL_DSTRING_STATIC_SIZE)
#define NS_DSTRING_PRINTF_MAX      2048

/*
 * Typedefs of variables
 */

typedef struct Ns_CacheSearch {
    Ns_Time        now;
    Tcl_HashSearch hsearch;
} Ns_CacheSearch;

typedef struct Ns_Cache         Ns_Cache;
typedef struct Ns_Entry         Ns_Entry;
typedef uintptr_t               Ns_Cls;
typedef uintptr_t               Ns_Sls;
typedef void                    Ns_OpContext;
typedef struct Ns_TaskQueue     Ns_TaskQueue;
typedef struct Ns_Task          Ns_Task;
typedef struct Ns_EventQueue    Ns_EventQueue;
typedef struct Ns_Event         Ns_Event;
typedef struct Ns_Server        Ns_Server;
typedef struct Ns_Set           Ns_Set;

#define NS_CACHE_MAX_TRANSACTION_DEPTH 16

typedef struct Ns_CacheTransactionStack {
    uintptr_t    stack[NS_CACHE_MAX_TRANSACTION_DEPTH];
    int          uncommitted[NS_CACHE_MAX_TRANSACTION_DEPTH];
    unsigned int depth;
} Ns_CacheTransactionStack;


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
    Deprecated,
    Dev,
    Security,
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
 * Global variables:
 *
 *  LogSeverity, which can be used from modules (e.g. nsssl)
 *
 */

NS_EXTERN Ns_LogSeverity Ns_LogTaskDebug;   /* Severity at which to log verbose. */
NS_EXTERN Tcl_Encoding   NS_utf8Encoding;   /* Cached UTF-8 encoding */

#if defined(_MSC_VER) && !defined(Ns_LogAccessDebug_DEFINED_ALREADY)
extern NS_IMPORT  Ns_LogSeverity Ns_LogAccessDebug;
#elif defined(_MSC_VER)
#else
NS_EXTERN Ns_LogSeverity Ns_LogAccessDebug;
#endif

struct Ns_ObjvSpec;
struct Ns_Conn;


/*
 * Typedefs of functions
 */

typedef int           (Ns_SortProc)(void *left, void *right);
typedef bool          (Ns_EqualProc)(void *left, void *right);
typedef void          (Ns_ElemVoidProc)(void *elem);
typedef void *        (Ns_ElemValProc)(void *elem);
typedef bool          (Ns_ElemTestProc)(void *elem);
typedef void          (Ns_Callback) (void *arg);
typedef Ns_ReturnCode (Ns_LogCallbackProc)(void *arg);
typedef void          (Ns_FreeProc)(void *arg);
typedef void          (Ns_ShutdownProc)(const Ns_Time *toPtr, void *arg);
typedef int           (Ns_TclInterpInitProc)(Tcl_Interp *interp, const void *arg);
typedef int           (Ns_TclTraceProc)(Tcl_Interp *interp, const void *arg);
typedef void          (Ns_TclDeferProc)(Tcl_Interp *interp, void *arg);
typedef bool          (Ns_SockProc)(NS_SOCKET sock, void *arg, unsigned int why);
typedef void          (Ns_TaskProc)(Ns_Task *task, NS_SOCKET sock, void *arg,
                                    Ns_SockState why);
typedef void          (Ns_EventProc)(Ns_Event *event, NS_SOCKET sock, void *arg,
                                     Ns_Time *now, Ns_SockState why);
typedef void          (Ns_SchedProc)(void *arg, int id);
typedef Ns_ReturnCode (Ns_ServerInitProc)(const char *server);
typedef Ns_ReturnCode (Ns_ModuleInitProc)(const char *server, const char *module)
    NS_GNUC_NONNULL(2);
typedef void          (Ns_AdpParserProc)(Tcl_DString *outPtr, char *page);
typedef Ns_ReturnCode (Ns_AuthorizeRequestProc)(void *arg,
                                                struct Ns_Conn *conn,
                                                int *continuationPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
typedef Ns_ReturnCode (Ns_AuthorizeUserProc)(void *arg, const Ns_Server *servPtr,
                                             const char *user, const char *passwd,
                                             int *continuationPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
typedef int           (Ns_ObjvProc)(struct Ns_ObjvSpec *spec, Tcl_Interp *interp,
                                    TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
    NS_GNUC_NONNULL(1);
typedef int           (Ns_IndexCmpProc) (const void *left, const void *right)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
typedef int           (Ns_IndexKeyCmpProc) (const void *key, const void *elemPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
typedef bool (Ns_UrlSpaceContextFilterEvalProc) (void *contextSpec, void *context);

typedef bool (Ns_HeadersEncodeProc)(
    struct Ns_Conn     *conn,
    const Ns_Set       *merged,     /* merged, sanitized headers to encode */
    void               *out_obj,    /* backend-defined sink */
    size_t             *out_len     /* optional: item count or bytes written */
) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * Generic function pointer type, can be used for recasting between different
 * function types.
 */
typedef void (*ns_funcptr_t)(void);


/*
 * The field of a key-value data structure.
 */

typedef struct Ns_SetField {
    char *name;
    char *value;
} Ns_SetField;

/*
 * Ns_Set: the key-value data structure.
 */
#if NS_MAJOR_VERSION > 4
# define NS_SET_DSTRING 1
#endif

/* #define NS_SET_DSTRING 1 */
/* #define NS_SET_DEBUG 1 */
/*
 * Activate named ns_sets for the time being, to ease potential debugging. The code
 * is slightly faster, when this is deactivated (names are as all malloced).
 */
#define NS_SET_WITH_NAMES 1

#ifdef NS_SET_WITH_NAMES
# define NS_SET_NAME_AUTH "auth"
# define NS_SET_NAME_CLIENT_RESPONSE "client-response"
# define NS_SET_NAME_DB "db"
# define NS_SET_NAME_MP "mp"
# define NS_SET_NAME_PARSEQ "parseq"
# define NS_SET_NAME_QUERY "query"
# define NS_SET_NAME_REQUEST "request headers"
# define NS_SET_NAME_RESPONSE "response headers"
#else
# define NS_SET_NAME_AUTH NULL
# define NS_SET_NAME_CLIENT_RESPONSE NULL
# define NS_SET_NAME_DB NULL
# define NS_SET_NAME_MP NULL
# define NS_SET_NAME_PARSEQ NULL
# define NS_SET_NAME_QUERY NULL
# define NS_SET_NAME_REQUEST NULL
# define NS_SET_NAME_RESPONSE NULL
#endif

#define NS_SET_OPTION_NOCASE 0x01

struct Ns_Set {
    const char  *name;
    size_t       size;
    size_t       maxSize;
#ifdef NS_SET_DSTRING
    Tcl_DString  data;
#endif
    Ns_SetField *fields;
    unsigned int flags;
};

/*
 * The request structure.
 */

typedef enum {
    NS_REQUEST_TYPE_PLAIN =       0x00u, /* request target is path              */
    NS_REQUEST_TYPE_PROXY =       0x01u, /* request target has scheme+host+port */
    NS_REQUEST_TYPE_CONNECT =     0x02u, /* request target has host+port        */
    NS_REQUEST_TYPE_ASTERISK =    0x03u  /* asterisk-form, just a '*' character */
} Ns_RequestType;

typedef struct Ns_Request {
    const char     *line;
    const char     *method;
    const char     *protocol;
    const char     *host;
    const char     *url;
    const char     *urlv;
    char           *query;
    const char     *fragment;
    const char     *serverRoot;
    TCL_SIZE_T      url_len;
    TCL_SIZE_T      urlv_len;
    TCL_SIZE_T      urlc;
    Ns_RequestType  requestType;
    unsigned short  port;
    double          version;
} Ns_Request;

/*
 * Typedef for URL components
 */
typedef struct Ns_URL {
    const char *protocol;
    const char *userinfo;
    const char *host;
    const char *port;
    const char *path;
    const char *tail;
    const char *query;
    const char *fragment;
} Ns_URL;

/*
 * Match-information from UrlSpaceMatches
 */
typedef struct Ns_UrlSpaceMatchInfo {
    ssize_t offset;
    size_t  segmentLength;
    bool    isSegmentMatch;
} Ns_UrlSpaceMatchInfo;

typedef enum {
    NS_URLSPACE_DEFAULT =        0,
    NS_URLSPACE_FAST =           1,
    NS_URLSPACE_EXACT =          2
} Ns_UrlSpaceOp;

/*
 * The connection structure.
 */

typedef struct Ns_Conn {
    Ns_Request   request;
    Ns_Set      *headers;
    Ns_Set      *outputheaders;
    Ns_Set      *auth;
    size_t       contentLength;
    unsigned int flags;
} Ns_Conn;

/*
 * The index data structure.  This is a linear array of values.
 */

typedef struct Ns_Index {
    void            **el;
    Ns_IndexCmpProc  *CmpEls;
    Ns_IndexCmpProc  *CmpKeyWithEl;
    size_t            n;
    size_t            max;
    size_t            inc;
} Ns_Index;

typedef struct Ns_IndexContextSpec {
    Ns_FreeProc *freeProc;
    void        *data;
    Ns_FreeProc *dataFreeProc;
} Ns_IndexContextSpec;

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
    const char      *key;
    Ns_ObjvProc     *proc;
    void            *dest;
    void            *arg;
} Ns_ObjvSpec;

typedef struct Ns_ObjvValueRange {
    Tcl_WideInt minValue;
    Tcl_WideInt maxValue;
} Ns_ObjvValueRange;

typedef struct Ns_ObjvTimeRange {
    Ns_Time minValue;
    Ns_Time maxValue;
} Ns_ObjvTimeRange;


/*
 * The following struct is used to validate options from
 * a choice of values.
 */

typedef struct Ns_ObjvTable {
    const char      *key;
    unsigned int     value;
} Ns_ObjvTable;

/*
 * The following struct is used to define a command with subcmds.
 */
typedef struct Ns_SubCmdSpec {
    const char      *key;
    TCL_OBJCMDPROC_T  *proc;
} Ns_SubCmdSpec;


/*
 * The following structure defines the Tcl code to run
 * for a callback function.
 */

typedef struct Ns_TclCallback {
    ns_funcptr_t     cbProc;
    const char      *server;
    const Ns_Server *servPtr;
    const char      *script;
    char           **argv;
    TCL_SIZE_T       argc;
    char            *args[1];
} Ns_TclCallback;

/*
 * The following structure defines a driver.
 */

typedef struct Ns_Driver {
    void       *arg;           /* Driver callback data. */
    const char *server;        /* Virtual server name. */
    const char *type;          /* Type of driver, e.g. "nssock" */
    const char *moduleName;    /* Module name, e.g. "nssock1" */
    const char *threadName;    /* Thread name, e.g. "nssock1:1" */
    const char *location;      /* Location, e.g, "http://foo:9090" */
    const char *address;       /* Address in location, e.g. "foo" */
    const char *protocol;      /* Protocol in location, e.g, "http" */
    Ns_Time     sendwait;      /* send() I/O timeout in seconds */
    Ns_Time     recvwait;      /* recv() I/O timeout in seconds */
    size_t      bufsize;       /* Conn bufsize (0 for SSL) */
    const Ns_Set *extraHeaders;  /* Extra header fields added for every request */
} Ns_Driver;

/*
 * The following structure defines the public
 * parts of the driver socket connection.
 */

typedef struct Ns_Sock {
    NS_SOCKET                   sock;     /* Connection socket */
    Ns_Driver                  *driver;
    void                       *arg;      /* Driver context. */
    struct NS_SOCKADDR_STORAGE  sa;       /* Actual peer address */
} Ns_Sock;

/*
 * The following structure defines a range of bytes to send from a
 * file or memory location. The descriptor fd must be a normal file
 * in the filesystem, not a socket.
 */

typedef struct Ns_FileVec {
    const char *buffer;  /* Optional memory buffer, when fd is invalid */
    size_t      length;  /* Number of bytes to send from offset. */
    off_t       offset;  /* Offset for file (or buffer) */
    int         fd;      /* File descriptor of file to send, or < 0 for memory. */
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
(Ns_DriverListenProc)(Ns_Driver *driver, const char *address, unsigned short port, int backlog, bool reusePort)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef NS_DRIVER_ACCEPT_STATUS
(Ns_DriverAcceptProc)(Ns_Sock *sock, NS_SOCKET listensock,
                      struct sockaddr *saPtr, socklen_t *socklen)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

typedef ssize_t
(Ns_DriverRecvProc)(Ns_Sock *sock, struct iovec *bufs, int nbufs,
                    Ns_Time *timeoutPtr, unsigned int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef ssize_t
(Ns_DriverSendProc)(Ns_Sock *sock, const struct iovec *bufs, int nbufs, unsigned int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef ssize_t
(Ns_DriverSendFileProc)(Ns_Sock *sock, Ns_FileVec *bufs, int nbufs, unsigned int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef Ns_ReturnCode
(Ns_DriverRequestProc)(void *arg, Ns_Conn *conn)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef bool
(Ns_DriverKeepProc)(Ns_Sock *sock)
     NS_GNUC_NONNULL(1);

typedef void
(Ns_DriverCloseProc)(Ns_Sock *sock)
     NS_GNUC_NONNULL(1);

typedef int
(Ns_DriverClientInitProc)(Tcl_Interp *interp, Ns_Sock *sock, void* arg)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

typedef Tcl_Obj *
(Ns_DriverConnInfoProc)(Ns_Sock *sock);

typedef struct Ns_DriverClientInitArg {
    NS_TLS_SSL_CTX *ctx;
    const char *sniHostname;
    const char *caFile;
    const char *caPath;
} Ns_DriverClientInitArg;

/*
 * The following structure defines the values to initialize the driver. This is
 * passed to Ns_DriverInit.
 */

typedef struct Ns_DriverInitData {
    const char              *name;             /* This will show up in log file entries */
    Ns_DriverListenProc     *listenProc;       /* Open listening socket for conns. */
    Ns_DriverAcceptProc     *acceptProc;       /* Accept a new nonblocking socket. */
    Ns_DriverRecvProc       *recvProc;         /* Read bytes from conn into iovec. */
    Ns_DriverSendProc       *sendProc;         /* Write bytes to conn from iovec. */
    Ns_DriverSendFileProc   *sendFileProc;     /* Optional: write bytes from files/buffers. */
    Ns_DriverKeepProc       *keepProc;         /* Keep a socket open after conn done? */
    Ns_DriverRequestProc    *requestProc;      /* First proc to be called by a connection thread. */
    Ns_DriverCloseProc      *closeProc;        /* Close a connection socket. */
    Ns_DriverClientInitProc *clientInitProc;   /* Initialize a client connection */
    int                      version;          /* Version 4. */
    unsigned int             opts;             /* NS_DRIVER_ASYNC | NS_DRIVER_SSL  */
    void                    *arg;              /* Module's driver callback data */
    const char              *path;             /* Path to find config parameter such as port, address, etc. */
    const char              *protocol;         /* Protocol */
    unsigned short           defaultPort;      /* Default port */
    Ns_DriverConnInfoProc   *connInfoProc;     /* NS_DRIVER_VERSION_5: Obtain information about a connection */
    const char              *libraryVersion;   /* NS_DRIVER_VERSION_5: Version of the used library */
    Ns_ThreadProc           *driverThreadProc; /* NS_DRIVER_VERSION_6: event loop */
    Ns_HeadersEncodeProc    *headersEncodeProc;/* NS_DRIVER_VERSION_6: encode headers from Ns_Set */
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

typedef void (Ns_WalkProc)
    (Tcl_DString *dsPtr, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef Ns_ReturnCode (Ns_OpProc)
    (const void *arg, Ns_Conn *conn)
    NS_GNUC_NONNULL(2);

typedef void (Ns_TraceProc)
    (void *arg, Ns_Conn *conn);

typedef Ns_ReturnCode (Ns_FilterProc)
    (const void *arg, Ns_Conn *conn, Ns_FilterType why);

typedef Ns_ReturnCode (Ns_LogFilter)
    (void *arg, Ns_LogSeverity severity, const Ns_Time *stamp, const char *msg, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

typedef Ns_ReturnCode (Ns_UrlToFileProc)
    (Tcl_DString *dsPtr, const char *server, const char *url);

typedef Ns_ReturnCode (Ns_Url2FileProc)
    (Tcl_DString *dsPtr, const char *url, const void *arg);

typedef const char* (Ns_ServerRootProc)
    (Tcl_DString *dest, const char *host, const void *arg);

typedef char* (Ns_ConnLocationProc)
    (Ns_Conn *conn, Tcl_DString *dest, const Ns_TclCallback *cbPtr);

#ifdef NS_WITH_DEPRECATED
typedef int (Ns_LogProc)               /* Deprecated */
    (Tcl_DString *dsPtr, Ns_LogSeverity severity, const char *fmt, va_list ap);

typedef int (Ns_LogFlushProc)          /* Deprecated */
    (const char *msg, size_t len);

typedef char *(Ns_LocationProc)        /* Deprecated */
    (Ns_Conn *conn);
#endif

/*
 * adpcmds.c:
 */

NS_EXTERN int
Ns_AdpAppend(Tcl_Interp *interp, const char *buf, TCL_SIZE_T len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpGetOutput(Tcl_Interp *interp, Tcl_DString **dsPtrPtr,
                int *doStreamPtr, size_t *maxBufferPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * adprequest.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_AdpRequest(Ns_Conn *conn, const char *fileName)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_AdpRequestEx(Ns_Conn *conn, const char *fileName, const Ns_Time *expiresPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpFlush(Tcl_Interp *interp, bool doStream)
    NS_GNUC_NONNULL(1);

/*
 * auth.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_AuthorizeRequest(Ns_Conn *conn, const char **authorityPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

//NS_EXTERN Ns_ReturnCode
//Ns_AuthorizeUser(Ns_Conn *conn, const char **authorityPtr)
//    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_AuthorizeUser(const Ns_Server *server, const char *user, const char *passwd,
                 const char ** authorityPtr)
NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)  NS_GNUC_NONNULL(4);

NS_EXTERN void *
Ns_RegisterAuthorizeRequest(const char *server, Ns_AuthorizeRequestProc *proc,
                            void *arg, const char *authority, bool first)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void *
Ns_RegisterAuthorizeUser(const char *server, Ns_AuthorizeUserProc *proc,
                         void *arg, const char *authority, bool first)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


#if 0
NS_EXTERN void
Ns_SetRequestAuthorizeProc(const char *server, Ns_AuthorizeRequestProc *procPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetUserAuthorizeProc(const char *server, Ns_AuthorizeUserProc *procPtr)
    NS_GNUC_NONNULL(1);
#endif

NS_EXTERN Ns_ReturnCode
Ns_AuthDigestValidate(const Ns_Set *UNUSED(auth), const char *UNUSED(storedPwd))
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * cache.c:
 */

NS_EXTERN Ns_Cache *
Ns_CacheCreate(const char *name, int keys, time_t ttl, Ns_FreeProc *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Cache *
Ns_CacheCreateSz(const char *name, int keys, size_t maxSize, Ns_FreeProc *freeProc)
    NS_GNUC_RETURNS_NONNULL NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Cache *
Ns_CacheCreateEx(const char *name, int keys, time_t ttl, size_t maxSize,
                 Ns_FreeProc *freeProc)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheDestroy(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Entry *
Ns_CacheFindEntry(Ns_Cache *cache, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Entry *
Ns_CacheFindEntryT(Ns_Cache *cache, const char *key, const Ns_CacheTransactionStack *transactionStackPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Entry *
Ns_CacheCreateEntry(Ns_Cache *cache, const char *key, int *newPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_Entry *
Ns_CacheWaitCreateEntry(Ns_Cache *cache, const char *key, int *newPtr,
                        const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_Entry *
Ns_CacheWaitCreateEntryT(Ns_Cache *cache, const char *key, int *newPtr,
                        const Ns_Time *timeoutPtr, const Ns_CacheTransactionStack *transactionStackPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN const char *
Ns_CacheName(const Ns_Cache *cache) NS_GNUC_RETURNS_NONNULL
    NS_GNUC_NONNULL(1) NS_GNUC_CONST;

NS_EXTERN const char *
Ns_CacheKey(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN void *
Ns_CacheGetValue(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN size_t
Ns_CacheGetReuse(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN size_t
Ns_CacheGetSize(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN const Ns_Time *
Ns_CacheGetExpirey(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1) NS_GNUC_CONST;

NS_EXTERN uintptr_t
Ns_CacheGetTransactionEpoch(const Ns_Entry *entry)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN unsigned long
Ns_CacheCommitEntries(Ns_Cache *cache, uintptr_t epoch)
    NS_GNUC_NONNULL(1);

NS_EXTERN unsigned long
Ns_CacheRollbackEntries(Ns_Cache *cache, uintptr_t epoch)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheSetValue(Ns_Entry *entry, void *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void *
Ns_CacheGetValueT(const Ns_Entry *entry, const Ns_CacheTransactionStack *transactionStackPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN void
Ns_CacheSetValueSz(Ns_Entry *entry, void *value, size_t size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_CacheSetValueExpires(Ns_Entry *entry, void *value, size_t size,
                        const Ns_Time *timeoutPtr, int cost, size_t maxSize,
                        uintptr_t transactionEpoch)
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
Ns_CacheFirstEntryT(Ns_Cache *cache, Ns_CacheSearch *search, const Ns_CacheTransactionStack *transactionStackPtr)
        NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Entry *
Ns_CacheNextEntry(Ns_CacheSearch *search)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Entry *
Ns_CacheNextEntryT(Ns_CacheSearch *search, const Ns_CacheTransactionStack *transactionStackPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_CacheFlush(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheLock(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_CacheTryLock(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheUnlock(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_CacheWait(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_CacheTimedWait(Ns_Cache *cache, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheSignal(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CacheBroadcast(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN char*
Ns_CacheStats(Ns_Cache *cache, Tcl_DString *dest)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_CacheResetStats(Ns_Cache *cache)
    NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_CacheGetMaxSize(const Ns_Cache *cache)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN void
Ns_CacheSetMaxSize(Ns_Cache *cache, size_t maxSize)
    NS_GNUC_NONNULL(1);

NS_EXTERN TCL_SIZE_T
Ns_CacheGetNrUncommittedEntries(const Ns_Cache *cache)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

/*
 * callbacks.c:
 */

NS_EXTERN void *Ns_RegisterAtPreStartup(Ns_Callback *proc, void *arg)   NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtStartup(Ns_Callback *proc, void *arg)      NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtSignal(Ns_Callback *proc, void *arg)       NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtReady(Ns_Callback *proc, void *arg)        NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtShutdown(Ns_ShutdownProc *proc, void *arg) NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_RegisterAtExit(Ns_Callback *proc, void *arg)         NS_GNUC_NONNULL(1);

/*
 * cls.c:
 */

NS_EXTERN void Ns_ClsAlloc(Ns_Cls *clsPtr, Ns_Callback *cleanupProc);
NS_EXTERN void *Ns_ClsGet(const Ns_Cls *clsPtr, Ns_Conn *conn) NS_GNUC_PURE;
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


NS_EXTERN Ns_ReturnCode
Ns_CompressInit(Ns_CompressStream *cStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CompressFree(Ns_CompressStream *cStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_CompressBufsGzip(Ns_CompressStream *cStream, struct iovec *bufs, int nbufs,
                    Tcl_DString *dsPtr, int level, bool flush)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_CompressGzip(const char *buf, int len, Tcl_DString *dsPtr, int level)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_InflateInit(Ns_CompressStream *cStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_InflateBufferInit(Ns_CompressStream *cStream, const void *inBuf, size_t inSize)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_InflateBuffer(Ns_CompressStream *cStream, void *outBuf, size_t outSize, size_t *nrBytes)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_InflateEnd(Ns_CompressStream *cStream)
    NS_GNUC_NONNULL(1);

/*
 * config.c:
 */

NS_EXTERN const char *
Ns_ConfigString(const char *section, const char *key, const char *defaultValue)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_ConfigBool(const char *section, const char *key, bool defaultValue)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_ConfigFlag(const char *section, const char *key, unsigned int flag, int defaultValue,
              unsigned int *flagsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

NS_EXTERN int
Ns_ConfigInt(const char *section, const char *key, int defaultValue)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigIntRange(const char *section, const char *key, int defaultValue,
                  int minValue, int maxValue)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_ConfigWideInt(const char *section, const char *key, Tcl_WideInt defaultValue)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_ConfigWideIntRange(const char *section, const char *key,
                      Tcl_WideInt defaultValue,
                      Tcl_WideInt minValue, Tcl_WideInt maxValue)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_ConfigMemUnitRange(const char *section, const char *key,
                      const char *defaultString, Tcl_WideInt defaultValue,
                      Tcl_WideInt minValue, Tcl_WideInt maxValue)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_ConfigGetValue(const char *section, const char *key)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_ConfigGetValueExact(const char *section, const char *key)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_ConfigGetInt(const char *section, const char *key, int *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN bool
Ns_ConfigGetInt64(const char *section, const char *key, int64_t *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN bool
Ns_ConfigGetBool(const char *section, const char *key, bool *valuePtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN const char *
Ns_ConfigFilename(const char *section, const char* key, TCL_SIZE_T keyLength,
                  const char *directory, const char* defaultValue,
                  bool normalizePath, bool update)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

NS_EXTERN const char *
Ns_ConfigGetPath(const char *server, const char *module, ...)
     NS_GNUC_SENTINEL;

NS_EXTERN const char *
Ns_ConfigSectionPath(Ns_Set **setPtr, const char *server, const char *module, ...)
     NS_GNUC_SENTINEL NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Set **
Ns_ConfigGetSections(void);

NS_EXTERN Ns_Set *
Ns_ConfigGetSection(const char *section)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Set *
Ns_ConfigGetSection2(const char *section, bool markAsRead)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Set *
Ns_ConfigCreateSection(const char *section)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_GetVersion(int *majorV, int *minorV, int *patchLevelV, int *type);

NS_EXTERN const Ns_Set *
Ns_ConfigSet(const char *section, const char *key, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConfigTimeUnitRange(const char *section, const char *key,
                       const char *defaultString,
                       long minSec, long minUsec,
                       long maxSec, long maxUsec,
                       Ns_Time *timePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)  NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(8);

/*
 * conn.c:
 */

NS_EXTERN Ns_Time *      Ns_ConnAcceptTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;
NS_EXTERN Ns_Set *       Ns_ConnAuth(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const char *   Ns_ConnAuthPasswd(const Ns_Conn *conn) NS_GNUC_NONNULL(1);
NS_EXTERN const char *   Ns_ConnAuthUser(const Ns_Conn *conn) NS_GNUC_NONNULL(1);
NS_EXTERN const char *   Ns_ConnContent(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN int            Ns_ConnContentFd(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const char *   Ns_ConnContentFile(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN size_t         Ns_ConnContentLength(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN size_t         Ns_ConnContentSent(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN size_t         Ns_ConnContentSize(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const char *   Ns_ConnCurrentAddr(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN unsigned short Ns_ConnCurrentPort(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Ns_Time *      Ns_ConnDequeueTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;
NS_EXTERN const char *   Ns_ConnDriverName(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE NS_GNUC_RETURNS_NONNULL;
NS_EXTERN Ns_Time *      Ns_ConnFilterTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE NS_GNUC_RETURNS_NONNULL;
NS_EXTERN int            Ns_ConnGetCompression(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Tcl_Encoding   Ns_ConnGetEncoding(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Tcl_Encoding   Ns_ConnGetUrlEncoding(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Ns_Set *       Ns_ConnHeaders(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const char *   Ns_ConnHost(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE NS_GNUC_RETURNS_NONNULL;
NS_EXTERN uintptr_t      Ns_ConnId(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_CONST;
#ifdef NS_WITH_DEPRECATED
NS_EXTERN const char *   Ns_ConnLocation(Ns_Conn *conn) NS_GNUC_DEPRECATED_FOR(Ns_ConnLocationAppend);
#endif
NS_EXTERN char *         Ns_ConnLocationAppend(Ns_Conn *conn, Tcl_DString *dest) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN bool           Ns_ConnModifiedSince(const Ns_Conn *conn, time_t since) NS_GNUC_NONNULL(1);
NS_EXTERN Ns_Set *       Ns_ConnOutputHeaders(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
#ifdef NS_WITH_DEPRECATED
NS_EXTERN const char *   Ns_ConnPeer(const Ns_Conn *conn) NS_GNUC_PURE NS_GNUC_DEPRECATED_FOR(Ns_ConnPeerAddr);
#endif
NS_EXTERN const char *   Ns_ConnPeerAddr(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN unsigned short Ns_ConnPeerPort(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const char *   Ns_ConnForwardedPeerAddr(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const char *   Ns_ConnConfiguredPeerAddr(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN unsigned short Ns_ConnPort(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Ns_Time *      Ns_ConnQueueTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;
NS_EXTERN ssize_t        Ns_ConnResponseLength(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN int            Ns_ConnResponseStatus(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const char *   Ns_ConnServer(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN void           Ns_ConnSetCompression(Ns_Conn *conn, int level) NS_GNUC_NONNULL(1);
NS_EXTERN void           Ns_ConnSetContentSent(Ns_Conn *conn, size_t length) NS_GNUC_NONNULL(1);
NS_EXTERN void           Ns_ConnSetEncoding(Ns_Conn *conn, Tcl_Encoding encoding) NS_GNUC_NONNULL(1);
NS_EXTERN const char *   Ns_ConnSetPeer(Ns_Conn *conn, const struct sockaddr *saPtr,
                                        const struct sockaddr *clientsaPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
NS_EXTERN void           Ns_ConnSetResponseStatus(Ns_Conn *conn, int newStatus) NS_GNUC_NONNULL(1);
NS_EXTERN void           Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding) NS_GNUC_NONNULL(1);
NS_EXTERN NS_SOCKET      Ns_ConnSock(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Tcl_DString*   Ns_ConnSockContent(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Ns_Sock *      Ns_ConnSockPtr(const Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Ns_Time *      Ns_ConnStartTime(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;
NS_EXTERN void           Ns_ConnTimeSpans(
    const Ns_Conn *conn, Ns_Time *acceptTimeSpanPtr, Ns_Time *queueTimeSpanPtr,
    Ns_Time *filterTimeSpanPtr, Ns_Time *runTimeSpanPtr
)    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);
NS_EXTERN Ns_Time *      Ns_ConnTimeout(Ns_Conn *conn) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN bool           Ns_ConnUnmodifiedSince(const Ns_Conn *conn, time_t since) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode  Ns_SetConnLocationProc(Ns_ConnLocationProc *proc, Ns_TclCallback *cbPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
#ifdef NS_WITH_DEPRECATED
NS_EXTERN void           Ns_SetLocationProc(const char *server, Ns_LocationProc *proc) NS_GNUC_DEPRECATED_FOR(Ns_SetConnLocationProc);
#endif
NS_EXTERN const char *   Ns_ConnTarget(Ns_Conn *conn, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN const Ns_UrlSpaceMatchInfo *Ns_ConnGetUrlSpaceMatchInfo(const Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN Ns_Server *    Ns_ConnServPtr(const Ns_Conn *conn)
        NS_GNUC_NONNULL(1);

/*
 * connio.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_ConnWriteChars(Ns_Conn *conn, const char *buf, size_t toWrite, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnWriteVChars(Ns_Conn *conn, struct iovec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnWriteData(Ns_Conn *conn, const void *buf, size_t toWrite, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnWriteVData(Ns_Conn *conn, struct iovec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnSendFd(Ns_Conn *conn, int fd, ssize_t nsend)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnSendFp(Ns_Conn *conn, FILE *fp, ssize_t nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnSendChannel(Ns_Conn *conn, Tcl_Channel chan, ssize_t nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnSendFileVec(Ns_Conn *conn, Ns_FileVec *bufs, int nbufs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnSendDString(Ns_Conn *conn, const Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnPuts(Ns_Conn *conn, const char *s)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN ssize_t
Ns_ConnSend(Ns_Conn *conn, struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnClose(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnFlushContent(const Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_ConnGets(char *buf, size_t bufsize, const Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN size_t
Ns_ConnRead(const Ns_Conn *conn, void *vbuf, size_t toRead)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnReadLine(const Ns_Conn *conn, Tcl_DString *dsPtr, size_t *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnReadHeaders(const Ns_Conn *conn, Ns_Set *set, size_t *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnCopyToDString(const Ns_Conn *conn, size_t toCopy, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_ConnCopyToFd(const Ns_Conn *conn, size_t ncopy, int fd)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnCopyToFile(const Ns_Conn *conn, size_t ncopy, FILE *fp)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_ConnCopyToChannel(const Ns_Conn *conn, size_t ncopy, Tcl_Channel chan)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN int
Ns_ConnWrite(Ns_Conn *conn, const void *buf, size_t toWrite)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN Ns_ReturnCode
Ns_WriteConn(Ns_Conn *conn, const char *buf, size_t toWrite)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(Ns_ConnWriteVData);

NS_EXTERN Ns_ReturnCode
Ns_WriteCharConn(Ns_Conn *conn, const char *buf, size_t toWrite)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED_FOR(Ns_ConnWriteVChars);
#endif

NS_EXTERN bool
Ns_CompleteHeaders(Ns_Conn *conn, size_t dataLength, unsigned int flags, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN bool
Ns_FinalizeResponseHeaders(Ns_Conn *conn, size_t bodyLength, unsigned int flags,
                           void *out_obj, size_t *out_len)
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

NS_EXTERN const char *
Ns_ConnGetCookie(Tcl_DString *dest, const Ns_Conn *conn, const char *name)
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

NS_EXTERN bool
Ns_GetHostByAddr(Tcl_DString *dsPtr, const char *addr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_GetAddrByHost(Tcl_DString *dsPtr, const char *host)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_GetAllAddrByHost(Tcl_DString *dsPtr, const char *host)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * driver.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_DriverInit(const char *server, const char *module, const Ns_DriverInitData *init)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
NsAsyncWrite(int fd, const char *buffer, size_t nbyte)
    NS_GNUC_NONNULL(2);

NS_EXTERN void
NsAsyncWriterQueueDisable(bool shutdown);

NS_EXTERN void
NsAsyncWriterQueueEnable(void);


/*
 * dstring.c:
 */

NS_EXTERN char *
Ns_DStringVarAppend(Tcl_DString *dsPtr, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_DStringExport(Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_DStringAppendArg(Tcl_DString *dsPtr, const char *bytes)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DStringPrintf(Tcl_DString *dsPtr, const char *fmt, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_PRINTF(2,3);

NS_EXTERN char *
Ns_DStringVPrintf(Tcl_DString *dsPtr, const char *fmt, va_list apSrc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PRINTF(2, 0);

NS_EXTERN char **
Ns_DStringAppendArgv(Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_DStringAppendPrintable(Tcl_DString *dsPtr, bool indentMode, bool tabExpandMode, const char *buffer, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN char *
Ns_DStringAppendEscaped(Tcl_DString *dsPtr, const char *inputString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DStringAppendTime(Tcl_DString *dsPtr, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_DStringAppendSockState(Tcl_DString *dsPtr, Ns_SockState state)
    NS_GNUC_NONNULL(1);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN Tcl_DString *
Ns_DStringPop(void)
     NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_DStringPush(Tcl_DString *dsPtr)
     NS_GNUC_DEPRECATED;
#endif


#ifdef NS_WITH_DEPRECATED_5_0
NS_EXTERN char *Ns_DStringValue(const Tcl_DString *dsPtr) NS_GNUC_PURE
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringValue);
NS_EXTERN TCL_SIZE_T Ns_DStringLength(const Tcl_DString *dsPtr) NS_GNUC_PURE
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringLength);
NS_EXTERN char *Ns_DStringAppend(Tcl_DString *dsPtr, const char *bytes)
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringAppend);
NS_EXTERN char *Ns_DStringAppendElement(Tcl_DString *dsPtr, const char *bytes)
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringAppendElement);
NS_EXTERN char *Ns_DStringNAppend(Tcl_DString *dsPtr, const char *bytes, TCL_SIZE_T length)
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringAppend);
NS_EXTERN void Ns_DStringInit(Tcl_DString *dsPtr)
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringInit);
NS_EXTERN void Ns_DStringFree(Tcl_DString *dsPtr)
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringFree);
NS_EXTERN void Ns_DStringSetLength(Tcl_DString *dsPtr, TCL_SIZE_T length)
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringSetLength);
NS_EXTERN void Ns_DStringTrunc(Tcl_DString *dsPtr, TCL_SIZE_T length)
    NS_GNUC_DEPRECATED_FOR(Tcl_DStringSetLength);
#endif

/*
 * event.c
 */

NS_EXTERN Ns_EventQueue *
Ns_CreateEventQueue(int maxevents);

NS_EXTERN bool
Ns_EventEnqueue(Ns_EventQueue *queue, NS_SOCKET sock, Ns_EventProc *proc, void *arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_EventCallback(Ns_Event *event, Ns_SockState when, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_RunEventQueue(Ns_EventQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TriggerEventQueue(const Ns_EventQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ExitEventQueue(Ns_EventQueue *queue)
    NS_GNUC_NONNULL(1);


/*
 * exec.c:
 */

NS_EXTERN pid_t
Ns_ExecProcess(const char *exec, const char *dir, int fdin, int fdout,
               const char *args, const Ns_Set *env)
    NS_GNUC_NONNULL(1);

NS_EXTERN pid_t
Ns_ExecProc(const char *exec, const char *const *argv)
    NS_GNUC_NONNULL(1);

NS_EXTERN pid_t
Ns_ExecArgblk(const char *exec, const char *dir, int fdin, int fdout,
              const char *args, const Ns_Set *env)
    NS_GNUC_NONNULL(1);

NS_EXTERN pid_t
Ns_ExecArgv(const char *exec, const char *dir, int fdin, int fdout, const char *const *argv, const Ns_Set *env)
    NS_GNUC_NONNULL(1);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN Ns_ReturnCode
Ns_WaitProcess(pid_t pid)
    NS_GNUC_DEPRECATED_FOR(Ns_WaitForProcessStatus);
#endif

NS_EXTERN Ns_ReturnCode
Ns_WaitForProcess(pid_t pid, int *exitcodePtr);

NS_EXTERN Ns_ReturnCode
Ns_WaitForProcessStatus(pid_t pid, int *exitcodePtr, int *waitstatusPtr);

/*
 * fastpath.c:
 */
NS_EXTERN bool
Ns_Stat(const char *path, struct stat *stPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnFile(Ns_Conn *conn, int statusCode, const char *mimeType, const char *fileName)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN const char *
Ns_PageRoot(const char *server) NS_GNUC_DEPRECATED_FOR(Ns_PagePath);
#endif

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
                  Ns_FilterProc *proc, Ns_FilterType when, void *arg, bool first)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4)
    NS_GNUC_RETURNS_NONNULL;

void *
Ns_RegisterFilter2(const char *server, const char *method, const char *url,
                   Ns_FilterProc *proc, Ns_FilterType when, void *arg, bool first,
                   void *ctxFilterSpec)
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
Ns_Base64Encode(const unsigned char *input, size_t inputSize, char *buf, size_t maxLineLength, int encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN size_t
Ns_HtuuEncode(const unsigned char *input, size_t inputSize, char *buf)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN size_t
Ns_HtuuDecode(const char *input, unsigned char *buf, size_t bufSize)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN size_t
Ns_HtuuEncode2(const unsigned char *input, size_t inputSize, char *buf, int encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_HtuuDecode2(Tcl_Interp *interp, const char *input, unsigned char *buf, size_t bufSize, int encoding, bool strict, size_t *decodedLength)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
/*
 * index.c:
 */

NS_EXTERN void
Ns_IndexInit(Ns_Index *indexPtr, size_t inc, int (*CmpEls) (const void *left, const void *right),
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
Ns_IndexEl(const Ns_Index *indexPtr, size_t i) NS_GNUC_PURE NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexStringInit(Ns_Index *indexPtr, size_t inc) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Index *
Ns_IndexStringDup(const Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexStringAppend(Ns_Index *addtoPtr, const Ns_Index *addfromPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_IndexStringDestroy(Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexStringTrunc(Ns_Index *indexPtr) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_IndexIntInit(Ns_Index *indexPtr, size_t inc) NS_GNUC_NONNULL(1);

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
Ns_ListLast(Ns_List *lPtr)
    NS_GNUC_PURE;

NS_EXTERN void
Ns_ListFree(Ns_List *lPtr, Ns_ElemVoidProc *freeProc);

NS_EXTERN void
Ns_IntPrint(int d);

NS_EXTERN void
Ns_StringPrint(const char *s)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ListPrint(const Ns_List *lPtr, Ns_ElemVoidProc *printProc);

NS_EXTERN Ns_List *
Ns_ListCopy(const Ns_List *lPtr);

NS_EXTERN int
Ns_ListLength(const Ns_List *lPtr)
    NS_GNUC_PURE;

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

NS_EXTERN Ns_Task *
Ns_TaskTimedCreate(NS_SOCKET sock, Ns_TaskProc *proc, void *arg, Ns_Time *)
    NS_GNUC_NONNULL(2)
    NS_GNUC_RETURNS_NONNULL
    NS_GNUC_WARN_UNUSED_RESULT;

NS_EXTERN Ns_ReturnCode
Ns_TaskEnqueue(Ns_Task *task, Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_TaskRun(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskCallback(Ns_Task *task, Ns_SockState when, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskDone(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_TaskCompleted(const Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_TaskCancel(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_TaskWait(Ns_Task *task, Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskWaitCompleted(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TaskSetCompleted(const Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_TaskFree(Ns_Task *task)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_TaskQueueLength(Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_TaskQueueName(Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN intptr_t
Ns_TaskQueueRequests(Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1);

/*
 * tclobj.c:
 */

NS_EXTERN void
Ns_TclResetObjType(Tcl_Obj *objPtr, CONST86 Tcl_ObjType *newTypePtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TclSetTwoPtrValue(Tcl_Obj *objPtr, CONST86 Tcl_ObjType *newTypePtr,
                     void *ptr1, void *ptr2)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_TclSetOtherValuePtr(Tcl_Obj *objPtr, CONST86 Tcl_ObjType *newTypePtr, void *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_TclSetStringRep(Tcl_Obj *objPtr, const char *bytes, TCL_SIZE_T length)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_TclGetAddrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr,
                     const char *type, void **addrPtrPtr)
     NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_TclSetAddrObj(Tcl_Obj *objPtr, const char *type, void *addr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_TclGetOpaqueFromObj(Tcl_Obj *objPtr, const char *type, void **addrPtrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_TclSetOpaqueObj(Tcl_Obj *objPtr, const char *type, void *addr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_SetFromAnyProc Ns_TclSetFromAnyError;

/*
 * tclobjv.c
 */

NS_EXTERN Ns_ReturnCode
Ns_ParseObjv(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec,
             Tcl_Interp *interp, TCL_SIZE_T parseOffset, TCL_SIZE_T objc, Tcl_Obj *const* objv)
    NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ObjvProc Ns_ObjvArgs;
NS_EXTERN Ns_ObjvProc Ns_ObjvBool;
NS_EXTERN Ns_ObjvProc Ns_ObjvBreak NS_GNUC_CONST;
NS_EXTERN Ns_ObjvProc Ns_ObjvByteArray;
NS_EXTERN Ns_ObjvProc Ns_ObjvDouble;
NS_EXTERN Ns_ObjvProc Ns_ObjvEval;
#ifdef NS_WITH_DEPRECATED_5_0
NS_EXTERN Ns_ObjvProc Ns_ObjvFlags NS_GNUC_DEPRECATED_FOR(Ns_ObjvIndex);
#endif
NS_EXTERN Ns_ObjvProc Ns_ObjvIndex;
NS_EXTERN Ns_ObjvProc Ns_ObjvInt;
NS_EXTERN Ns_ObjvProc Ns_ObjvLong;
NS_EXTERN Ns_ObjvProc Ns_ObjvMemUnit;
NS_EXTERN Ns_ObjvProc Ns_ObjvObj;
NS_EXTERN Ns_ObjvProc Ns_ObjvServer;
NS_EXTERN Ns_ObjvProc Ns_ObjvSet;
NS_EXTERN Ns_ObjvProc Ns_ObjvString;
NS_EXTERN Ns_ObjvProc Ns_ObjvTime;
NS_EXTERN Ns_ObjvProc Ns_ObjvUShort;
NS_EXTERN Ns_ObjvProc Ns_ObjvUrlspaceSpec;
NS_EXTERN Ns_ObjvProc Ns_ObjvWideInt;

NS_EXTERN int Ns_TclGetMemUnitFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Tcl_WideInt *memUnitPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int Ns_CheckWideRange(Tcl_Interp *interp, const char *name, const Ns_ObjvValueRange *r, Tcl_WideInt value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int Ns_CheckTimeRange(Tcl_Interp *interp, const char *name, const Ns_ObjvTimeRange *r, Ns_Time *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_SubcmdObjv(const Ns_SubCmdSpec *subcmdSpec, ClientData clientData,
              Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

NS_EXTERN char *
Ns_ObjvTablePrint(Tcl_DString *dsPtr, Ns_ObjvTable *values)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#define Ns_NrElements(arr)  ((int) (sizeof(arr) / sizeof((arr)[0])))

/*
 * tclthread.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_TclThread(Tcl_Interp *interp, const char *script, Ns_Thread *thrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
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

NS_EXTERN int
Ns_GetTimeFromString(Tcl_Interp *interp, const char *str, Ns_Time *tPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

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
                     const char *keyedList, TCL_SIZE_T *keysArgcPtr, char ***keysArgvPtr);

NS_EXTERN char *
Tcl_SetKeyedListField(Tcl_Interp  *interp, const char *fieldName,
                      const char *fieldValue, const char *keyedList);

/*
 * listen.c:
 */

NS_EXTERN NS_SOCKET
Ns_SockListenCallback(const char *addr, unsigned short port, Ns_SockProc *proc, bool bind, void *arg)
    NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);


NS_EXTERN bool
Ns_SockPortBound(unsigned short port);

/*
 * log.c:
 */

NS_EXTERN const char *
Ns_InfoErrorLog(void)
    NS_GNUC_PURE;

NS_EXTERN Ns_ReturnCode
Ns_LogRoll(void);

NS_EXTERN void
Ns_Log(Ns_LogSeverity severity, const char *fmt, ...)
    NS_GNUC_NONNULL(2)
    NS_GNUC_PRINTF(2, 3);

NS_EXTERN void
Ns_VALog(Ns_LogSeverity severity, const char *fmt, va_list apSrc)
    NS_GNUC_NONNULL(2) NS_GNUC_PRINTF(2, 0);

NS_EXTERN void
Ns_Fatal(const char *fmt, ...)
    NS_GNUC_NONNULL(1)
    NS_GNUC_PRINTF(1, 2)
    NS_GNUC_NORETURN;

NS_EXTERN char *
Ns_LogTime(char *timeBuf)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_LogTime2(char *timeBuf, bool gmt)
    NS_GNUC_NONNULL(1);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN void
Ns_SetLogFlushProc(Ns_LogFlushProc *procPtr)
    NS_GNUC_DEPRECATED_FOR(Ns_AddLogFilter) NS_GNUC_NORETURN;

NS_EXTERN void
Ns_SetNsLogProc(Ns_LogProc *procPtr)
    NS_GNUC_DEPRECATED_FOR(Ns_AddLogFilter) NS_GNUC_NORETURN;
#endif

NS_EXTERN void
Ns_AddLogFilter(Ns_LogFilter *procPtr, void *arg, Ns_FreeProc *freeProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_RemoveLogFilter(Ns_LogFilter *procPtr, void *const arg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_LogSeverity
Ns_CreateLogSeverity(const char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_LogSeverityName(Ns_LogSeverity severity)
    NS_GNUC_RETURNS_NONNULL NS_GNUC_PURE;

NS_EXTERN bool
Ns_LogSeverityEnabled(Ns_LogSeverity severity)
    NS_GNUC_PURE;

NS_EXTERN bool
Ns_LogSeveritySetEnabled(Ns_LogSeverity severity, bool enabled)
    NS_GNUC_PURE;


/*
 * rollfile.c
 */

NS_EXTERN Ns_ReturnCode
Ns_RollFile(const char *fileName, TCL_SIZE_T max)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_PurgeFiles(const char *fileName, TCL_SIZE_T max)
    NS_GNUC_NONNULL(1);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN Ns_ReturnCode
Ns_RollFileByDate(const char *fileName, TCL_SIZE_T max)
    NS_GNUC_NONNULL(1)
    NS_GNUC_DEPRECATED_FOR(Ns_PurgeFiles);
#endif

NS_EXTERN Ns_ReturnCode
Ns_RollFileFmt(Tcl_Obj *fileObj, const char *rollfmt, TCL_SIZE_T maxbackup)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_RollFileCondFmt(Ns_LogCallbackProc openProc, Ns_LogCallbackProc closeProc, const void *arg,
                   const char *filename, const char *rollfmt, TCL_SIZE_T maxbackup)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

/*
 * nsmain.c:
 */

NS_EXTERN void
Nsd_LibInit(void);

NS_EXTERN int
Ns_Main(int argc, char *const*argv, Ns_ServerInitProc *initProc);

NS_EXTERN Ns_ReturnCode
Ns_WaitForStartup(void);

NS_EXTERN void
Ns_StopServer(char *server);

/*
 * info.c:
 */
NS_EXTERN const char *
Ns_InfoAddress(void) NS_GNUC_CONST;

NS_EXTERN time_t
Ns_InfoBootTime(void) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_InfoBuildDate(void) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_InfoConfigFile(void) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_InfoHomePath(void) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_InfoLogPath(void) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_InfoHostname(void) NS_GNUC_PURE;

NS_EXTERN bool
Ns_InfoIPv6(void) NS_GNUC_CONST;

NS_EXTERN const char *
Ns_InfoNameOfExecutable(void) NS_GNUC_PURE;

NS_EXTERN pid_t
Ns_InfoPid(void) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_InfoPlatform(void) NS_GNUC_CONST;

NS_EXTERN const char *
Ns_InfoServerName(void) NS_GNUC_CONST;

NS_EXTERN bool
Ns_InfoServersStarted(void);

NS_EXTERN const char *
Ns_InfoServerVersion(void) NS_GNUC_CONST;

NS_EXTERN bool
Ns_InfoShutdownPending(void);

NS_EXTERN bool
Ns_InfoSSL(void) NS_GNUC_CONST;

NS_EXTERN bool
Ns_InfoStarted(void);

NS_EXTERN const char *
Ns_InfoTag(void) NS_GNUC_CONST;

NS_EXTERN long
Ns_InfoUptime(void);

/*
 * mimetypes.c:
 */

NS_EXTERN const char *
Ns_GetMimeType(const char *file)
    NS_GNUC_NONNULL(1)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN bool
Ns_IsBinaryMimeType(const char *contentType)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

/*
 * encoding.c:
 */

NS_EXTERN Tcl_Encoding
Ns_GetCharsetEncoding(const char *charset)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetCharsetEncodingEx(const char *charset, TCL_SIZE_T len)
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

#ifdef NS_WITH_DEPRECATED
NS_EXTERN Tcl_Encoding
Ns_GetEncoding(const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(Ns_GetCharsetEncodingEx);
#endif

/*
 * modload.c:
 */

NS_EXTERN void
Ns_RegisterModule(const char *name, Ns_ModuleInitProc *proc)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ModuleLoad(Tcl_Interp *interp, const char *server, const char *module, const char *file,
              const char *init)
    NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

/*
 * nsthread.c:
 */
#ifdef NS_WITH_DEPRECATED
NS_EXTERN void
Ns_SetThreadServer(const char *server)
    NS_GNUC_PRINTF(1, 0)
    NS_GNUC_DEPRECATED_FOR(Ns_ThreadSetName);

NS_EXTERN const char *
Ns_GetThreadServer(void)
    NS_GNUC_DEPRECATED_FOR(Ns_ThreadGetName);
#endif

/*
 * op.c:
 */

NS_EXTERN void
Ns_RegisterRequest(const char *server, const char *method, const char *url,
                   Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg,
                   int unsigned flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3)
    NS_GNUC_NONNULL(4);


NS_EXTERN int Ns_RegisterRequest2(Tcl_Interp *interp,
                                  const char *server, const char *method, const char *url,
                                  Ns_OpProc *proc, Ns_Callback *deleteCallback, void *arg,
                                  unsigned int flags, void *contextSpec)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4)
    NS_GNUC_NONNULL(5);

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
                     bool inherit)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterProxyRequest(const char *server, const char *method,
                          const char *protocol)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_UnRegisterRequestEx(const char *server, const char *method, const char *url,
                       unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_ConnRunRequest(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnRedirect(Ns_Conn *conn, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * pathname.c:
 */

NS_EXTERN const char *
Ns_BinPath(Tcl_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN const char *
Ns_HashPath(Tcl_DString *dsPtr, const char *path, int levels)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_HomePath(Tcl_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN bool
Ns_HomePathExists(const char *path, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_LibPath(Tcl_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN const char *
Ns_MakePath(Tcl_DString *dsPtr, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN const char *
Ns_ModulePath(Tcl_DString *dsPtr, const char *server, const char *module, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN const char *
Ns_NormalizePath(Tcl_DString *dsPtr, const char *path)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN const char *
Ns_NormalizeUrl(Tcl_DString *dsPtr, const char *path)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_RETURNS_NONNULL;

NS_EXTERN const char *
Ns_PagePath(Tcl_DString *dsPtr, const char *server, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_PathIsAbsolute(const char *path) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_RequireDirectory(const char *path)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_ServerPath(Tcl_DString *dsPtr, const char *server, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_SetServerRootProc(Ns_ServerRootProc *proc, void *arg);

NS_EXTERN const char *
Ns_LogPath(Tcl_DString *dsPtr, const char *server, const char *filename)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * proc.c:
 */

NS_EXTERN void
Ns_RegisterProcInfo(ns_funcptr_t procAddr, const char *desc, Ns_ArgProc *argProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_GetProcInfo(Tcl_DString *dsPtr, ns_funcptr_t procAddr, const void *arg)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_StringArgProc(Tcl_DString *dsPtr, const void *arg)
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
Ns_QuoteHtml(Tcl_DString *dsPtr, const char *htmlString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * request.c:
 */

NS_EXTERN void
Ns_FreeRequest(Ns_Request *request);

NS_EXTERN void
Ns_ResetRequest(Ns_Request *request)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ParseRequest(Ns_Request *request, const char *line, size_t len)
    NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_SkipUrl(const Ns_Request *request, int n)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_SetRequestUrl(Ns_Request *request, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ParseHeader(Ns_Set *set, const char *line, const char *prefix, Ns_HeaderCaseDisposition disp,
               size_t *fieldNumberPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_HttpMessageParse(char *messageString, size_t messageLength, size_t *firstLineLengthPtr,
                    Ns_Set *hdrPtr, char **payloadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_HttpResponseMessageParse(char *messageString, size_t messageLength,
                            Ns_Set *hdrPtr, int *majorPtr, int *minorPtr, int *statusPtr, char **payloadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(6);

/*
 * return.c:
 */

NS_EXTERN void
Ns_ConnSetHeaders(const Ns_Conn *conn, const char *field, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnSetHeadersSz(const Ns_Conn *conn,
                    const char *field, TCL_SIZE_T fieldLength,
                    const char *value, TCL_SIZE_T valueLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_ConnUpdateHeaders(const Ns_Conn *conn, const char *field, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnUpdateHeadersSz(const Ns_Conn *conn,
                       const char *field, TCL_SIZE_T fieldLength,
                       const char *value, TCL_SIZE_T valueLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_ConnCondSetHeaders(const Ns_Conn *conn, const char *field, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnCondSetHeadersSz(const Ns_Conn *conn,
                        const char *field, TCL_SIZE_T fieldLength,
                        const char *value, TCL_SIZE_T valueLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

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
Ns_ConnSetLengthHeader(Ns_Conn *conn, size_t length, bool doStream)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetLastModifiedHeader(const Ns_Conn *conn, const time_t *mtime)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetExpiresHeader(const Ns_Conn *conn, const char *expires)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnConstructHeaders(const Ns_Conn *conn, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnNotice(Ns_Conn *conn, int status, const char *title,
                    const char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnAdminNotice(Ns_Conn *conn, int status, const char *title,
                         const char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnHtml(Ns_Conn *conn, int status, const char *html, ssize_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnCharData(Ns_Conn *conn, int status, const char *data,
                      ssize_t len, const char *mimeType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnData(Ns_Conn *conn, int status, const char *data,
                  ssize_t len, const char *mimeType)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnOpenChannel(Ns_Conn *conn, int status, const char *mimeType,
                         Tcl_Channel chan, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnOpenFile(Ns_Conn *conn, int status, const char *mimeType,
                      FILE *fp, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnOpenFd(Ns_Conn *conn, int status, const char *mimeType, int fd, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN void
Ns_ConnSetRequiredHeaders(Ns_Conn *conn, const char *mimeType, size_t length)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_ConnQueueHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN size_t
Ns_ConnFlushHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN Ns_ReturnCode
Ns_ConnResetReturn(Ns_Conn *conn) NS_GNUC_CONST
    NS_GNUC_DEPRECATED;
#endif


/*
 * returnresp.c:
 */

NS_EXTERN void
Ns_RegisterReturn(int status, const char *url);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnStatus(Ns_Conn *conn, int httpStatus)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnOk(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnNoResponse(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnRedirect(Ns_Conn *conn, const char *url)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnBadRequest(Ns_Conn *conn, const char *reason)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnHeaderLineTooLong(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnUnauthorized(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnForbidden(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnMoved(Ns_Conn *conn, const char *url)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnNotFound(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnInvalidMethod(Ns_Conn *conn)
        NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnNotModified(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnEntityTooLarge(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnNotImplemented(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnTryReturnInternalError(Ns_Conn *conn, Ns_ReturnCode status, const char *causeString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnInternalError(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnRequestURITooLong(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_ConnReturnUnavailable(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

/*
 * server.c
 */
NS_EXTERN const char *  Ns_ServerLogDir(const char *server) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN bool          Ns_ServerRootProcEnabled(const char *server) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN int           Ns_ServerLogGetFd(const char *server, const void *handle, const char *filename)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
NS_EXTERN Ns_ReturnCode Ns_ServerLogCloseAll(const char *server, const void *handle) NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_ServerLogRollAll(const char *server, const void *handle, const char *rollfmt, TCL_SIZE_T maxbackup)
    NS_GNUC_NONNULL(1);
NS_EXTERN Ns_Server *   Ns_GetServer(const char *server)
    NS_GNUC_NONNULL(1);
NS_EXTERN const char *  Ns_ServerName(const Ns_Server *servPtr)
    NS_GNUC_NONNULL(1);
/*
 * tclvar.c
 */

NS_EXTERN Ns_ReturnCode
Ns_VarGet(const char *server, const char *array, const char *keyString, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN bool
Ns_VarExists(const char *server, const char *array, const char *keyString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_VarSet(const char *server, const char *array, const char *keyString,
          const char *value, ssize_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_VarUnset(const char *server, const char *array, const char *keyString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_VarIncr(const char *server, const char *array, const char *keyString, int incr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_VarAppend(const char *server, const char *array, const char *keyString,
             const char *value, ssize_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

/*
 * sched.c:
 */

NS_EXTERN int
Ns_After(const Ns_Time *interval, Ns_SchedProc *proc, void *arg, ns_funcptr_t deleteProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_Cancel(int id);

NS_EXTERN bool
Ns_Pause(int id);

NS_EXTERN bool
Ns_Resume(int id);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN int
Ns_ScheduleProc(Ns_SchedProc *proc, void *arg, int thread, int secs)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(Ns_ScheduleProcEx);
#endif

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
                  const Ns_Time *interval, Ns_SchedProc *cleanupProc)
    NS_GNUC_NONNULL(1)  NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_UnscheduleProc(int id);

/*
 * set.c:
 */

NS_EXTERN size_t
Ns_SetUpdate(Ns_Set *set, const char *keyString, const char *valueString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN size_t
Ns_SetUpdateSz(Ns_Set *set,
               const char *keyString, TCL_SIZE_T keyLength,
               const char *valueString, TCL_SIZE_T valueLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN size_t
Ns_SetIUpdate(Ns_Set *set, const char *keyString, const char *valueString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN size_t
Ns_SetIUpdateSz(Ns_Set *set,
                const char *keyString, TCL_SIZE_T keyLength,
                const char *valueString, TCL_SIZE_T valueLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *
Ns_SetCreate(const char *name)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Set *
Ns_SetCreateSz(const char *name, size_t size)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Set *
Ns_SetRecreate(Ns_Set *set)
    NS_GNUC_NONNULL(1)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN Ns_Set *
Ns_SetRecreate2(Ns_Set **toPtr, Ns_Set *from)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_RETURNS_NONNULL;

NS_EXTERN void
Ns_SetFree(Ns_Set *set);

NS_EXTERN size_t
Ns_SetPut(Ns_Set *set, const char *key, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN size_t
Ns_SetPutSz(Ns_Set *set,
            const char *keyString, TCL_SIZE_T keyLength,
            const char *valueString, TCL_SIZE_T valueLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_SetUniqueCmp(const Ns_Set *set, const char *key,
                int (*cmp) (const char *s1, const char *s2))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_SetFindCmp(const Ns_Set *set, const char *key,
              int (*cmp) (const char *s1, const char *s2))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN const char *
Ns_SetGetCmp(const Ns_Set *set, const char *key,
             int (*cmp) (const char *s1, const char *s2))
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN bool
Ns_SetUnique(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PURE;

NS_EXTERN bool
Ns_SetIUnique(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_SetFind(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PURE;

NS_EXTERN int
Ns_SetIFind(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_SetGet(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_SetIGet(const Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetTrunc(Ns_Set *set, size_t size)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_SetDelete(Ns_Set *set, ssize_t index)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetPutValue(Ns_Set *set, size_t index, const char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_SetPutValueSz(Ns_Set *set, size_t index, const char *value, TCL_SIZE_T size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN bool
Ns_SetDeleteKey(Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_SetIDeleteKey(Ns_Set *set, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *
Ns_SetListFind(Ns_Set *const*sets, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN const char*
Ns_SetFormat(Tcl_DString *dsPtr, const Ns_Set *set, bool withName,
             const char *leadString, const char *separatorString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

NS_EXTERN Ns_Set **
Ns_SetSplit(const Ns_Set *set, char sep)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetListFree(Ns_Set **sets)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetMerge(Ns_Set *high, const Ns_Set *low)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetIMerge(Ns_Set *high, const Ns_Set *low)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *
Ns_SetCopy(const Ns_Set *old);

NS_EXTERN void
Ns_SetMove(Ns_Set *to, Ns_Set *from)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SetPrint(Tcl_DString *outputDsPtr, const Ns_Set *set)
    NS_GNUC_NONNULL(2);


NS_EXTERN const char *
Ns_SetGetValue(const Ns_Set *set, const char *key, const char *def)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_SetIGetValue(const Ns_Set *set, const char *key, const char *def)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_DStringAppendSet(Tcl_DString *dsPtr, const Ns_Set *set)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void Ns_SetClearValues(Ns_Set *set, TCL_SIZE_T maxAlloc)
    NS_GNUC_NONNULL(1);

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
Ns_SockListenEx(const char *address, unsigned short port, int backlog, bool reuseport);

NS_EXTERN NS_SOCKET
Ns_SockListenUdp(const char *address, unsigned short port, bool reuseport);

NS_EXTERN NS_SOCKET
Ns_SockListenRaw(int proto);

NS_EXTERN NS_SOCKET
Ns_SockListenUnix(const char *path, int backlog, unsigned short mode)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockBindUdp(const struct sockaddr *saPtr, bool reusePort)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockBindRaw(int proto);

NS_EXTERN NS_SOCKET
Ns_SockBindUnix(const char *path, int socktype, unsigned short mode)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
NsForkBinder(void);

NS_EXTERN void
NsStopBinder(void);

NS_EXTERN NS_SOCKET
Ns_SockBinderListen(char type, const char *address, unsigned short port, int options);

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
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PURE;

NS_EXTERN void
Ns_SlsSetKeyed(Ns_Sock *sock, const char *key, Tcl_Obj *valueObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN const char *
Ns_SlsGetKeyed(Ns_Sock *sock, const char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_SlsAppendKeyed(Tcl_DString *dest, Ns_Sock *sock)
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
Ns_SockSendFileBufs(Ns_Sock *sock, const Ns_FileVec *bufs, int nbufs, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_SockCork(const Ns_Sock *sock, bool cork)
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
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN Ns_ReturnCode
Ns_SockPipe(NS_SOCKET socks[2])
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_SockWait(NS_SOCKET sock, unsigned int what, int timeout);

NS_EXTERN Ns_ReturnCode
Ns_SockTimedWait(NS_SOCKET sock, unsigned int what, const Ns_Time *timeoutPtr);

NS_EXTERN ssize_t
Ns_SockRecv(NS_SOCKET sock, void *buffer, size_t length, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(2);

NS_EXTERN ssize_t
Ns_SockSend(NS_SOCKET sock, const void *buffer, size_t length, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SockSetReceiveState(Ns_Sock *sock, Ns_SockState sockState, unsigned long recvErrno)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SockSetSendErrno(Ns_Sock *sock, unsigned long sendErrno)
    NS_GNUC_NONNULL(1);

NS_EXTERN unsigned long
Ns_SockGetSendErrno(Ns_Sock *sock) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

NS_EXTERN ssize_t
Ns_SockGetSendRejected(Ns_Sock *sock) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_SockGetSendCount(Ns_Sock *sock) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

NS_EXTERN unsigned int
Ns_SockFlagAdd(Ns_Sock *sock, unsigned int flag) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

NS_EXTERN unsigned int
Ns_SockFlagClear(Ns_Sock *sock, unsigned int flag) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_SockInErrorState(const Ns_Sock *sock) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

unsigned short
Ns_SockGetPort(const Ns_Sock *sock)
    NS_GNUC_NONNULL(1);

const char *
Ns_SockGetAddr(const Ns_Sock *sock)
    NS_GNUC_NONNULL(1);

NS_EXTERN ssize_t
Ns_SockRecvBufs(Ns_Sock *sock, struct iovec *bufs, int nbufs,
                const Ns_Time *timeoutPtr, unsigned int flags);
NS_EXTERN ssize_t
Ns_SockRecvBufs2(NS_SOCKET sock, struct iovec *bufs, int nbufs, unsigned int flags,
                 Ns_SockState *sockStatePtr, unsigned long *errnoPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

NS_EXTERN ssize_t
Ns_SockSendBufs(Ns_Sock *sock, const struct iovec *bufs, int nbufs,
                const Ns_Time *timeoutPtr, unsigned int flags)
    NS_GNUC_NONNULL(1);

#ifdef NS_WITH_DEPRECATED_5_0
NS_EXTERN ssize_t
Ns_SockSendBufs2(NS_SOCKET sock, const struct iovec *bufs, int nbufs,
                 unsigned int flags)
    NS_GNUC_DEPRECATED_FOR(Ns_SockSendBufsEx);
#endif

NS_EXTERN ssize_t
Ns_SockSendBufsEx(NS_SOCKET sock, const struct iovec *bufs, int nbufs,
                  unsigned int flags, unsigned long *errorCodePtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN NS_SOCKET
Ns_BindSock(const struct sockaddr *saPtr)
    NS_GNUC_DEPRECATED_FOR(Ns_SockBind);
#endif

NS_EXTERN NS_SOCKET
Ns_SockBind(const struct sockaddr *saPtr, bool reusePort)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockListen(const char *address, unsigned short port);

NS_EXTERN NS_SOCKET
Ns_SockAccept(NS_SOCKET sock, struct sockaddr *saPtr, socklen_t *lenPtr);

NS_EXTERN NS_SOCKET
Ns_SockConnect(const char *host, unsigned short port)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockConnect2(const char *host, unsigned short port,
                const char *lhost, unsigned short lport)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockConnectUnix(const char *path, int socktype, Ns_ReturnCode *statusPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockAsyncConnect(const char *host, unsigned short port)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockAsyncConnect2(const char *host, unsigned short port,
                     const char *lhost, unsigned short lport)
    NS_GNUC_NONNULL(1);

NS_EXTERN NS_SOCKET
Ns_SockTimedConnect(const char *host, unsigned short port, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN NS_SOCKET
Ns_SockTimedConnect2(const char *host, unsigned short port,
                     const char *lhost, unsigned short lport,
                     const Ns_Time *timeoutPtr,
                     Ns_ReturnCode *statusPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(5);

NS_EXTERN void
Ns_SockConnectError(Tcl_Interp *interp,
                    const char *host, unsigned short portNr,
                    Ns_ReturnCode status,
                    const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_SockSetNonBlocking(NS_SOCKET sock);

NS_EXTERN Ns_ReturnCode
Ns_SockSetBlocking(NS_SOCKET sock);

NS_EXTERN void
Ns_SockSetNodelay(NS_SOCKET sock);

NS_EXTERN void
Ns_SockSetDeferAccept(NS_SOCKET sock, long secs);

NS_EXTERN void
Ns_SockSetKeepalive(NS_SOCKET sock, int optval);

NS_EXTERN Ns_ReturnCode
Ns_SockCloseLater(NS_SOCKET sock);

NS_EXTERN void
Ns_ClearSockErrno(void);

NS_EXTERN ns_sockerrno_t
Ns_GetSockErrno(void);

NS_EXTERN void
Ns_SetSockErrno(ns_sockerrno_t err);

NS_EXTERN const char *
Ns_SockStrError(ns_sockerrno_t err);

#ifdef _WIN32
NS_EXTERN char *
NsWin32ErrMsg(ns_sockerrno_t err);

NS_EXTERN NS_SOCKET
ns_sockdup(NS_SOCKET sock);

NS_EXTERN int
ns_socknbclose(NS_SOCKET sock);
#endif

NS_EXTERN int
Ns_SockErrorCode(Tcl_Interp *interp, NS_SOCKET sock);

const char *
Ns_PosixSetErrorCode(Tcl_Interp *interp, int errorNum)
    NS_GNUC_NONNULL(1);

NS_EXTERN struct sockaddr *
Ns_SockGetClientSockAddr(Ns_Sock *sock) NS_GNUC_CONST
    NS_GNUC_NONNULL(1);

NS_EXTERN struct sockaddr *
Ns_SockGetConfiguredSockAddr(Ns_Sock *sock)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

/*
 * sockaddr.c:
 */

NS_EXTERN bool
Ns_SockaddrMaskBits(struct sockaddr *mask, unsigned int nrBits)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_SockaddrMask(const struct sockaddr *addr, const struct sockaddr *mask, struct sockaddr *maskedAddr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN bool
Ns_SockaddrSameIP(const struct sockaddr *addr1, const struct sockaddr *addr2)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_PURE;

NS_EXTERN bool
Ns_SockaddrSetLoopback(struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
ns_inet_pton(struct sockaddr *saPtr, const char *addr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
ns_inet_ntop(const struct sockaddr *NS_RESTRICT saPtr, char *NS_RESTRICT buffer, size_t size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_GetSockAddr(struct sockaddr *saPtr, const char *host, unsigned short port)
    NS_GNUC_NONNULL(1);

NS_EXTERN unsigned short
Ns_SockaddrGetPort(const struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN void
Ns_SockaddrSetPort(struct sockaddr *saPtr, unsigned short port)
    NS_GNUC_NONNULL(1);

NS_EXTERN socklen_t
Ns_SockaddrGetSockLen(const struct sockaddr *saPtr)
        NS_GNUC_NONNULL(1) NS_GNUC_PURE;

NS_EXTERN void
Ns_LogSockaddr(Ns_LogSeverity severity, const char *prefix, const struct sockaddr *saPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN  Ns_ReturnCode
Ns_SockaddrParseIPMask(Tcl_Interp *interp, const char *ipString,
                       struct sockaddr *ipPtr, struct sockaddr *maskPtr,
                       unsigned int *nrBitsPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN bool
Ns_SockaddrMaskedMatch(const struct sockaddr *addr,
                       const struct sockaddr *mask,
                       const struct sockaddr *masked) NS_GNUC_PURE
        NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN bool
Ns_SockaddrPublicIpAddress(const struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_SockaddrTrustedReverseProxy(const struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_SockaddrInAny(const struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_SockaddrAddToDictIpProperties(const struct sockaddr *ipPtr, Tcl_Obj *dictObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * sockcallback.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_SockCallback(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when);

NS_EXTERN Ns_ReturnCode
Ns_SockCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when,
                  const Ns_Time *timeout, const char **threadNamePtr);

NS_EXTERN void
Ns_SockCancelCallback(NS_SOCKET sock);

NS_EXTERN Ns_ReturnCode
Ns_SockCancelCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, const char **threadNamePtr);

/*
 * str.c:
 */

NS_EXTERN char *
Ns_StrTrim(char *chars)
    NS_GNUC_NONNULL(1);

NS_EXTERN char *
Ns_StrTrimLeft(char *chars) NS_GNUC_CONST
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

NS_EXTERN Ns_ReturnCode
Ns_StrToInt(const char *chars, int *intPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_StrToWideInt(const char *chars, Tcl_WideInt *intPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_ReturnCode
Ns_StrToMemUnit(const char *chars, Tcl_WideInt *intPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_Match(const char *a, const char *b) NS_GNUC_CONST;

NS_EXTERN const char *
Ns_NextWord(const char *line) NS_GNUC_CONST
    NS_GNUC_NONNULL(1);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN const char *
Ns_StrNStr(const char *chars, const char *subString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_DEPRECATED_FOR(Ns_StrCaseFind);
#endif

NS_EXTERN const char *
Ns_StrCaseFind(const char *chars, const char *subString) NS_GNUC_CONST
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN bool
Ns_StrIsValidHostHeaderContent(const char *chars) NS_GNUC_CONST
    NS_GNUC_NONNULL(1);

NS_EXTERN const unsigned char *
Ns_GetBinaryString(Tcl_Obj *obj, bool forceBinary, TCL_SIZE_T *lengthPtr, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN bool
Ns_Valid_UTF8(const unsigned char *bytes, size_t nrBytes, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_Is7bit(const char *bytes, size_t nrBytes) NS_GNUC_PURE
    NS_GNUC_NONNULL(1);

NS_EXTERN ssize_t
Ns_UpperCharPos(const char *bytes, size_t nrBytes) NS_GNUC_CONST
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_TclReturnCodeString(int code) NS_GNUC_PURE;

NS_EXTERN const char *
Ns_ReturnCodeString(Ns_ReturnCode code) NS_GNUC_PURE;

NS_EXTERN const char *
NsSockErrorCodeString(unsigned long errorCode, char *buffer, size_t bufferSize)
    NS_GNUC_NONNULL(2);

NS_EXTERN const char *Ns_FilterTypeString(Ns_FilterType when);

/*
 * tclcallbacks.c:
 */

NS_EXTERN Ns_TclCallback *
Ns_TclNewCallback(Tcl_Interp *interp, ns_funcptr_t cbProc, Tcl_Obj *scriptObjPtr,
                  TCL_SIZE_T objc, Tcl_Obj *const* objv)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_TclEvalCallback(Tcl_Interp *interp, const Ns_TclCallback *cbPtr,
                   Tcl_DString *resultDString, ...) NS_GNUC_SENTINEL
    NS_GNUC_NONNULL(2);


NS_EXTERN Ns_Callback Ns_TclCallbackProc;
NS_EXTERN Ns_Callback Ns_TclFreeCallback;
NS_EXTERN Ns_ArgProc  Ns_TclCallbackArgProc;

/*
 * tclenv.c:
 */

NS_EXTERN char **
Ns_CopyEnviron(Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1);

NS_EXTERN char **
Ns_GetEnviron(void);

/*
 * tclfile.c:
 */

NS_EXTERN int
Ns_TclGetOpenChannel(Tcl_Interp *interp, const char *chanId, int write,
                     bool check, Tcl_Channel *chanPtr)
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

NS_EXTERN Ns_ReturnCode
Ns_TclEval(Tcl_DString *dsPtr, const char *server, const char *script)
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

NS_EXTERN Ns_ReturnCode
Ns_TclRegisterTrace(const char *server, Ns_TclTraceProc *proc, const void *arg, Ns_TclTraceType when)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_TclLibrary(const char *server)
    NS_GNUC_NONNULL(1);

NS_EXTERN const char *
Ns_TclInterpServer(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN const Ns_Server *
Ns_TclInterpServPtr(Tcl_Interp *interp)
     NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_TclInitModule(const char *server, const char *module)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN void
Ns_FreeConnInterp(Ns_Conn *conn)
     NS_GNUC_DEPRECATED_FOR(NsFreeConnInterp);

NS_EXTERN Ns_ReturnCode
Ns_TclRegisterAtCreate(Ns_TclTraceProc *proc, const void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(RegisterAt);

NS_EXTERN Ns_ReturnCode
Ns_TclRegisterAtCleanup(Ns_TclTraceProc *proc, const void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(RegisterAt);

NS_EXTERN Ns_ReturnCode
Ns_TclRegisterAtDelete(Ns_TclTraceProc *proc, const void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED_FOR(RegisterAt);

NS_EXTERN void
Ns_TclRegisterDeferred(Tcl_Interp *interp, Ns_TclDeferProc *proc, void *arg)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED;
#endif


/*
 * tclhttp.c
 */
NS_EXTERN bool
Ns_HttpParseHost2(char *hostString, bool strict,
                  const char **hostStart, const char **portStart, char **end)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN void
Ns_HttpParseHost(char *hostString, char **hostStart, char **portStart)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3)
    NS_GNUC_DEPRECATED_FOR(Ns_HttpParseHost2);
#endif

NS_EXTERN char *
Ns_HttpLocationString(Tcl_DString *dsPtr, const char *protoString,
                      const char *hostString,
                      unsigned short port, unsigned short defPort)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

/*
 * tclmisc.c
 */

NS_EXTERN bool
Ns_SetNamedVar(Tcl_Interp *interp, Tcl_Obj *varPtr, Tcl_Obj *valPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);


NS_EXTERN void Ns_TclPrintfResult(Tcl_Interp *interp, const char *fmt, ...)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2)
    NS_GNUC_PRINTF(2, 3);

NS_EXTERN const char *
Ns_TclLogErrorInfo(Tcl_Interp *interp, const char *extraInfo)
    NS_GNUC_NONNULL(1);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN const char *
Ns_TclLogError(Tcl_Interp *interp)
    NS_GNUC_NONNULL(1)
    NS_GNUC_DEPRECATED_FOR(Ns_TclLoggErrorInfo);

NS_EXTERN const char *
Ns_TclLogErrorRequest(Tcl_Interp *interp, Ns_Conn *conn)
    NS_GNUC_NONNULL(1)
    NS_GNUC_DEPRECATED_FOR(Ns_TclLogErrorInfo);
#endif

NS_EXTERN void
Ns_LogDeprecated(Tcl_Obj *const* objv, TCL_SIZE_T objc, const char *alternative, const char *explanation)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_LogDeprecatedParameter(const char *oldSection, const char *oldParameter,
                          const char *newSection, const char *newParameter, const char *explanation)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

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

NS_EXTERN char *
Ns_HexString(const unsigned char *octets, char *outputBuffer, TCL_SIZE_T size, bool isUpper)
    NS_GNUC_NONNULL(1,2);

/*
 * tclrequest.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_TclRequest(Ns_Conn *conn, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * tclset.c:
 */

NS_EXTERN int Ns_TclEnterSet(Tcl_Interp *interp, Ns_Set *set, Ns_TclSetType type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *Ns_TclGetSet(Tcl_Interp *interp, const char *setId)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int Ns_TclGetSet2(Tcl_Interp *interp, const char *setId, Ns_Set **setPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN int Ns_TclFreeSet(Tcl_Interp *interp, const char *setId)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Set *Ns_SetCreateFromDict(Tcl_Interp *interp, const char *name, Tcl_Obj *listObj, unsigned int flags)
    NS_GNUC_NONNULL(3);

/*
 * httptime.c:
 */

NS_EXTERN char *
Ns_HttpTime(Tcl_DString *dsPtr, const time_t *when)
    NS_GNUC_NONNULL(1);

NS_EXTERN time_t
Ns_ParseHttpTime(const char *chars)
    NS_GNUC_NONNULL(1);

/*
 * url.c:
 */

NS_EXTERN const char *
Ns_RelativeUrl(const char *url, const char *location) NS_GNUC_CONST;

NS_EXTERN Ns_ReturnCode
Ns_ParseUrl(char *url, bool strict, Ns_URL *urlPtr, const char **errorMsg)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN Ns_ReturnCode
Ns_AbsoluteUrl(Tcl_DString *dsPtr, const char *urlString, const char *baseString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN bool
Ns_PlainUrlPath(const char *url, const char **errorMsgPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

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

NS_EXTERN Ns_ReturnCode
Ns_UrlToFile(Tcl_DString *dsPtr, const char *server, const char *url)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN void
Ns_SetUrlToFileProc(const char *server, Ns_UrlToFileProc *procPtr)
    NS_GNUC_DEPRECATED_FOR(Ns_RegisterUrl2FileProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
#endif

NS_EXTERN void
Ns_RegisterFastUrl2File(const char *server, const char *url, const char *basePath, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Ns_Url2FileProc Ns_FastUrl2FileProc;


/*
 * urlencode.c:
 */

NS_EXTERN Tcl_Encoding
Ns_GetUrlEncoding(const char *charset);

NS_EXTERN char *
Ns_UrlPathEncode(Tcl_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_UrlPathDecode(Tcl_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_UrlQueryEncode(Tcl_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_UrlQueryDecode(Tcl_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding, Ns_ReturnCode *resultPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_CookieEncode(Tcl_DString *dsPtr, const char *cookie, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_CookieDecode(Tcl_DString *dsPtr, const char *cookie, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_Oauth1Encode(Tcl_DString *dsPtr, const char *cookie, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_Oauth1Decode(Tcl_DString *dsPtr, const char *cookie, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN char *
Ns_EncodeUrlWithEncoding(Tcl_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
    NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryEncode)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DecodeUrlWithEncoding(Tcl_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
     NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryDecode)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_EncodeUrlCharset(Tcl_DString *dsPtr, const char *urlSegment, const char *charset)
     NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryEncode)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_DecodeUrlCharset(Tcl_DString *dsPtr, const char *urlSegment, const char *charset)
     NS_GNUC_DEPRECATED_FOR(Ns_UrlQueryDecode)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
#endif

NS_EXTERN void
Ns_UrlEncodingWarnUnencoded(const char *msg, const char *inputStr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * urlopen.c:
 */
#ifdef NS_WITH_DEPRECATED
NS_EXTERN Ns_ReturnCode
Ns_FetchPage(Tcl_DString *dsPtr, const char *url, const char *server)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN Ns_ReturnCode
Ns_FetchURL(Tcl_DString *dsPtr, const char *url, Ns_Set *headers)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
#endif

/*
 * urlspace.c:
 */

NS_EXTERN int
Ns_UrlSpecificAlloc(void);

NS_EXTERN void
Ns_UrlSpecificWalk(int id, const char *server, Ns_WalkProc func, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN void
Ns_UrlSpecificSet(const char *server, const char *key, const char *url, int id,
                  void *data, unsigned int flags, Ns_Callback freeProc)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

NS_EXTERN void
Ns_UrlSpecificSet2(const char *server, const char *key, const char *url, int id,
                   void *data, unsigned int flags, Ns_Callback freeProc,
                   void *contextSpec)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5);

NS_EXTERN void *
Ns_UrlSpecificGet(Ns_Server *server, const char *key,
                  const char *url, int id, unsigned int flags, Ns_UrlSpaceOp op,
                  Ns_UrlSpaceMatchInfo *matchInfoPtr,
                  Ns_UrlSpaceContextFilterEvalProc proc, void *context)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

#ifdef NS_WITH_DEPRECATED
NS_EXTERN void *
Ns_UrlSpecificGetFast(const char *server, const char *key, const char *url, int id)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_DEPRECATED_FOR(Ns_UrlSpecificGet);
#endif

NS_EXTERN void *
Ns_UrlSpecificGetExact(const char *server, const char *key, const char *url,
                       int id, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void *
Ns_UrlSpecificDestroy(const char *server, const char *key, const char *url,
                      int id, unsigned int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

/*
 * fd.c:
 */

NS_EXTERN Ns_ReturnCode
Ns_CloseOnExec(int fd);

NS_EXTERN Ns_ReturnCode
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

#ifdef _WIN32
NS_EXTERN int
ns_mkstemp(char *charTemplate);
#endif

#ifndef HAVE_MKDTEMP
NS_EXTERN char *
ns_mkdtemp(char *charTemplate)
    NS_GNUC_NONNULL(1);
#endif

NS_EXTERN int
ns_poll(struct pollfd *fds, NS_POLL_NFDS_TYPE nfds, long timo)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_GetNameForUid(Tcl_DString *dsPtr, uid_t uid)
    NS_GNUC_NONNULL(1);

NS_EXTERN bool
Ns_GetNameForGid(Tcl_DString *dsPtr, gid_t gid);

NS_EXTERN bool
Ns_GetUserHome(Tcl_DString *dsPtr, const char *user)
    NS_GNUC_NONNULL(1);

NS_EXTERN long
Ns_GetUserGid(const char *user)
    NS_GNUC_NONNULL(1);

NS_EXTERN long
Ns_GetUid(const char *user)
    NS_GNUC_NONNULL(1);

NS_EXTERN long
Ns_GetGid(const char *group)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_SetUser(const char *user);

NS_EXTERN Ns_ReturnCode
Ns_SetGroup(const char *group);

/*
 * form.c:
 */
NS_EXTERN Ns_Set *
Ns_ConnGetQuery(Tcl_Interp *interp, Ns_Conn *conn, Tcl_Obj *fallbackCharsetObj, Ns_ReturnCode *rcPtr)
    NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnClearQuery(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_QueryToSet(char *query, Ns_Set *set, Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * tls.c:
 */

NS_EXTERN int
Ns_TLS_CtxClientCreate(Tcl_Interp *interp,
                       const char *cert, const char *caFile, const char *caPath, bool verify,
                       NS_TLS_SSL_CTX **ctxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(6);

NS_EXTERN int
Ns_TLS_CtxServerCreateCfg(Tcl_Interp *interp,
                          const char *cert, const char *caFile, const char *caPath,
                          bool verify, const char *ciphers, const char *ciphersuites,
                          const char *protocols, const char *alpn, void *app_data,
                          unsigned int flags, NS_TLS_SSL_CTX **ctxPtr)
    NS_GNUC_NONNULL(9) NS_GNUC_NONNULL(12);

NS_EXTERN int
Ns_TLS_CtxServerCreate(Tcl_Interp *interp,
                       const char *cert, const char *caFile, const char *caPath,
                       bool verify, const char *ciphers, const char *ciphersuites,
                       const char *protocols,
                       NS_TLS_SSL_CTX **ctxPtr)
    NS_GNUC_NONNULL(9);


NS_EXTERN int
Ns_TLS_CtxServerInit(const char *section, Tcl_Interp *interp, unsigned int flags, void* app_data,
                     NS_TLS_SSL_CTX **ctxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(5);

NS_EXTERN void
Ns_TLS_CtxFree(NS_TLS_SSL_CTX *ctx)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_ReturnCode
Ns_TLS_SSLConnect(Tcl_Interp *interp, NS_SOCKET sock, NS_TLS_SSL_CTX *ctx,
                  const char *sni_hostname, const char *caFile, const char *caPath,
                  const Ns_Time *timeoutPtr, NS_TLS_SSL **sslPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(8);

NS_EXTERN int
Ns_TLS_SSLAccept(Tcl_Interp *interp, NS_SOCKET sock,
                 NS_TLS_SSL_CTX *ctx, NS_TLS_SSL **sslPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

#ifdef HAVE_OPENSSL_EVP_H
NS_EXTERN ssize_t
Ns_SSLRecvBufs2(SSL *sslPtr, struct iovec *bufs, int UNUSED(nbufs), Ns_SockState *sockStatePtr, unsigned long *errnoPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

NS_EXTERN ssize_t
Ns_SSLSendBufs2(SSL *ssl, const struct iovec *bufs, int nbufs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN const char *
Ns_SSLSetErrorCode(Tcl_Interp *interp, unsigned long sslERRcode)
    NS_GNUC_NONNULL(1);
#endif


#endif /* NS_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
