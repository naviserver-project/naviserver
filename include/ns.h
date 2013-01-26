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
 * The following describe various properties of a connection.
 */

#define NS_CONN_CLOSED             0x001 /* The underlying socket is closed */
#define NS_CONN_SKIPHDRS           0x002 /* Client is HTTP/0.9, do not send HTTP headers  */
#define NS_CONN_SKIPBODY           0x004 /* HTTP HEAD request, do not send body */
#define NS_CONN_READHDRS           0x008 /* Unused */
#define NS_CONN_SENTHDRS           0x010 /* Response headers have been sent to client */
#define NS_CONN_WRITE_ENCODED      0x020 /* Character data mode requested mime-type header. */
#define NS_CONN_STREAM             0x040 /* Data is to be streamed when ready.  */
#define NS_CONN_STREAM_CLOSE       0x080 /* Writer Stream should be closed.  */
#define NS_CONN_CHUNK              0x100 /* Streamed data is to be chunked. */
#define NS_CONN_SENT_LAST_CHUNK    0x200 /* Marks that the last chunk was sent in chunked mode */
#define NS_CONN_SENT_VIA_WRITER    0x400 /* Response data has been sent via writer thread */
#define NS_CONN_SOCK_CORKED        0x800 /* underlying socket is corked */
#define NS_CONN_ZIPACCEPTED       0x1000 /* the request accepts zip encoding */
#define NS_CONN_ENTITYTOOLARGE    0x2000 /* the sent Entity was too large */
#define NS_CONN_REQUESTURITOOLONG 0x4000 /* request-URI too long */
#define NS_CONN_LINETOOLONG       0x8000 /* request Header line too long */

/*
 * The following are valid return codes from an Ns_UserAuthorizeProc.
 */

                                        /* NS_OK The user's access is authorized */
#define NS_UNAUTHORIZED            (-2) /* Bad user/passwd or unauthorized */
#define NS_FORBIDDEN               (-3) /* Authorization is not possible */
                                        /* NS_ERROR The authorization function failed */

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

                                        /* NS_OK Run next filter */
#define NS_FILTER_BREAK            (-4) /* Run next stage of connection */
#define NS_FILTER_RETURN           (-5) /* Close connection */

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
#define NS_SOCK_DONE               0x10 /* Task processing is done */
#define NS_SOCK_CANCEL             0x20 /* Remove event from sock callback thread */
#define NS_SOCK_TIMEOUT            0x40 /* Timeout waiting for socket event. */
#define NS_SOCK_INIT               0x80 /* Initialise a Task callback. */
#define NS_SOCK_ANY                (NS_SOCK_READ|NS_SOCK_WRITE|NS_SOCK_EXCEPTION)

/*
 * The following are valid comm driver options.
 */

#define NS_DRIVER_ASYNC            0x01 /* Use async read-ahead. */
#define NS_DRIVER_SSL              0x02 /* Use SSL port, protocol defaults. */
#define NS_DRIVER_NOPARSE          0x04 /* Do not parse request */

#define NS_DRIVER_VERSION_1        1    /* Obsolete. */
#define NS_DRIVER_VERSION_2        2    /* Current version. */

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


/*
 * The following flags define how Ns_Set's are managed by Tcl.
 */

#define NS_TCL_SET_STATIC          0 /* Ns_Set managed elsewhere, maintain a Tcl reference */
#define NS_TCL_SET_DYNAMIC         1 /* Tcl owns the Ns_Set and will free when finnished */

#define NS_COOKIE_SECURE           1  /* The cookie should only be sent using HTTPS */
#define NS_COOKIE_SCRIPTABLE       2  /* Available to javascript on the client. */

#ifdef _WIN32
#define ns_sockclose               closesocket
#define ns_sockioctl               ioctlsocket
#define ns_sockerrno               GetLastError()
#define ns_sockstrerror            NsWin32ErrMsg
#else
#define ns_sockclose               close
#define ns_socknbclose             close
#define ns_sockioctl               ioctl
#define ns_sockerrno               errno
#define ns_sockstrerror            strerror
#define ns_sockdup                 dup
#endif

/*
 * C API macros.
 */

#define UCHAR(c)                   ((unsigned char)(c))
#define STREQ(a,b)                 (((*a) == (*b)) && (strcmp((a),(b)) == 0))
#define STRIEQ(a,b)                (strcasecmp((a),(b)) == 0)
#define Ns_IndexCount(X)           ((X)->n)
#define Ns_ListPush(elem,list)     ((list)=Ns_ListCons((elem),(list)))
#define Ns_ListFirst(list)         ((list)->first)
#define Ns_ListRest(list)          ((list)->rest)
#define Ns_SetSize(s)              ((s)->size)
#define Ns_SetName(s)              ((s)->name)
#define Ns_SetKey(s,i)             ((s)->fields[(i)].name)
#define Ns_SetValue(s,i)           ((s)->fields[(i)].value)
#define Ns_SetLast(s)              (((s)->size)-1)

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
#define NS_DSTRING_STATIC_SIZE     TCL_DSTRING_STATIC_SIZE
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
typedef int   (Ns_SockProc) (NS_SOCKET sock, void *arg, int why);
typedef void  (Ns_TaskProc) (Ns_Task *task, NS_SOCKET sock, void *arg, int why);
typedef void  (Ns_EventProc) (Ns_Event *event, NS_SOCKET sock, void *arg, Ns_Time *now, int why);
typedef void  (Ns_SchedProc) (void *arg, int id);
typedef int   (Ns_ServerInitProc) (CONST char *server);
typedef int   (Ns_ModuleInitProc) (CONST char *server, CONST char *module);
typedef int   (Ns_RequestAuthorizeProc) (char *server, char *method,
			char *url, char *user, char *pass, char *peer);
typedef void  (Ns_AdpParserProc)(Ns_DString *outPtr, char *page);
typedef int   (Ns_UserAuthorizeProc) (char *user, char *passwd);
struct Ns_ObjvSpec;
typedef int   (Ns_ObjvProc) (struct Ns_ObjvSpec *spec, Tcl_Interp *interp,
                             int *objcPtr, Tcl_Obj *CONST objv[]);

typedef int (Ns_OptionConverter) (Tcl_Interp *interp, Tcl_Obj *labelPtr, 
				  Tcl_Obj *objPtr, ClientData *clientData);

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
} Ns_Request;

/*
 * The connection structure.
 */

typedef struct Ns_Conn {
    Ns_Request *request;
    Ns_Set     *headers;
    Ns_Set     *outputheaders;
    Ns_Set     *auth;
    size_t      contentLength;
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
    Ns_Callback    *cbProc;
    CONST char     *server;
    char           *script;
    int             argc;
    char          **argv;
} Ns_TclCallback;

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
    off_t     offset;   /* Offset into file to begin sending, or void *. */
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
                      struct sockaddr *sockaddr, int *socklen)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

typedef ssize_t
(Ns_DriverRecvProc)(Ns_Sock *sock, struct iovec *bufs, int nbufs,
                    Ns_Time *timeoutPtr, int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef ssize_t
(Ns_DriverSendProc)(Ns_Sock *sock, struct iovec *bufs, int nbufs,
                    Ns_Time *timeoutPtr, int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef ssize_t
(Ns_DriverSendFileProc)(Ns_Sock *sock, Ns_FileVec *bufs, int nbufs,
                        Ns_Time *timeoutPtr, int flags)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef int
(Ns_DriverRequestProc)(void *arg, Ns_Conn *conn)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

typedef int
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
    int                    opts;         /* NS_DRIVER_ASYNC | NS_DRIVER_SSL  */
    void                  *arg;          /* Module's driver callback data */
    char                  *path;         /* Path to find port, address, etc. */
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

#define SHA_HASHWORDS  5
#define SHA_BLOCKWORDS 16

typedef struct Ns_CtxSHA1 {
    unsigned int key[SHA_BLOCKWORDS];
    uint32_t iv[SHA_HASHWORDS];
#ifdef HAVE64
    uint64_t bytes;
#else
    uint32_t bytesHi, bytesLo;
#endif
} Ns_CtxSHA1;

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
    (void *arg, Ns_LogSeverity severity, Ns_Time *time, char *msg, size_t len);

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
 * adpcmds.c:
 */

NS_EXTERN int
Ns_AdpAppend(Tcl_Interp *interp, CONST char *buf, int len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpGetOutput(Tcl_Interp *interp, Tcl_DString **dsPtrPtr,
                int *streamPtr, size_t *maxBufferPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * adprequest.c:
 */

NS_EXTERN int
Ns_AdpRequest(Ns_Conn *conn, CONST char *file)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpRequestEx(Ns_Conn *conn, CONST char *file, Ns_Time *expiresPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_AdpFlush(Tcl_Interp *interp, int stream)
    NS_GNUC_NONNULL(1);

/*
 * auth.c:
 */

NS_EXTERN int
Ns_AuthorizeRequest(char *server, char *method, char *url,
			       char *user, char *passwd, char *peer);
NS_EXTERN void
Ns_SetRequestAuthorizeProc(char *server, Ns_RequestAuthorizeProc *procPtr);
NS_EXTERN void
Ns_SetUserAuthorizeProc(Ns_UserAuthorizeProc *procPtr);

NS_EXTERN int
Ns_AuthorizeUser(char *user, char *passwd);

/*
 * cache.c:
 */

#define NS_CACHE_FREE ns_free

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
                        Ns_Time *timeoutPtr, int cost) NS_GNUC_NONNULL(1);

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

#ifdef HAVE_ZLIB_H
# include <zlib.h>
#endif

typedef struct Ns_CompressStream {

#ifdef HAVE_ZLIB_H
    z_stream   z;
#endif
    int        flags;

} Ns_CompressStream;


NS_EXTERN int
Ns_CompressInit(Ns_CompressStream *)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CompressFree(Ns_CompressStream *)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_CompressBufsGzip(Ns_CompressStream *, struct iovec *bufs, int nbufs, Ns_DString *,
                    int level, int flush)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_CompressGzip(const char *buf, int len, Tcl_DString *outPtr, int level);

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
Ns_ConfigFlag(CONST char *section, CONST char *key, int flag, int def,
              int *flagsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(5);

NS_EXTERN int
Ns_ConfigInt(CONST char *section, CONST char *key, int def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConfigIntRange(CONST char *section, CONST char *key, int def,
                  int min, int max)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_ConfigWideInt(CONST char *section, CONST char *key, Tcl_WideInt def)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN Tcl_WideInt
Ns_ConfigWideIntRange(CONST char *section, CONST char *key, Tcl_WideInt def,
                  Tcl_WideInt min, Tcl_WideInt max)
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
Ns_ConfigGetInt64(CONST char *section, CONST char *key, int64_t *valuePtr)
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

NS_EXTERN Ns_Set *
Ns_ConfigCreateSection(CONST char *section);

NS_EXTERN void
Ns_GetVersion(int *major, int *minor, int *patch, int *type);

/*
 * conn.c:
 */

NS_EXTERN int
Ns_ConnId(Ns_Conn *conn);

NS_EXTERN int
Ns_ConnContentFd(Ns_Conn *conn);

NS_EXTERN size_t
Ns_ConnContentSize(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnContentFile(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnSetEncoding(Ns_Conn *conn, Tcl_Encoding encoding);

NS_EXTERN Tcl_Encoding
Ns_ConnGetEncoding(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding);

NS_EXTERN Tcl_Encoding
Ns_ConnGetUrlEncoding(Ns_Conn *conn);

NS_EXTERN int
Ns_ConnGetCompression(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnSetCompression(Ns_Conn *conn, int flag);

NS_EXTERN int
Ns_ConnModifiedSince(Ns_Conn *conn, time_t inTime);

NS_EXTERN int
Ns_ConnUnmodifiedSince(Ns_Conn *conn, time_t since);

NS_EXTERN int
Ns_ParseHeader(Ns_Set *set, char *header, Ns_HeaderCaseDisposition disp);

NS_EXTERN Ns_Set  *
Ns_ConnGetQuery(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnClearQuery(Ns_Conn *conn);

NS_EXTERN int
Ns_QueryToSet(char *query, Ns_Set *qset);

NS_EXTERN Ns_Set *
Ns_ConnAuth(Ns_Conn *conn);

NS_EXTERN Ns_Set *
Ns_ConnHeaders(Ns_Conn *conn);

NS_EXTERN Ns_Set *
Ns_ConnOutputHeaders(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnAuthUser(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnAuthPasswd(Ns_Conn *conn);

NS_EXTERN size_t
Ns_ConnContentLength(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnContent(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnServer(Ns_Conn *conn);

NS_EXTERN int
Ns_ConnResponseStatus(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnSetResponseStatus(Ns_Conn *conn, int new_status);

NS_EXTERN Tcl_WideInt
Ns_ConnContentSent(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnSetContentSent(Ns_Conn *conn, Tcl_WideInt length);

NS_EXTERN Tcl_WideInt
Ns_ConnResponseLength(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnPeer(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnSetPeer(Ns_Conn *conn, struct sockaddr_in *saPtr);

NS_EXTERN int
Ns_ConnPeerPort(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnLocation(Ns_Conn *conn) NS_GNUC_DEPRECATED;

NS_EXTERN char *
Ns_ConnLocationAppend(Ns_Conn *conn, Ns_DString *dest);

NS_EXTERN char *
Ns_ConnHost(Ns_Conn *conn);

NS_EXTERN int
Ns_ConnPort(Ns_Conn *conn);

NS_EXTERN NS_SOCKET
Ns_ConnSock(Ns_Conn *conn);

NS_EXTERN Ns_Sock *
Ns_ConnSockPtr(Ns_Conn *conn);

NS_EXTERN Ns_DString *
Ns_ConnSockContent(Ns_Conn *conn);

NS_EXTERN char *
Ns_ConnDriverName(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnSetUrlEncoding(Ns_Conn *conn, Tcl_Encoding encoding);

NS_EXTERN int
Ns_SetConnLocationProc(Ns_ConnLocationProc *proc, void *arg);

NS_EXTERN void
Ns_SetLocationProc(char *server, Ns_LocationProc *proc) NS_GNUC_DEPRECATED;

NS_EXTERN Ns_Time *
Ns_ConnStartTime(Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Time *
Ns_ConnAcceptTime(Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Time *
Ns_ConnQueueTime(Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Time *
Ns_ConnDequeueTime(Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Time *
Ns_ConnFilterTime(Ns_Conn *conn) NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnTimeStats(Ns_Conn *conn, Ns_Time *nowPtr, 
		 Ns_Time *acceptTimePtr, Ns_Time *queueTimePtr, 
		 Ns_Time *filterTimePtr, Ns_Time *runTimePtr)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);

NS_EXTERN int 
NsAsyncWrite(int fd, char *buffer, size_t nbyte) NS_GNUC_NONNULL(2);

NS_EXTERN void
NsAsyncWriterQueueDisable(int shutdown);

NS_EXTERN void
NsAsyncWriterQueueEnable();

NS_EXTERN Ns_Time *
Ns_ConnTimeout(Ns_Conn *conn) NS_GNUC_NONNULL(1);


/*
 * connio.c:
 */

NS_EXTERN int
Ns_ConnWriteChars(Ns_Conn *conn, CONST char *buf, size_t towrite, int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteVChars(Ns_Conn *conn, struct iovec *bufs, int nbufs, int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteData(Ns_Conn *conn, CONST void *buf, size_t towrite, int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnWriteVData(Ns_Conn *conn, struct iovec *bufs, int nbufs, int flags)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnSendFd(Ns_Conn *conn, int fd, Tcl_WideInt nsend)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnSendFp(Ns_Conn *conn, FILE *fp, Tcl_WideInt nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendChannel(Ns_Conn *conn, Tcl_Channel chan, Tcl_WideInt nsend)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendFileVec(Ns_Conn *conn, Ns_FileVec *bufs, int nbufs)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnSendDString(Ns_Conn *conn, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnPuts(Ns_Conn *conn, CONST char *string)
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
Ns_ConnGets(char *outBuffer, size_t inSize, Ns_Conn *conn)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN size_t
Ns_ConnRead(Ns_Conn *conn, void *vbuf, size_t toread)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReadLine(Ns_Conn *conn, Ns_DString *dsPtr, size_t *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReadHeaders(Ns_Conn *conn, Ns_Set *set, size_t *nreadPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnCopyToDString(Ns_Conn *conn, size_t ncopy, Ns_DString *dsPtr)
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
Ns_ConnInit(Ns_Conn *connPtr)
    NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_ConnWrite(Ns_Conn *conn, CONST void *buf, size_t towrite)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_WriteConn(Ns_Conn *conn, CONST char *buf, size_t towrite)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_WriteCharConn(Ns_Conn *conn, CONST char *buf, size_t towrite)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_CompleteHeaders(Ns_Conn *conn, Tcl_WideInt length, int flags, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1);

/*
 * cookies.c:
 */

NS_EXTERN void
Ns_ConnSetCookie(Ns_Conn *conn,  char *name, char *value, time_t maxage);

NS_EXTERN void
Ns_ConnSetSecureCookie(Ns_Conn *conn,  char *name, char *value, time_t maxage);

NS_EXTERN void
Ns_ConnSetCookieEx(Ns_Conn *conn,  char *name, char *value, time_t maxage,
                                  char *domain, char *path, int flags);
NS_EXTERN void
Ns_ConnDeleteCookie(Ns_Conn *conn, char *name, char *domain, char *path);

NS_EXTERN void
Ns_ConnDeleteSecureCookie(Ns_Conn *conn, char *name, char *domain, char *path);

NS_EXTERN char *
Ns_ConnGetCookie(Ns_DString *dest, Ns_Conn *conn, char *name);

/*
 * crypt.c:
 */

NS_EXTERN char *
Ns_Encrypt(char *pw, char *salt, char iobuf[ ]);

/*
 * dns.c:
 */

NS_EXTERN int
Ns_GetHostByAddr(Ns_DString *dsPtr, char *addr);

NS_EXTERN int
Ns_GetAddrByHost(Ns_DString *dsPtr, char *host);

NS_EXTERN int
Ns_GetAllAddrByHost(Ns_DString *dsPtr, char *host);

/*
 * driver.c:
 */

NS_EXTERN int Ns_DriverInit(char *server, char *module, Ns_DriverInitData *init);

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

NS_EXTERN char *
Ns_DStringVPrintf(Ns_DString *dsPtr, CONST char *fmt, va_list ap)
     NS_GNUC_NONNULL(2);

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

NS_EXTERN int
Ns_ExecProcess(char *exec, char *dir, int fdin, int fdout,
			  char *args, Ns_Set *env);

NS_EXTERN int
Ns_ExecProc(char *exec, char **argv);

NS_EXTERN int
Ns_ExecArgblk(char *exec, char *dir, int fdin, int fdout,
			 char *args, Ns_Set *env);

NS_EXTERN int
Ns_ExecArgv(char *exec, char *dir, int fdin, int fdout, char **argv, Ns_Set *env);

NS_EXTERN int
Ns_WaitProcess(int pid);

NS_EXTERN int
Ns_WaitForProcess(int pid, int *statusPtr);

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

NS_EXTERN void *
Ns_RegisterFilter(char *server, char *method, char *URL,
			       Ns_FilterProc *proc, int when, void *args);

NS_EXTERN void *
Ns_RegisterServerTrace(char *server, Ns_TraceProc *proc, void *arg);

NS_EXTERN void *
Ns_RegisterConnCleanup(char *server, Ns_TraceProc *proc, void *arg);

NS_EXTERN void *
Ns_RegisterCleanup(Ns_TraceProc *proc, void *arg);

/*
 * htuu.c
 */

NS_EXTERN size_t
Ns_HtuuEncode(unsigned char *string, size_t bufsize, char *buf);

NS_EXTERN size_t
Ns_HtuuDecode(char *string, unsigned char *buf, size_t bufsize);

/*
 * index.c:
 */

NS_EXTERN void
Ns_IndexInit(Ns_Index *indexPtr, int inc, int (*CmpEls) (const void *, const void *),
     			         int (*CmpKeyWithEl) (const void *, const void *));

NS_EXTERN void
Ns_IndexTrunc(Ns_Index*indexPtr);

NS_EXTERN void
Ns_IndexDestroy(Ns_Index *indexPtr);

NS_EXTERN Ns_Index *
Ns_IndexDup(Ns_Index *indexPtr);

NS_EXTERN void *
Ns_IndexFind(Ns_Index *indexPtr, void *key);

NS_EXTERN void *
Ns_IndexFindInf(Ns_Index *indexPtr, void *key);

NS_EXTERN void **
Ns_IndexFindMultiple(Ns_Index *indexPtr, void *key);

NS_EXTERN void
Ns_IndexAdd(Ns_Index *indexPtr, void *el);

NS_EXTERN void
Ns_IndexDel(Ns_Index *indexPtr, void *el);

NS_EXTERN void *
Ns_IndexEl(Ns_Index *indexPtr, int i);

NS_EXTERN void
Ns_IndexStringInit(Ns_Index *indexPtr, int inc);

NS_EXTERN Ns_Index *
Ns_IndexStringDup(Ns_Index *indexPtr);

NS_EXTERN void
Ns_IndexStringAppend(Ns_Index *addtoPtr, Ns_Index *addfromPtr);

NS_EXTERN void
Ns_IndexStringDestroy(Ns_Index *indexPtr);

NS_EXTERN void
Ns_IndexStringTrunc(Ns_Index *indexPtr);

NS_EXTERN void
Ns_IndexIntInit(Ns_Index *indexPtr, int inc);

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
Ns_ListCons(void *elem, Ns_List *lPtr);

NS_EXTERN Ns_List *
Ns_ListNreverse(Ns_List *lPtr);

NS_EXTERN Ns_List *
Ns_ListLast(Ns_List *lPtr);

NS_EXTERN void
Ns_ListFree(Ns_List *lPtr, Ns_ElemVoidProc *freeProc);

NS_EXTERN void
Ns_IntPrint(int d);

NS_EXTERN void
Ns_StringPrint(char *s);

NS_EXTERN void
Ns_ListPrint(Ns_List *lPtr, Ns_ElemVoidProc *printProc);

NS_EXTERN Ns_List *
Ns_ListCopy(Ns_List *lPtr);

NS_EXTERN int
Ns_ListLength(Ns_List *lPtr);

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
Ns_ListDeleteDuplicates(Ns_List *lPtr,
				        Ns_EqualProc *equalProc);

NS_EXTERN Ns_List *
Ns_ListNmapcar(Ns_List *lPtr, Ns_ElemValProc *valProc);

NS_EXTERN Ns_List *
Ns_ListMapcar(Ns_List *lPtr, Ns_ElemValProc *valProc);
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
Ns_CreateTaskQueue(char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_DestroyTaskQueue(Ns_TaskQueue *queue)
    NS_GNUC_NONNULL(1);

NS_EXTERN Ns_Task *
Ns_TaskCreate(NS_SOCKET sock, Ns_TaskProc *proc, void *arg)
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

NS_EXTERN int
Ns_ParseObjv(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec,
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
NS_EXTERN Ns_ObjvProc Ns_ObjvSet;

NS_EXTERN Ns_OptionConverter Ns_OptionObj;
NS_EXTERN Ns_OptionConverter Ns_OptionString;

#define Ns_NrElements(arr)  ((int) (sizeof(arr) / sizeof(arr[0])))

NS_EXTERN int
Ns_ParseOptions(CONST char *options[], Ns_OptionConverter *converter[], 
		ClientData clientData[], Tcl_Interp *interp, int offset, 
		int max, int *nextArg, int objc, Tcl_Obj *CONST objv[]);

/*
 * tclthread.c:
 */

NS_EXTERN int
Ns_TclThread(Tcl_Interp *interp, char *script, Ns_Thread *thrPtr)
     NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_TclDetachedThread(Tcl_Interp *interp, char *script);

/*
 * tcltime.c
 */

NS_EXTERN Tcl_Obj*
Ns_TclNewTimeObj(Ns_Time *timePtr);

NS_EXTERN void
Ns_TclSetTimeObj(Tcl_Obj *objPtr, Ns_Time *timePtr);

NS_EXTERN int
Ns_TclGetTimeFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Ns_Time *timePtr);

NS_EXTERN int
Ns_TclGetTimePtrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Ns_Time **timePtrPtr);

/*
 * tclxkeylist.c:
 */

NS_EXTERN char *
Tcl_DeleteKeyedListField (Tcl_Interp  *interp, CONST char *fieldName,
        CONST char *keyedList);

NS_EXTERN int
Tcl_GetKeyedListField (Tcl_Interp  *interp, CONST char *fieldName,
        CONST char *keyedList, char **fieldValuePtr);

NS_EXTERN int
Tcl_GetKeyedListKeys (Tcl_Interp  *interp, char CONST *subFieldName,
        CONST char *keyedList, int *keysArgcPtr, char ***keysArgvPtr);

NS_EXTERN char *
Tcl_SetKeyedListField (Tcl_Interp  *interp, CONST char *fieldName,
        CONST char *fieldvalue, CONST char *keyedList);

/*
 * listen.c:
 */

NS_EXTERN int
Ns_SockListenCallback(char *addr, int port, Ns_SockProc *proc, void *arg);

NS_EXTERN int
Ns_SockPortBound(int port);

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
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SetLogFlushProc(Ns_LogFlushProc *procPtr) NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_SetNsLogProc(Ns_LogProc *procPtr)  NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_AddLogFilter(Ns_LogFilter *procPtr, void *arg, Ns_Callback *freePtr);

NS_EXTERN void
Ns_RemoveLogFilter(Ns_LogFilter *procPtr, void *arg);

NS_EXTERN Ns_LogSeverity
Ns_CreateLogSeverity(CONST char *name)
    NS_GNUC_NONNULL(1);

NS_EXTERN CONST char *
Ns_LogSeverityName(Ns_LogSeverity severity);

NS_EXTERN int
Ns_LogSeverityEnabled(Ns_LogSeverity severity);


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

NS_EXTERN void
Nsd_LibInit(void);

NS_EXTERN int
Ns_Main(int argc, char **argv, Ns_ServerInitProc *initProc);

NS_EXTERN int
Ns_WaitForStartup(void);

NS_EXTERN void
Ns_StopServer(char *server);

/*
 * info.c:
 */

NS_EXTERN char *
Ns_InfoHomePath(void);

NS_EXTERN char *
Ns_InfoServerName(void);

NS_EXTERN char *
Ns_InfoServerVersion(void);

NS_EXTERN char *
Ns_InfoConfigFile(void);

NS_EXTERN int
Ns_InfoPid(void);

NS_EXTERN char *
Ns_InfoNameOfExecutable(void);

NS_EXTERN char *
Ns_InfoPlatform(void);

NS_EXTERN int
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
Ns_GetMimeType(CONST char *file)
    NS_GNUC_NONNULL(1);

/*
 * encoding.c:
 */

NS_EXTERN Tcl_Encoding
Ns_GetCharsetEncoding(CONST char *charset)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetCharsetEncodingEx(CONST char *charset, size_t len)
    NS_GNUC_NONNULL(1);

NS_EXTERN CONST char *
Ns_GetEncodingCharset(Tcl_Encoding encoding)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetTypeEncoding(CONST char *mimetype)
    NS_GNUC_NONNULL(1);

NS_EXTERN Tcl_Encoding
Ns_GetFileEncoding(CONST char *file)
    NS_GNUC_NONNULL(1);


NS_EXTERN Tcl_Encoding
Ns_GetEncoding(CONST char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;


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

NS_EXTERN void
Ns_SetThreadServer(char *server);

NS_EXTERN char *
Ns_GetThreadServer(void);

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

NS_EXTERN int
Ns_PathIsAbsolute(CONST char *path);

NS_EXTERN char *
Ns_NormalizePath(Ns_DString *dsPtr, CONST char *path);

NS_EXTERN char *
Ns_MakePath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_HashPath(Ns_DString *dsPtr, CONST char *string, int levels);

NS_EXTERN char *
Ns_LibPath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_BinPath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_HomePath(Ns_DString *dsPtr, ...) NS_GNUC_SENTINEL;

NS_EXTERN int
Ns_HomePathExists(char *path, ...) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_ModulePath(Ns_DString *dsPtr, CONST char *server, CONST char *module, ...) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_ServerPath(Ns_DString *dest, CONST char *server, ...) NS_GNUC_SENTINEL;

NS_EXTERN char *
Ns_PagePath(Ns_DString *dest, CONST char *server, ...) NS_GNUC_SENTINEL;

NS_EXTERN int
Ns_SetServerRootProc(Ns_ServerRootProc *proc, void *arg);

/*
 * proc.c:
 */

NS_EXTERN void
Ns_RegisterProcInfo(void *procAddr, char *desc, Ns_ArgProc *argProc);

NS_EXTERN void
Ns_GetProcInfo(Tcl_DString *dsPtr, void *procPtr, void *arg);

NS_EXTERN void
Ns_StringArgProc(Tcl_DString *dsPtr, void *arg);

/*
 * queue.c:
 */

NS_EXTERN Ns_Conn *
Ns_GetConn(void);

/*
 * quotehtml.c:
 */

NS_EXTERN void
Ns_QuoteHtml(Ns_DString *pds, char *string);

/*
 * request.c:
 */

NS_EXTERN void
Ns_FreeRequest(Ns_Request *request);

NS_EXTERN void
Ns_ResetRequest(Ns_Request *request);

NS_EXTERN int
Ns_ParseRequest(Ns_Request *request, CONST char *line);

NS_EXTERN char *
Ns_SkipUrl(Ns_Request *request, int n);

NS_EXTERN void
Ns_SetRequestUrl(Ns_Request *request, CONST char *url);

/*
 * return.c:
 */

NS_EXTERN void
Ns_ConnSetHeaders(Ns_Conn *conn, CONST char *field, CONST char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN void
Ns_ConnUpdateHeaders(Ns_Conn *conn, CONST char *field, CONST char *value)
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
Ns_ConnSetTypeHeader(Ns_Conn *conn, CONST char *type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetEncodedTypeHeader(Ns_Conn *conn, CONST char *mtype)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnSetLengthHeader(Ns_Conn *conn, Tcl_WideInt length)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetLastModifiedHeader(Ns_Conn *conn, time_t *mtime)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_ConnSetExpiresHeader(Ns_Conn *conn, CONST char *expires)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_ConnConstructHeaders(Ns_Conn *conn, Ns_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_ConnReturnNotice(Ns_Conn *conn, int status, CONST char *title,
                    CONST char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnAdminNotice(Ns_Conn *conn, int status, CONST char *title,
                         CONST char *notice)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnHtml(Ns_Conn *conn, int status, CONST char *html, ssize_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnCharData(Ns_Conn *conn, int status, CONST char *data, 
		      ssize_t len, CONST char *type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnData(Ns_Conn *conn, int status, CONST char *data, 
		  ssize_t len, CONST char *type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

NS_EXTERN int
Ns_ConnReturnOpenChannel(Ns_Conn *conn, int status, CONST char *type,
                         Tcl_Channel chan, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnOpenFile(Ns_Conn *conn, int status, CONST char *type,
                      FILE *fp, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(4);

NS_EXTERN int
Ns_ConnReturnOpenFd(Ns_Conn *conn, int status, CONST char *type, int fd, size_t len)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ConnReturnHeaderLineTooLong(Ns_Conn *conn);

NS_EXTERN int
Ns_ConnReturnRequestURITooLong(Ns_Conn *conn);

NS_EXTERN void
Ns_ConnSetRequiredHeaders(Ns_Conn *conn, CONST char *type, size_t length)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN void
Ns_ConnQueueHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN Tcl_WideInt
Ns_ConnFlushHeaders(Ns_Conn *conn, int status)
    NS_GNUC_NONNULL(1) NS_GNUC_DEPRECATED;

NS_EXTERN int
Ns_ConnResetReturn(Ns_Conn *conn)
    NS_GNUC_DEPRECATED;

/*
 * returnresp.c:
 */

NS_EXTERN void
Ns_RegisterReturn(int status, CONST char *url);

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
Ns_ConnReturnMoved(Ns_Conn *conn, CONST char *url)
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
Ns_ConnReturnUnavailable(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

/*
 * tclvar.c
 */

NS_EXTERN int
Ns_VarGet(CONST char *server, CONST char *array, CONST char *key,
          Ns_DString *dsPtr);

NS_EXTERN int
Ns_VarExists(CONST char *server, CONST char *array, CONST char *key);

NS_EXTERN int
Ns_VarSet(CONST char *server, CONST char *array, CONST char *key,
          CONST char *value, size_t len);

NS_EXTERN int
Ns_VarUnset(CONST char *server, CONST char *array, CONST char *key);

NS_EXTERN Tcl_WideInt
Ns_VarIncr(CONST char *server, CONST char *array, CONST char *key, int count);

NS_EXTERN int
Ns_VarAppend(CONST char *server, CONST char *array, CONST char *key,
             CONST char *value, size_t len);

/*
 * sched.c:
 */

NS_EXTERN int
Ns_After(int seconds, Ns_Callback *proc, void *arg, Ns_Callback *deleteProc);

NS_EXTERN int
Ns_Cancel(int id);

NS_EXTERN int
Ns_Pause(int id);

NS_EXTERN int
Ns_Resume(int id);

NS_EXTERN int
Ns_ScheduleProc(Ns_Callback *proc, void *arg, int thread, int interval);

NS_EXTERN int
Ns_ScheduleDaily(Ns_SchedProc *proc, void *arg, int flags,
			    int hour, int minute, Ns_SchedProc *cleanupProc);

NS_EXTERN int
Ns_ScheduleWeekly(Ns_SchedProc *proc, void *arg, int flags,
			     int day, int hour, int minute,
			     Ns_SchedProc *cleanupProc);

NS_EXTERN int
Ns_ScheduleProcEx(Ns_SchedProc *proc, void *arg, int flags,
			     int interval, Ns_SchedProc *cleanupProc);

NS_EXTERN void
Ns_UnscheduleProc(int id);

/*
 * set.c:
 */

NS_EXTERN void
Ns_SetUpdate(Ns_Set *set, CONST char *key, CONST char *value);

NS_EXTERN Ns_Set *
Ns_SetCreate(CONST char *name);

NS_EXTERN void
Ns_SetFree(Ns_Set *set);

NS_EXTERN int
Ns_SetPut(Ns_Set *set, CONST char *key, CONST char *value);

NS_EXTERN int
Ns_SetPutSz(Ns_Set *set, CONST char *key, CONST char *value, int size);

NS_EXTERN int
Ns_SetUniqueCmp(Ns_Set *set, CONST char *key,
                              int (*cmp) (CONST char *s1, CONST char *s2));

NS_EXTERN int
Ns_SetFindCmp(Ns_Set *set, CONST char *key,
                            int (*cmp) (CONST char *s1, CONST char *s2));

NS_EXTERN char *
Ns_SetGetCmp(Ns_Set *set, CONST char *key,
                             int (*cmp) (CONST char *s1, CONST char *s2));

NS_EXTERN int
Ns_SetUnique(Ns_Set *set, CONST char *key);

NS_EXTERN int
Ns_SetIUnique(Ns_Set *set, CONST char *key);

NS_EXTERN int
Ns_SetFind(Ns_Set *set, CONST char *key);

NS_EXTERN int
Ns_SetIFind(Ns_Set *set, CONST char *key);

NS_EXTERN char *
Ns_SetGet(Ns_Set *set, CONST char *key);

NS_EXTERN char *
Ns_SetIGet(Ns_Set *set, CONST char *key);

NS_EXTERN void
Ns_SetTrunc(Ns_Set *set, int size);

NS_EXTERN void
Ns_SetDelete(Ns_Set *set, int index);

NS_EXTERN void
Ns_SetPutValue(Ns_Set *set, int index, CONST char *value);

NS_EXTERN void
Ns_SetDeleteKey(Ns_Set *set, CONST char *key);

NS_EXTERN void
Ns_SetIDeleteKey(Ns_Set *set, CONST char *key);

NS_EXTERN Ns_Set *
Ns_SetListFind(Ns_Set **sets, CONST char *name);

NS_EXTERN Ns_Set **
Ns_SetSplit(Ns_Set *set, char sep);

NS_EXTERN void
Ns_SetListFree(Ns_Set **sets);

NS_EXTERN void
Ns_SetMerge(Ns_Set *high, Ns_Set *low);

NS_EXTERN Ns_Set *
Ns_SetCopy(Ns_Set *old);

NS_EXTERN void
Ns_SetMove(Ns_Set *to, Ns_Set *from);

NS_EXTERN void
Ns_SetPrint(Ns_Set *set);

NS_EXTERN char *
Ns_SetGetValue(Ns_Set *set, CONST char *key, CONST char *def);

NS_EXTERN char *
Ns_SetIGetValue(Ns_Set *set, CONST char *key, CONST char *def);


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
Ns_SockListenEx(char *address, int port, int backlog);

NS_EXTERN NS_SOCKET
Ns_SockListenUdp(char *address, int port);

NS_EXTERN NS_SOCKET
Ns_SockListenRaw(int proto);

NS_EXTERN NS_SOCKET
Ns_SockListenUnix(char *path, int backlog, int mode);

NS_EXTERN NS_SOCKET
Ns_SockBindUdp(struct sockaddr_in *saPtr);

NS_EXTERN NS_SOCKET
Ns_SockBindRaw(int proto);

NS_EXTERN NS_SOCKET
Ns_SockBindUnix(char *path, int socktype, int mode);

NS_EXTERN void
NsForkBinder(void);

NS_EXTERN void
NsStopBinder(void);

NS_EXTERN NS_SOCKET
Ns_SockBinderListen(int type, char *address, int port, int options);

/*
 * sls.s
 */

NS_EXTERN void
Ns_SlsAlloc(Ns_Sls *slsPtr, Ns_Callback *cleanup)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_SlsSet(Ns_Sls *slsPtr, Ns_Sock *sock, void *data)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void *
Ns_SlsGet(Ns_Sls *slsPtr, Ns_Sock *sock)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SlsSetKeyed(Ns_Sock *sock, CONST char *key, CONST char *value)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

NS_EXTERN char *
Ns_SlsGetKeyed(Ns_Sock *sock, CONST char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN char *
Ns_SlsAppendKeyed(Ns_DString *dest, Ns_Sock *sock)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void
Ns_SlsUnsetKeyed(Ns_Sock *sock, CONST char *key)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * sockfile.c:
 */

NS_EXTERN size_t
Ns_SetFileVec(Ns_FileVec *bufs, int i,  int fd, CONST void *data,
              off_t offset, size_t length)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ResetFileVec(Ns_FileVec *bufs, int nbufs, size_t sent)
    NS_GNUC_NONNULL(1);

NS_EXTERN ssize_t
Ns_SockSendFileBufs(Ns_Sock *sock, CONST Ns_FileVec *bufs, int nbufs,
                    Ns_Time *timeoutPtr, int flags)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN int
Ns_SockCork(Ns_Sock *sock, int cork);

/*
 * sock.c:
 */

NS_EXTERN size_t
Ns_SetVec(struct iovec *iov, int i, CONST void *data, size_t len)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_ResetVec(struct iovec *iov, int nbufs, size_t sent)
    NS_GNUC_NONNULL(1);

NS_EXTERN size_t
Ns_SumVec(struct iovec *iov, int nbufs)
    NS_GNUC_NONNULL(1);

NS_EXTERN int
Ns_SockWait(NS_SOCKET sock, int what, int timeout);
NS_EXTERN int
Ns_SockTimedWait(NS_SOCKET sock, int what, Ns_Time *timeoutPtr);
NS_EXTERN int
Ns_SockRecv(NS_SOCKET sock, void *vbuf, size_t nrecv,
	    Ns_Time *timeoutPtr);
NS_EXTERN int
Ns_SockSend(NS_SOCKET sock, void *vbuf, size_t nsend,
	    Ns_Time *timeoutPtr);
NS_EXTERN int
Ns_SockRecvBufs(NS_SOCKET sock, struct iovec *bufs, int nbufs,
		Ns_Time *timeoutPtr, int flags);
NS_EXTERN ssize_t
Ns_SockSendBufs(Ns_Sock *sockPtr, struct iovec *bufs, int nbufs,
		Ns_Time *timeoutPtr, int flags);

NS_EXTERN NS_SOCKET
Ns_BindSock(struct sockaddr_in *psa) NS_GNUC_DEPRECATED;

NS_EXTERN NS_SOCKET
Ns_SockBind(struct sockaddr_in *psa);

NS_EXTERN NS_SOCKET
Ns_SockListen(char *address, int port);

NS_EXTERN NS_SOCKET
Ns_SockAccept(NS_SOCKET sock, struct sockaddr *psa, int *lenPtr);

NS_EXTERN NS_SOCKET
Ns_SockConnect(char *host, int port);

NS_EXTERN NS_SOCKET
Ns_SockConnect2(char *host, int port, char *lhost, int lport);

NS_EXTERN NS_SOCKET
Ns_SockAsyncConnect(char *host, int port);

NS_EXTERN NS_SOCKET
Ns_SockAsyncConnect2(char *host, int port, char *lhost, int lport);

NS_EXTERN NS_SOCKET
Ns_SockTimedConnect(char *host, int port, Ns_Time *timeoutPtr);

NS_EXTERN NS_SOCKET
Ns_SockTimedConnect2(char *host, int port, char *lhost, int lport,
                                      Ns_Time *timeoutPtr);

NS_EXTERN int
Ns_SockSetNonBlocking(NS_SOCKET sock);

NS_EXTERN int
Ns_SockSetBlocking(NS_SOCKET sock);

NS_EXTERN void
Ns_SockSetDeferAccept(NS_SOCKET sock, int secs);

NS_EXTERN int
Ns_GetSockAddr(struct sockaddr_in *psa, char *host, int port);

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
NsWin32ErrMsg(int err);

NS_EXTERN NS_SOCKET
ns_sockdup(NS_SOCKET sock);

NS_EXTERN int
ns_socknbclose(NS_SOCKET sock);
#endif

/*
 * sockcallback.c:
 */

NS_EXTERN int
Ns_SockCallback(NS_SOCKET sock, Ns_SockProc *proc, void *arg, int when);

NS_EXTERN int
Ns_SockCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, int when, int timeout);

NS_EXTERN void
Ns_SockCancelCallback(NS_SOCKET sock);

NS_EXTERN int
Ns_SockCancelCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg);

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

NS_EXTERN int
Ns_StrToWideInt(CONST char *string, Tcl_WideInt *intPtr)
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

NS_EXTERN Ns_TclCallback *
Ns_TclNewCallback(Tcl_Interp *interp, Ns_Callback *cbPtr, Tcl_Obj *scriptObjPtr, int objc,
		  Tcl_Obj *CONST objv[]);

NS_EXTERN int
Ns_TclEvalCallback(Tcl_Interp *interp, Ns_TclCallback *cbPtr,
		   Ns_DString *result, ...) NS_GNUC_SENTINEL;

NS_EXTERN Ns_Callback Ns_TclCallbackProc;
NS_EXTERN Ns_Callback Ns_TclFreeCallback;
NS_EXTERN Ns_ArgProc  Ns_TclCallbackArgProc;

/*
 * tclenv.c:
 */

NS_EXTERN char **
Ns_CopyEnviron(Ns_DString *dsPtr);

NS_EXTERN char **
Ns_GetEnviron(void);

/*
 * tclfile.c:
 */

NS_EXTERN int
Ns_TclGetOpenChannel(Tcl_Interp *interp, char *chanId, int write,
				int check, Tcl_Channel *chanPtr);

NS_EXTERN int
Ns_TclGetOpenFd(Tcl_Interp *interp, char *chanId, int write, int *fdPtr);

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

NS_EXTERN void
Ns_LogDeprecated(Tcl_Obj *CONST objv[], int objc, char *alternative, char *explanation)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxMD5Init(Ns_CtxMD5 *ctx)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxMD5Update(Ns_CtxMD5 *ctx, unsigned const char *buf, unsigned len)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxMD5Final(Ns_CtxMD5 *ctx, unsigned char digest[16])
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxSHAInit(Ns_CtxSHA1 *ctx)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxSHAUpdate(Ns_CtxSHA1 *ctx, const unsigned char *buf, unsigned len)
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxSHAFinal(Ns_CtxSHA1 *ctx, unsigned char digest[20])
    NS_GNUC_NONNULL(1);

NS_EXTERN void
Ns_CtxString(unsigned char *digest, char *buf, int size)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

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

NS_EXTERN char *
Ns_HttpTime(Ns_DString *pds, time_t *when);

NS_EXTERN time_t
Ns_ParseHttpTime(char *str);

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

NS_EXTERN Tcl_Encoding
Ns_GetUrlEncoding(char *charset);

NS_EXTERN char *
Ns_UrlPathEncode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);

NS_EXTERN char *
Ns_UrlPathDecode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);

NS_EXTERN char *
Ns_UrlQueryEncode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);

NS_EXTERN char *
Ns_UrlQueryDecode(Ns_DString *dsPtr, char *str, Tcl_Encoding enc);

NS_EXTERN char *
Ns_EncodeUrlWithEncoding(Ns_DString *dsPtr, char *string,
                                         Tcl_Encoding encoding) NS_GNUC_DEPRECATED;

NS_EXTERN char *
Ns_DecodeUrlWithEncoding(Ns_DString *dsPtr, char *string,
                                         Tcl_Encoding encoding) NS_GNUC_DEPRECATED;

NS_EXTERN char *
Ns_EncodeUrlCharset(Ns_DString *dsPtr, char *string,
                                    char *charset) NS_GNUC_DEPRECATED;

NS_EXTERN char *
Ns_DecodeUrlCharset(Ns_DString *dsPtr, char *string,
                                    char *charset) NS_GNUC_DEPRECATED;

/*
 * urlopen.c:
 */

NS_EXTERN int
Ns_FetchPage(Ns_DString *pds, char *url, char *server);

NS_EXTERN int
Ns_FetchURL(Ns_DString *pds, char *url, Ns_Set *headers);

/*
 * urlspace.c:
 */

NS_EXTERN int
Ns_UrlSpecificAlloc(void);

NS_EXTERN void
Ns_UrlSpecificWalk(int id, CONST char *server, Ns_ArgProc func, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);

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

NS_EXTERN int
Ns_CloseOnExec(int fd);

NS_EXTERN int
Ns_NoCloseOnExec(int fd);

NS_EXTERN int
Ns_DupHigh(int *fdPtr);

NS_EXTERN int
Ns_GetTemp(void);

NS_EXTERN void
Ns_ReleaseTemp(int fd);

/*
 * unix.c, win32.c:
 */

NS_EXTERN int
ns_sockpair(NS_SOCKET *socks);

NS_EXTERN int
ns_pipe(int *fds);

NS_EXTERN int
ns_poll(struct pollfd *fds, unsigned long int nfds, int timo);

NS_EXTERN int
Ns_GetNameForUid(Ns_DString *dsPtr, int uid);

NS_EXTERN int
Ns_GetNameForGid(Ns_DString *dsPtr, int gid);

NS_EXTERN int
Ns_GetUserHome(Ns_DString *dsPtr, char *user);

NS_EXTERN int
Ns_GetUserGid(char *user);

NS_EXTERN int
Ns_GetUid(char *user);

NS_EXTERN int
Ns_GetGid(char *group);

NS_EXTERN int
Ns_SetUser(char *user);

NS_EXTERN int
Ns_SetGroup(char *group);

/*
 * form.c:
 */

NS_EXTERN void
Ns_ConnClearQuery(Ns_Conn *conn)
    NS_GNUC_NONNULL(1);

#endif /* NS_H */
