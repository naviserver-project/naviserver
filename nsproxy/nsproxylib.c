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
 * nsproxylib.c --
 *
 *      Library for ns_proxy commands and main loops.
 *
 *      TODO:
 *
 *      Expand the Req structure to pass:
 *
 *        o. Array of limits as with get/setrlimit
 *        o. Chroot of the worker process
 *        o. Limit duration of the execution in the worker process
 *        o. ...
 *
 *      Add -onexit for worker process to run on teardown
 *      Add channels to proxy, so we can talk to it
 */

#include "nsproxy.h"

static const char * NS_EMPTY_STRING = "";

#ifdef _WIN32
# define SIGKILL 9
# define SIGTERM 15

ssize_t writev(int fildes, const struct iovec *iov, int iovcnt);

/*
 * Minimal writev() and readv() emulation for windows. Must be probably
 * extended to be useful.
 */
ssize_t writev(int fildes, const struct iovec *iov, int iovcnt)
{
    ssize_t result = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t written = ns_write(fildes, iov[i].iov_base, iov[i].iov_len);

        if (written != iov[i].iov_len) {
            /*
             * Give up, since we did not receive the expected data.
             * Maybe overly cautious and we have to handle partial
             * writes.
             */
            result = -1;
            break;
        } else {
            result += written;
        }
    }

    return result;
}

ssize_t readv(int fildes, const struct iovec *iov, int iovcnt)
{
    ssize_t result = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t read = ns_read(fildes, iov[i].iov_base, iov[i].iov_len);

        if (read < 0) {
            result = -1;
            break;
        } else {
            result += read;
        }
    }

    return result;
}
#else
# include <grp.h>
# include <poll.h>
#endif

/*
 * It is pain in the neck to get a satisfactory definition of
 * u_int_XX_t or uintXX_t as different OS'es do that in different
 * header files and sometimes even do not define such types at all.
 * We choose to define them ourselves here and stop the blues.
 * This relies on the assumption that on both 32 and 64 bit machines
 * an int is always 32 and short is always 16 bits.
 */

typedef unsigned int   uint32;
typedef unsigned short uint16;

#define MAJOR_VERSION 1
#define MINOR_VERSION 1

/*
 * The following structure defines a running proxy worker process.
 */

typedef struct Worker {
    int           rfd;
    int           wfd;
    int           signal;
    int           sigsent;
    int           twait;
    pid_t         pid;
    Ns_Time       expire;
    struct Pool  *poolPtr;
    struct Worker *nextPtr;
} Worker;

/*
 * The following structures defines a proxy request and response.
 * The lengths are in network order to support later proxy
 * operation over a socket connection.
 */

typedef struct Req {
    uint32 len;         /* Length of the message */
    uint16 major;       /* Major version number */
    uint16 minor;       /* Minor version number */
} Req;

typedef struct Res {
    uint32 code;
    uint32 clen;
    uint32 ilen;
    uint32 rlen;
} Res;

/*
 * The following structure defines a proxy connection allocated
 * from a pool.
 */

typedef enum {
    Idle,  /* Ready to receive a script */
    Busy,  /* Evaluating a script */
    Done   /* Result is pending */
} ProxyState;

typedef struct ProxyConf {
    Ns_Time        tget;     /* Timeout when getting proxy handles */
    Ns_Time        teval;    /* Timeout when evaluating scripts */
    Ns_Time        tsend;    /* Timeout to send data to proxy over pipe */
    Ns_Time        trecv;    /* Timeout to receive results over pipe */
    Ns_Time        twait;    /* Timeout to wait for workers to die */
    Ns_Time        tidle;    /* Timeout for worker to be idle */
    Ns_Time        logminduration;  /* Log commands taking longer than this duration */
    int            maxruns;  /* Max number of proxy uses */
} ProxyConf;

typedef struct Proxy {
    struct Proxy  *nextPtr;  /* Next in list of proxies */
    struct Proxy  *runPtr;   /* Next in list of running proxies */
    struct Pool   *poolPtr;  /* Pointer to proxy's pool */
    char          *id;       /* Proxy unique string id */
    int            numruns;  /* Number of runs of this proxy */
    ProxyState     state;    /* Current proxy state (idle, busy etc) */
    ProxyConf      conf;     /* Copy from the pool configuration */
    Worker        *workerPtr; /* Running worker process, if any */
    Ns_Time        when;     /* Absolute time when the proxy is used */
    Tcl_HashEntry *idPtr;    /* Pointer to proxy table entry */
    Tcl_HashEntry *cntPtr;   /* Pointer to count of proxies allocated */
    Tcl_DString    in;       /* Request dstring */
    Tcl_DString    out;      /* Response dstring */
    Tcl_Command    cmdToken; /* Proxy Tcl command */
    Tcl_Interp    *interp;   /* Interp holding the proxy's Tcl command */
} Proxy;

/*
 * The following structure defines a proxy pool.
 */

typedef enum {
    Stopped,   /* Initial (startup) state */
    Starting,  /* It is in the process of startup */
    Running,   /* Operating on pools and tearing down workers */
    Sleeping,  /* Sleeping on cond var and waiting for work */
    Awaken,    /* Help state to distinguish from running */
    Stopping   /* Teardown of the thread initiated */
} ReaperState;

typedef struct Pool {
    const char    *name;     /* Name of pool */
    struct Proxy  *firstPtr; /* First in list of avail proxies */
    struct Proxy  *runPtr;   /* First in list of running proxies */
    const char    *exec;     /* Worker executable */
    const char    *init;     /* Init script to eval on proxy start */
    const char    *reinit;   /* Re-init scripts to eval on proxy put */
    int            waiting;  /* Thread waiting for handles */
    int            maxworker; /* Max number of allowed worker processes */
    int            nfree;    /* Current number of available proxy handles */
    int            nused;    /* Current number of used proxy handles */
    uintptr_t      nextid;   /* Next in proxy unique ids; corresponds to nr of workers */
    ProxyConf      conf;     /* Collection of config options to pass to proxy */
    Ns_Set         *env;     /* Set with environment to pass to proxy */
    Ns_Mutex       lock;     /* Lock around the pool */
    Ns_Cond        cond;     /* Cond for use while allocating handles */
    Ns_Time        runTime;  /* cumulated run times */
    uintptr_t      nruns;    /* number of runs in this pool */
} Pool;

#define MIN_IDLE_TIMEOUT_SEC 10 /* == 10 seconds */

/*
 * The following enum lists all possible error conditions.
 */

typedef enum Err {
    ENone,
    EBusy,
    EDead,
    EDeadlock,
    EExec,
    EGetTimeout,
    EIdle,
    EImport,
    EInit,
    ERange,
    ERecv,
    ESend,
    ENoWait,
    EEvalTimeout
} Err;

static const char *errMsg[] = {
    "no error",
    "currently evaluating a script",
    "child process died",
    "allocation deadlock",
    "could not create child process",
    "timeout waiting for handle",
    "no script evaluating",
    "invalid response",
    "init script failed",
    "insufficient handles",
    "result recv failed",
    "script send failed",
    "no wait for script result",
    "timeout waiting for evaluation",
    NULL
};

static const char *errCode[] = {
    "ENone",
    "EBusy",
    "EDead",
    "EDeadlock",
    "EExec",
    "EGetTimeout",
    "EIdle",
    "EImport",
    "EInit",
    "ERange",
    "ERecv",
    "ESend",
    "ENoWait",
    "EEvalTimeout",
    NULL
};

static Ns_LogSeverity Ns_LogNsProxyDebug = 0;


/*
 * Static functions defined in this file.
 */

static Tcl_ObjCmdProc ProxyObjCmd;
static Tcl_ObjCmdProc ConfigureObjCmd;
static Tcl_ObjCmdProc GetObjCmd;
static Tcl_ObjCmdProc StatsObjCmd;
static Tcl_ObjCmdProc ClearObjCmd;
static Tcl_ObjCmdProc StopObjCmd;

static Tcl_ObjCmdProc RunProxyCmd;
static Tcl_CmdDeleteProc DelProxyCmd;
static Tcl_InterpDeleteProc DeleteData;

static Ns_ShutdownProc Shutdown;

static Pool*  GetPool(const char *poolName, const InterpData *idataPtr) NS_GNUC_NONNULL(1);
static void   FreePool(Pool *poolPtr) NS_GNUC_NONNULL(1);

static Proxy* CreateProxy(Pool *poolPtr) NS_GNUC_NONNULL(1);
static Err    PopProxy(Pool *poolPtr, Proxy **proxyPtrPtr, int nwant, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void   PushProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static Proxy* GetProxy(const char *proxyId, InterpData *idataPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int    Eval(Tcl_Interp *interp, Proxy *proxyPtr, const char *script, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Err    Send(Tcl_Interp *interp, Proxy *proxyPtr, const char *script)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Err    Wait(Tcl_Interp *interp, Proxy *proxyPtr, const Ns_Time *timeoutPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Err    Recv(Tcl_Interp *interp, Proxy *proxyPtr, int *resultPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void   GetStats(const Proxy *proxyPtr)  NS_GNUC_NONNULL(1);

static Err    CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int    ReleaseProxy(Tcl_Interp *interp, Proxy *proxyPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void   CloseProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static int    CloseWorkerOfProxy(Proxy *proxyPtr, const char *proxyId, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);

static void   FreeProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static void   ResetProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static void   ProxyError(Tcl_Interp *interp, Err err) NS_GNUC_NONNULL(1);
static void   FmtActiveProxy(Tcl_Interp *interp, const Proxy *proxyPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void   ReleaseHandles(Tcl_Interp *interp, InterpData *idataPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Worker* ExecWorker(Tcl_Interp *interp, const Proxy *proxyPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Err    CreateWorker(Tcl_Interp *interp, Proxy *proxyPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void   SetExpire(Worker *workerPtr, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);
static bool   SendBuf(const Worker *workerPtr, const Ns_Time *timePtr, const Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static bool   RecvBuf(const Worker *workerPtr, const Ns_Time *timePtr, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static int    WaitFd(int fd, short events, long ms);

static int    Import(Tcl_Interp *interp, const Tcl_DString *dsPtr, int *resultPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static void   Export(Tcl_Interp *interp, int code, Tcl_DString *dsPtr)
    NS_GNUC_NONNULL(3);

static void   UpdateIov(struct iovec *iov, size_t n)
    NS_GNUC_NONNULL(1);
static void   SetOpt(const char *str, char const **optPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void   ReaperThread(void *UNUSED(arg));
static void   CloseWorker(Worker *workerPtr, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);
static void   ReapProxies(void);
static long   GetTimeDiff(const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1);

static void   AppendObj(Tcl_Obj *listObj, const char *flag, Tcl_Obj *obj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Tcl_Obj* StringObj(const char* chars);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable pools;     /* Tracks proxy pools */

static ReaperState reaperState = Stopped;

static Ns_Cond  pcond = NULL;          /* Those are used to control access to */
static Ns_Mutex plock = NULL;          /* The list of Worker structures of worker */
static Worker    *firstClosePtr = NULL; /* Processes which are being closed. */

static Tcl_DString defexec;             /* Stores full path of the proxy executable */


/*
 *----------------------------------------------------------------------
 *
 * Nsproxy_Init --
 *
 *      libnsproxy initialization.
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
Nsproxy_LibInit(void)
{
    static bool initialized = NS_FALSE;

    if (!initialized) {
        initialized = NS_TRUE;

        Ns_MutexInit(&plock);
        Ns_MutexSetName(&plock, "ns:proxy");

        Nsd_LibInit();

        Tcl_DStringInit(&defexec);
        Ns_BinPath(&defexec, "nsproxy", (char *)0L);
        Tcl_InitHashTable(&pools, TCL_STRING_KEYS);

        Ns_RegisterAtShutdown(Shutdown, NULL);
        Ns_RegisterProcInfo((ns_funcptr_t)Shutdown, "nsproxy:shutdown", NULL);

        Ns_LogNsProxyDebug = Ns_CreateLogSeverity("Debug(nsproxy)");
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyTclInit --
 *
 *      Initialize the Tcl interface.
 *
 * Results:
 *      TCL_OK.
 *
 * Side effects:
 *      Adds the ns_proxy command to given interp.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ProxyTclInit(Tcl_Interp *interp)
{
    InterpData *idataPtr;

    idataPtr = ns_calloc(1u, sizeof(InterpData));
    Tcl_InitHashTable(&idataPtr->ids, TCL_STRING_KEYS);
    Tcl_InitHashTable(&idataPtr->cnts, TCL_ONE_WORD_KEYS);
    Tcl_SetAssocData(interp, ASSOC_DATA, DeleteData, idataPtr);
    (void)Tcl_CreateObjCommand(interp, "ns_proxy", ProxyObjCmd, idataPtr, NULL);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyMain --
 *
 *      Main loop for nsproxy worker processes. Initialize Tcl interp and loop
 *      processing requests. On communication errors or when the peer closes
 *      it's write-pipe, worker process exits gracefully.
 *
 * Results:
 *      Always zero.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ProxyMain(int argc, char *const*argv, Tcl_AppInitProc *init)
{
    Tcl_Interp  *interp;
    Worker        proc;
    int          result, max;
    Tcl_DString  in, out;
    const char  *script, *dots, *uarg = NULL, *user;
    char        *group = NULL, *active;
    uint16       major, minor;
    size_t       activeSize;

    /*
     * The call to Tcl_FindExecutable() must be done before we ever
     * attempt any Tcl related call.
     */
    Tcl_FindExecutable(argv[0]);

    Nsproxy_LibInit();

    if (argc > 4 || argc < 3) {
        char *pgm = strrchr(argv[0], INTCHAR('/'));
        Ns_Fatal("usage: %s pool id ?command?", (pgm != NULL) ? (pgm+1) : argv[0]);
    }
    if (argc < 4) {
        active = NULL;
        activeSize = 0;
        max = -1;
    } else {
        active = argv[3];
        activeSize = strlen(active);
        max = (int)activeSize - 8;
        if (max < 0) {
            active = NULL;
        }
    }

    /*
     * Initialize Worker structure
     */
    memset(&proc, 0, sizeof(proc));

    /*
     * Move the proxy input and output fd's from 0 and 1 to avoid
     * protocol errors with scripts accessing stdin and stdout.
     * Stdin is open on /dev/null and stdout is dup'ed to stderr.
     */

    major = htons(MAJOR_VERSION);
    minor = htons(MINOR_VERSION);
    proc.pid = NS_INVALID_PID;

    proc.rfd = ns_dup(0);
    if (proc.rfd < 0) {
        Ns_Fatal("nsproxy: dup: %s", strerror(errno));
    }
    proc.wfd = ns_dup(1);
    if (proc.wfd < 0) {
        Ns_Fatal("nsproxy: dup: %s", strerror(errno));
    }
    ns_close(0);
    if (ns_open("/dev/null", O_RDONLY | O_CLOEXEC, 0) != 0) {
        Ns_Fatal("nsproxy: open: %s", strerror(errno));
    }
    ns_close(1);
    if (ns_dup(2) != 1) {
        Ns_Fatal("nsproxy: dup: %s", strerror(errno));
    }

    /*
     * Make sure possible child processes do not inherit this one.
     * As, when the user evaluates the "exec" command, the child
     * process(es) will otherwise inherit the descriptor and keep
     * it open even if the proxy process is killed in the meantime.
     * This will of course block the caller, possibly forever.
     */

    (void)Ns_CloseOnExec(proc.wfd);

    /*
     * Create the interp, initialize with user init proc, if any.
     */

    interp = Ns_TclCreateInterp();
    if (init != NULL) {
        if ((*init)(interp) != TCL_OK) {
            Ns_Fatal("nsproxy: init: %s", Tcl_GetStringResult(interp));
        }
    }

    /*
     * Parse encoded user/group information. Those are
     * optionally encoded in the passed pool name:
     *
     *    pool?:username_or_uid?:groupname_or_gid??
     *
     * Examples:
     *
     *    mypool
     *    mypool:myname
     *    mypool:myname:mygroup
     *
     * The uid/gid fiddling code is replicated from the Ns_Main().
     *
     * etc...
     */

    user = strchr(argv[1], INTCHAR(':'));
    if (user != NULL) {
        uarg = ns_strdup(user + 1);
        user = uarg;
        group = strchr(user, INTCHAR(':'));
        if (group != NULL) {
            *group = 0;
            group++;
        }
    }

    if (Ns_SetGroup(group) == NS_ERROR || Ns_SetUser(user) == NS_ERROR) {
        Ns_Fatal("nsproxy: unable to switch to user '%s', group '%s'", user, group);
    }

    /*
     * Loop continuously processing proxy requests.
     */

    Tcl_DStringInit(&in);
    Tcl_DStringInit(&out);

    while (RecvBuf(&proc, NULL, &in) == NS_TRUE) {
        Req      req, *reqPtr = &req;
        uint32_t len;

        if (Tcl_DStringLength(&in) < (int)sizeof(Req)) {
            break;
        }

        memcpy(&req, in.string, sizeof(req));

        if (reqPtr->major != major || reqPtr->minor != minor) {
            Ns_Fatal("nsproxy: version mismatch");
        }
        len = ntohl(reqPtr->len);
        if (len == 0) {
            Export(NULL, TCL_OK, &out);
        } else if (len > 0) {
            script = Tcl_DStringValue(&in) + sizeof(Req);
            if (active != NULL) {
                int n = (int)len;

                if (n < max) {
                    dots = NS_EMPTY_STRING;
                } else {
                    dots = " ...";
                    n = max;
                }
                snprintf(active, activeSize, "{%.*s%s}", n, script, dots);
            }
            result = Tcl_EvalEx(interp, script, (int)len, 0);
            Export(interp, result, &out);
            if (active != NULL) {
                assert(max > 0);
                memset(active, ' ', (size_t)max);
            }
        } else {
            Ns_Fatal("nsproxy: invalid length");
        }
        if (SendBuf(&proc, NULL, &out) == NS_FALSE) {
            break;
        }
        Tcl_DStringSetLength(&in, 0);
        Tcl_DStringSetLength(&out, 0);
    }

    if (uarg != NULL) {
        ns_free((char *)uarg);
    }
    Tcl_DStringFree(&in);
    Tcl_DStringFree(&out);

    return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyCleanup --
 *
 *      Tcl trace to release any proxy handles
 *      held in the current interp
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
Ns_ProxyCleanup(Tcl_Interp *interp, const void *UNUSED(arg))
{
    InterpData *idataPtr = Tcl_GetAssocData(interp, ASSOC_DATA, NULL);

    if (idataPtr != NULL) {
        ReleaseHandles(interp, idataPtr);
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Shutdown --
 *
 *      Server trace to timely shutdown proxy system
 *      including stopping the reaper thread.
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
Shutdown(const Ns_Time *timeoutPtr, void *UNUSED(arg))
{
    Pool           *poolPtr;
    Proxy          *proxyPtr, *tmpPtr;
    Tcl_HashSearch  search;
    int             reap;
    Ns_ReturnCode   status;

    /*
     * Cleanup all known pools. This will put all idle
     * proxies on the close list. At this point, there
     * should be no running nor detached proxies.
     * If yes, we will leak memory on exit (proxies and
     * the whole pool will be left un-freed).
     */

    if (timeoutPtr == NULL) {
        Tcl_HashEntry *hPtr;

        Ns_MutexLock(&plock);
        hPtr = Tcl_FirstHashEntry(&pools, &search);
        while (hPtr != NULL) {
            poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
            Ns_MutexLock(&poolPtr->lock);
            poolPtr->maxworker = 0; /* Disable creation of new workers */
            proxyPtr = poolPtr->firstPtr;
            while (proxyPtr != NULL) {
                if (proxyPtr->workerPtr != NULL) {
                    CloseWorker(proxyPtr->workerPtr, &proxyPtr->conf.twait);
                }
                tmpPtr = proxyPtr->nextPtr;
                FreeProxy(proxyPtr);
                proxyPtr = tmpPtr;
            }
            Ns_MutexUnlock(&poolPtr->lock);
            Tcl_DeleteHashEntry(hPtr);
            if (poolPtr->nused == 0) {
                FreePool(poolPtr);
            } else {
                Ns_Log(Warning, "nsproxy: [%s]: has %d used proxies",
                       poolPtr->name, poolPtr->nused);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(&pools);
        Ns_MutexUnlock(&plock);
        return;
    }

    Ns_MutexLock(&plock);
    reap = firstClosePtr != NULL || reaperState != Stopped;
    Ns_MutexUnlock(&plock);

    if (reap == 0) {
        return;
    }

    /*
     * There is something on the close list. Start
     * the reaper thread if not done already and
     * wait for it to gracefully exit.
     */

    Ns_Log(Notice, "nsproxy: shutdown started");
    ReapProxies();
    Ns_MutexLock(&plock);
    reaperState = Stopping;
    status = NS_OK;
    Ns_CondSignal(&pcond);
    while (reaperState != Stopped && status == NS_OK) {
        status = Ns_CondTimedWait(&pcond, &plock, timeoutPtr);
        if (status != NS_OK) {
            Ns_Log(Warning, "nsproxy: timeout waiting for reaper exit");
        }
    }
    Ns_MutexUnlock(&plock);

    Ns_Log(Notice, "nsproxy: shutdown complete");
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyGet --
 *
 *      Get one proxy handle for the given pool.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Ns_ProxyGet(Tcl_Interp *interp, const char *poolName, PROXY* handlePtr, Ns_Time *timePtr)
{
    Pool  *poolPtr;
    Proxy *proxyPtr;
    Err    err;
    int    result;

    /*
     * Get just one proxy from the pool
     */
    poolPtr = GetPool(poolName, NULL);

    err = PopProxy(poolPtr, &proxyPtr, 1, timePtr);
    if (unlikely(err != 0)) {
        Ns_TclPrintfResult(interp, "could not allocate from pool \"%s\": %s",
                           poolPtr->name, errMsg[err]);
        ProxyError(interp, err);
        result = TCL_ERROR;

    } else if (CheckProxy(interp, proxyPtr) != ENone) {
        /*
         * No proxy connection.
         */
        PushProxy(proxyPtr);
        Ns_CondBroadcast(&poolPtr->cond);
        result = TCL_ERROR;

    } else {
        /*
         * Valid proxy for connection.
         */
        *handlePtr = (PROXY *)proxyPtr;
         result = TCL_OK;

    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyPut --
 *
 *      Return the proxy handle back.
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
Ns_ProxyPut(PROXY handle)
{
    PushProxy((Proxy *)handle);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyEval --
 *
 *      Evaluates the script in the proxy.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int Ns_ProxyEval(Tcl_Interp *interp, PROXY handle, const char *script, const Ns_Time *timeoutPtr)
{
    return Eval(interp, (Proxy *)handle, script, timeoutPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * ExecWorker --
 *
 *      Create a new proxy worker process.
 *
 * Results:
 *      Pointer to new Worker or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Worker *
ExecWorker(Tcl_Interp *interp, const Proxy *proxyPtr)
{
    Pool  *poolPtr;
    char  *argv[5];
    char   active[100];
    Worker *workerPtr;
    int    rpipe[2], wpipe[2];
    size_t len;
    pid_t  pid;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    poolPtr = proxyPtr->poolPtr;
    len = sizeof(active) - 1;
    memset(active, ' ', len);
    active[len] = '\0';

    Ns_MutexLock(&poolPtr->lock);
    argv[0] = ns_strdup(poolPtr->exec);
    argv[1] = ns_strdup(poolPtr->name);
    Ns_MutexUnlock(&poolPtr->lock);

    argv[2] = proxyPtr->id;
    argv[3] = active;
    argv[4] = NULL;

    if (ns_pipe(rpipe) != 0) {
        Ns_TclPrintfResult(interp, "pipe failed: %s", Tcl_PosixError(interp));
        return NULL;
    }
    if (ns_pipe(wpipe) != 0) {
        Ns_TclPrintfResult(interp, "pipe failed: %s", Tcl_PosixError(interp));
        ns_close(rpipe[0]);
        ns_close(rpipe[1]);
        return NULL;
    }

    pid = Ns_ExecArgv(poolPtr->exec, NULL, rpipe[0], wpipe[1], argv, poolPtr->env);

    ns_close(rpipe[0]);
    ns_close(wpipe[1]);

    ns_free(argv[0]);
    ns_free(argv[1]);

    if (pid == NS_INVALID_PID) {
        Ns_TclPrintfResult(interp, "exec failed: %s", Tcl_PosixError(interp));
        ns_close(wpipe[0]);
        ns_close(rpipe[1]);
        return NULL;
    }

    workerPtr = ns_calloc(1u, sizeof(Worker));
    workerPtr->poolPtr = proxyPtr->poolPtr;
    workerPtr->pid = pid;
    workerPtr->rfd = wpipe[0];
    workerPtr->wfd = rpipe[1];

    SetExpire(workerPtr, &proxyPtr->conf.tidle);

    Ns_Log(Ns_LogNsProxyDebug, "nsproxy: worker process %ld started", (long) workerPtr->pid);

    return workerPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * SetExpire --
 *
 *      Sets the absolute expire time for the worker process.
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
SetExpire(Worker *workerPtr, const Ns_Time *timePtr)
{
    NS_NONNULL_ASSERT(workerPtr != NULL);

    Ns_Log(Ns_LogNsProxyDebug, "set expire in %ld ms for pool %s worker %ld",
           timePtr == NULL ? -1 : Ns_TimeToMilliseconds(timePtr),
           workerPtr->poolPtr->name, (long)workerPtr->pid);

    if (timePtr != NULL) {
        Ns_GetTime(&workerPtr->expire);
        Ns_IncrTime(&workerPtr->expire, timePtr->sec, timePtr->usec);
    } else {
        workerPtr->expire.sec  = TIME_T_MAX;
        workerPtr->expire.usec = 0;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Eval --
 *
 *      Send a script and wait for and receive a response.
 *
 * Results:
 *      Tcl result code from evaluating script, or TCL_ERROR if
 *      any communication errors or timeouts.
 *
 * Side effects:
 *      Will leave proxy response or error message in interp.
 *
 *----------------------------------------------------------------------
 */

static int
Eval(Tcl_Interp *interp, Proxy *proxyPtr, const char *script, const Ns_Time *timeoutPtr)
{
    Err     err;
    int     status = TCL_ERROR;
    Ns_Time startTime;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    Ns_GetTime(&startTime);

    err = Send(interp, proxyPtr, script);
    if (err == ENone) {
        err = Wait(interp, proxyPtr, timeoutPtr);
        if (err == ENone) {
            (void) Recv(interp, proxyPtr, &status);
        }
        /*
         * Don't count check-proxy calls (script == NULL)
         */
        if (script != NULL) {
            Ns_Time endTime, diffTime;

            Ns_GetTime(&endTime);
            (void)Ns_DiffTime(&endTime, &startTime, &diffTime);
            if (Ns_DiffTime(&proxyPtr->conf.logminduration, &diffTime, NULL) < 1) {
                Ns_Log(Notice, "nsproxy %s duration %" PRId64 ".%06ld secs: '%s'",
                       proxyPtr->poolPtr->name, (int64_t)diffTime.sec, diffTime.usec, script);
            }

            Ns_Log(Debug, "Eval calls GetStats <%s>", script);
            GetStats(proxyPtr);
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * GetStats --
 *
 *      Obtain run time statistics
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Update the pool's run time
 *
 *----------------------------------------------------------------------
 */
static void
GetStats(const Proxy *proxyPtr)
{
    Ns_Time now, runTimeSpan;

    NS_NONNULL_ASSERT(proxyPtr != NULL);

    Ns_GetTime(&now);
    Ns_DiffTime(&now, &proxyPtr->when, &runTimeSpan);
    Ns_IncrTime(&proxyPtr->poolPtr->runTime, runTimeSpan.sec, runTimeSpan.usec);
    proxyPtr->poolPtr->nruns++;
}

/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Send a script to a proxy.
 *
 * Results:
 *      Proxy Err code.
 *
 * Side effects:
 *      Will format error message in given interp on failure.
 *
 *----------------------------------------------------------------------
 */

static Err
Send(Tcl_Interp *interp, Proxy *proxyPtr, const char *script)
{
    Err err = ENone;
    Req req;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    if (proxyPtr->workerPtr == NULL) {
        err = EDead;
    } else if (proxyPtr->state != Idle) {
        err = EBusy;
    } else {
        proxyPtr->numruns++;
        if (proxyPtr->conf.maxruns > 0
            && proxyPtr->numruns > proxyPtr->conf.maxruns) {
            Ns_Log(Notice, "proxy maxrun reached pool %s worker %ld",
                   proxyPtr->poolPtr->name, (long)proxyPtr->workerPtr->pid);
            CloseProxy(proxyPtr);
            err = CreateWorker(interp, proxyPtr);
        }
        if (err == ENone) {
            size_t len = script == NULL ? 0u : strlen(script);

            req.len   = htonl((uint32_t)len);
            req.major = htons(MAJOR_VERSION);
            req.minor = htons(MINOR_VERSION);
            Tcl_DStringSetLength(&proxyPtr->in, 0);
            Tcl_DStringAppend(&proxyPtr->in, (char *) &req, sizeof(req));
            if (len > 0u) {
                Tcl_DStringAppend(&proxyPtr->in, script, (int)len);
            }
            proxyPtr->state = Busy;

            /*
             * Proxy is active, put it on the
             * head of the run queue,
             */

            Ns_GetTime(&proxyPtr->when);

            Ns_MutexLock(&proxyPtr->poolPtr->lock);
            proxyPtr->runPtr = proxyPtr->poolPtr->runPtr;
            proxyPtr->poolPtr->runPtr = proxyPtr;
            Ns_MutexUnlock(&proxyPtr->poolPtr->lock);

            if (script != NULL) {
                Ns_Log(Ns_LogNsProxyDebug, "proxy send pool %s worker %ld: %s",
                       proxyPtr->poolPtr->name, (long)proxyPtr->workerPtr->pid, script);
            }

            if (SendBuf(proxyPtr->workerPtr, &proxyPtr->conf.tsend,
                         &proxyPtr->in) == NS_FALSE) {
                err = ESend;
            }
        }
    }

    if (err != ENone) {
        Ns_TclPrintfResult(interp, "could not send script \"%s\" to proxy \"%s\": %s",
                           script == NULL ? NS_EMPTY_STRING : script,
                           proxyPtr->id, errMsg[err]);
        ProxyError(interp, err);
    }

    return err;
}


/*
 *----------------------------------------------------------------------
 *
 * Wait --
 *
 *      Wait for response from proxy process.
 *
 * Results:
 *      Proxy Err code.
 *
 * Side effects:
 *      Will format error message in given interp on failure.
 *
 *----------------------------------------------------------------------
 */

static Err
Wait(Tcl_Interp *interp, Proxy *proxyPtr, const Ns_Time *timeoutPtr)
{
    Err err = ENone;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    if (proxyPtr->state == Idle) {
        err = EIdle;
    } else if (proxyPtr->workerPtr == NULL) {
        err = EDead;
    } else if (proxyPtr->state != Done) {
        time_t ms;

        if (timeoutPtr != NULL) {
            ms = Ns_TimeToMilliseconds(timeoutPtr);
        } else {
            ms = -1;
        }
        if (ms <= 0) {
            ms = Ns_TimeToMilliseconds(&proxyPtr->conf.teval);
        }
        if (ms <= 0) {
            ms = -1;
        }
        if (WaitFd(proxyPtr->workerPtr->rfd, POLLIN, (long)ms) == 0) {
            err = EEvalTimeout;
        } else {
            proxyPtr->state = Done;
        }
    }

    if (err != ENone) {
        Ns_TclPrintfResult(interp, "could not wait for proxy \"%s\": %s",
                           proxyPtr->id, errMsg[err]);
        ProxyError(interp, err);
    }

    return err;
}


/*
 *----------------------------------------------------------------------
 *
 * Recv --
 *
 *      Receive proxy results.
 *
 * Results:
 *      Proxy Err code.
 *
 * Side effects:
 *      Will append proxy results or error message to given interp.
 *
 *----------------------------------------------------------------------
 */

static Err
Recv(Tcl_Interp *interp, Proxy *proxyPtr, int *resultPtr)
{
    Err err = ENone;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);
    NS_NONNULL_ASSERT(resultPtr != NULL);

    if (proxyPtr->state == Idle) {
        err = EIdle;
    } else if (proxyPtr->state == Busy) {
        err = ENoWait;
    } else {
        Tcl_DStringSetLength(&proxyPtr->out, 0);
        if (RecvBuf(proxyPtr->workerPtr, &proxyPtr->conf.trecv,
                    &proxyPtr->out) == NS_FALSE) {
            err = ERecv;
        } else if (Import(interp, &proxyPtr->out, resultPtr) != TCL_OK) {
            err = EImport;
        } else {
            proxyPtr->state = Idle;
        }
        ResetProxy(proxyPtr);
    }

    if (err != ENone) {
        Ns_TclPrintfResult(interp, "could not receive from proxy \"%s\": %s",
                         proxyPtr->id, errMsg[err]);
        ProxyError(interp, err);
    }

    return err;
}

/*
 *----------------------------------------------------------------------
 *
 * SendBuf --
 *
 *      Send a dstring buffer to the specified worker process.
 *
 * Results:
 *      NS_TRUE if sent, NS_FALSE on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
SendBuf(const Worker *workerPtr, const Ns_Time *timePtr, const Tcl_DString *dsPtr)
{
    ssize_t      n;
    uint32       ulen;
    struct iovec iov[2];
    Ns_Time      end;
    bool         success = NS_TRUE;

    NS_NONNULL_ASSERT(workerPtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (timePtr != NULL) {
        Ns_GetTime(&end);
        Ns_IncrTime(&end, timePtr->sec, timePtr->usec);
    }

    ulen = htonl((unsigned int)dsPtr->length);
    iov[0].iov_base = (void *)&ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = (size_t)dsPtr->length;

    while ((iov[0].iov_len + iov[1].iov_len) > 0u) {
        do {
            n = writev(workerPtr->wfd, iov, 2);
        } while (n == -1 && errno == NS_EINTR);

        if (n == -1) {
            long waitMs;

            if ((errno != EAGAIN) && (errno != NS_EWOULDBLOCK)) {
                success = NS_FALSE;
                break;

            } else if (timePtr != NULL) {
                waitMs = GetTimeDiff(&end);
                if (waitMs < 0) {
                    success = NS_FALSE;
                    break;
                }
            } else {
                waitMs = -1;
            }
            if (WaitFd(workerPtr->wfd, POLLOUT, waitMs) == 0) {
                success = NS_FALSE;
                break;
            }
        } else if (n > 0) {
            UpdateIov(iov, (size_t)n);
        }
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * RecvBuf --
 *
 *      Receive a dstring buffer.
 *
 * Results:
 *      NS_TRUE if sent, NS_FALSE on error.
 *
 * Side effects:
 *      Will resize output dstring as needed.
 *
 *----------------------------------------------------------------------
 */

static bool
RecvBuf(const Worker *workerPtr, const Ns_Time *timePtr, Tcl_DString *dsPtr)
{
    uint32       ulen = 0u;
    ssize_t      n;
    size_t       avail;
    struct iovec iov[2];
    Ns_Time      end;
    bool         success = NS_TRUE;

    NS_NONNULL_ASSERT(workerPtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (timePtr != NULL) {
        Ns_GetTime(&end);
        Ns_IncrTime(&end, timePtr->sec, timePtr->usec);
    }

    avail = (size_t)dsPtr->spaceAvl - 1u;
    iov[0].iov_base = (void *)&ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = avail;

    while (iov[0].iov_len > 0) {
        do {
            n = readv(workerPtr->rfd, iov, 2);
        } while ((n == -1) && (errno == NS_EINTR));

        if (n == 0) {
            success = NS_FALSE; /* EOF */
            break;

        } else if (n < 0) {
            long  waitMs;

            if (errno != EAGAIN && errno != NS_EWOULDBLOCK) {
                success = NS_FALSE;
                break;

            } else if (timePtr != NULL) {
                waitMs = GetTimeDiff(&end);
                if (waitMs < 0) {
                    success = NS_FALSE;
                    break;
                }
            } else {
                waitMs = -1;
            }
            if (WaitFd(workerPtr->rfd, POLLIN, waitMs) == 0) {
                success = NS_FALSE;
                break;
            }
        } else if (n > 0) {
            UpdateIov(iov, (size_t)n);
        }
    }
    if (success) {
        char    *ptr;
        ssize_t  len;

        n = (ssize_t)(avail - iov[1].iov_len);
        Tcl_DStringSetLength(dsPtr, (int)n);
        len = (ssize_t)ntohl(ulen);
        Tcl_DStringSetLength(dsPtr, (int)len);
        len -= n;
        ptr  = dsPtr->string + n;

        while (len > 0) {
            do {
                n = ns_read(workerPtr->rfd, ptr, (size_t)len);
            } while ((n == -1) && (errno == NS_EINTR));

            if (n == 0) {
                success = NS_FALSE; /* EOF */
                break;

            } else if (n < 0) {
                long waitMs;

                if (errno != EAGAIN && errno != NS_EWOULDBLOCK) {
                    success = NS_FALSE;
                    break;

                } else if (timePtr != NULL) {
                    waitMs = GetTimeDiff(&end);
                    if (waitMs < 0) {
                        success = NS_FALSE;
                        break;
                    }
                } else {
                    waitMs = -1;
                }
                if (WaitFd(workerPtr->rfd, POLLIN, waitMs) == 0) {
                    success = NS_FALSE;
                    break;
                }
            } else if (n > 0) {
                len -= n;
                ptr += n;
            }
        }
    }

    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * WaitFd --
 *
 *      Waits for the given event on the worker pipe.
 *
 * Results:
 *      1 if event received, 0 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
WaitFd(int fd, short events, long ms)
{
    struct pollfd pfd;
    int n;

    pfd.fd = fd;
    pfd.events = events | POLLPRI | POLLERR;
    pfd.revents = pfd.events;
    do {
        n = ns_poll(&pfd, 1, ms);
    } while (n == -1 && errno == NS_EINTR);
    if (n == -1) {
        n = 0;
        Ns_Log(Error, "nsproxy: poll failed: %s", strerror(errno));
    }

    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * UpdateIov --
 *
 *      Update the base and len in given iovec based on bytes
 *      already processed.
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
UpdateIov(struct iovec *iov, size_t n)
{
    NS_NONNULL_ASSERT(iov != NULL);

    if (n >= iov[0].iov_len) {
        n -= iov[0].iov_len;
        iov[0].iov_base = NULL;
        iov[0].iov_len = 0;
    } else {
        iov[0].iov_len  -= n;
        iov[0].iov_base = (char *)(iov[0].iov_base) + n;
        n = 0;
    }
    iov[1].iov_len  -= n;
    iov[1].iov_base = (char *)(iov[1].iov_base) + n;
}


/*
 *----------------------------------------------------------------------
 *
 * Export --
 *
 *      Export result of Tcl, include error, to given dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Given dstring will contain response header and data.
 *
 *----------------------------------------------------------------------
 */

static void
Export(Tcl_Interp *interp, int code, Tcl_DString *dsPtr)
{
    Res          hdr;
    const char  *einfo = NULL, *ecode = NULL, *result = NULL;
    unsigned int clen = 0u, ilen = 0u, rlen = 0u;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (interp != NULL) {
        if (code == TCL_OK) {
            einfo = NULL;
            ecode = NULL;
        } else {
            ecode = Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
            einfo = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
        }
        clen = (ecode != NULL) ? ((unsigned int)strlen(ecode) + 1) : 0u;
        ilen = (einfo != NULL) ? ((unsigned int)strlen(einfo) + 1) : 0u;
        result = Tcl_GetStringResult(interp);
        rlen = (unsigned int)strlen(result);
    }
    hdr.code = htonl((unsigned int)code);
    hdr.clen = htonl(clen);
    hdr.ilen = htonl(ilen);
    hdr.rlen = htonl(rlen);
    Tcl_DStringAppend(dsPtr, (char *) &hdr, sizeof(hdr));
    if (clen > 0) {
        Tcl_DStringAppend(dsPtr, ecode, (int)clen);
    }
    if (ilen > 0) {
        Tcl_DStringAppend(dsPtr, einfo, (int)ilen);
    }
    if (rlen > 0) {
        Tcl_DStringAppend(dsPtr, result, (int)rlen);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Import --
 *
 *      Import result of Tcl to given interp.
 *
 * Results:
 *      Tcl result code from remote worker process.
 *
 * Side effects:
 *      Will set interp result and error data as needed.
 *
 *----------------------------------------------------------------------
 */

static int
Import(Tcl_Interp *interp, const Tcl_DString *dsPtr, int *resultPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(resultPtr != NULL);

    if (dsPtr->length < (int)sizeof(Res)) {
        result = TCL_ERROR;

    } else {
        Res         res, *resPtr = &res;
        const char *str    = dsPtr->string + sizeof(Res);
        size_t      rlen, clen, ilen;

        memcpy(&res, dsPtr->string, sizeof(Res));

        clen = ntohl(resPtr->clen);
        ilen = ntohl(resPtr->ilen);
        rlen = ntohl(resPtr->rlen);
        if (clen > 0) {
            Tcl_Obj *err = Tcl_NewStringObj(str, -1);

            Tcl_SetObjErrorCode(interp, err);
            str += clen;
        }
        if (ilen > 0) {
            Tcl_AddErrorInfo(interp, str);
            str += ilen;
        }
        if (rlen > 0) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(str, -1));
        }
        *resultPtr = (int)ntohl(resPtr->code);
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * StatsObjCmd --
 *
 *    Implements the "ns_proxy stats" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
static int
StatsObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *pool;
    Ns_ObjvSpec args[] = {
        {"pool",    Ns_ObjvString, &pool, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString ds, *dsPtr = &ds;
        Pool       *poolPtr = GetPool(pool, clientData);
        int         processes = 0;
        Proxy      *proxyPtr;

        Tcl_DStringInit(dsPtr);
        Ns_MutexLock(&plock);
        Ns_MutexLock(&poolPtr->lock);

        for (proxyPtr = poolPtr->firstPtr; proxyPtr != NULL; proxyPtr = proxyPtr->nextPtr) {
            if (proxyPtr->workerPtr != NULL) {
                processes ++;
            }
        }
        Ns_DStringPrintf(dsPtr, "proxies %" PRIuPTR, poolPtr->nextid);
        Ns_DStringPrintf(dsPtr, " waiting %d", poolPtr->waiting);
        Ns_DStringPrintf(dsPtr, " maxworkers %d", poolPtr->maxworker);
        Ns_DStringPrintf(dsPtr, " free %d", poolPtr->nfree);
        Ns_DStringPrintf(dsPtr, " used %d", poolPtr->nused);
        Ns_DStringPrintf(dsPtr, " requests %" PRIuPTR, poolPtr->nruns);
        Ns_DStringPrintf(dsPtr, " processes %d", processes);
        Tcl_DStringAppend(dsPtr, " runtime ", 9);
        Ns_DStringAppendTime(dsPtr, &poolPtr->runTime);

        Ns_MutexUnlock(&poolPtr->lock);
        Ns_MutexUnlock(&plock);

        Tcl_DStringResult(interp, dsPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * StopObjCmd --
 *
 *    Implements the "ns_proxy stop" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */
static int
StopObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *pool, *handle = NULL;
    Ns_ObjvSpec args[] = {
        {"pool",    Ns_ObjvString, &pool, NULL},
        {"?handle", Ns_ObjvString, &handle, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_HashEntry *hPtr;
        Tcl_HashSearch search;
        Pool          *thePoolPtr = GetPool(pool, clientData);
        int            reap = 0;

        Ns_MutexLock(&plock);
        for (hPtr = Tcl_FirstHashEntry(&pools, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
            Pool *poolPtr = (Pool *)Tcl_GetHashValue(hPtr);

            if (thePoolPtr == poolPtr) {
                Proxy *proxyPtr;

                Ns_MutexLock(&poolPtr->lock);
                for (proxyPtr = poolPtr->runPtr; proxyPtr != NULL; proxyPtr = proxyPtr->runPtr) {
                    reap += CloseWorkerOfProxy(proxyPtr, handle, &proxyPtr->conf.twait);
                }
                Ns_MutexUnlock(&poolPtr->lock);
                break;
            }
        }
        Ns_MutexUnlock(&plock);
        if (reap != 0) {
            ReapProxies();
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * StopObjCmd --
 *
 *    Implements the "ns_proxy clear" command.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

static int
ClearObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *pool, *handle = NULL;
    Ns_ObjvSpec args[] = {
        {"pool",    Ns_ObjvString, &pool, NULL},
        {"?handle", Ns_ObjvString, &handle, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_HashEntry *hPtr;
        Tcl_HashSearch search;
        Pool          *thePoolPtr = GetPool(pool, clientData);
        int            reap = 0;

        Ns_MutexLock(&plock);
        for (hPtr = Tcl_FirstHashEntry(&pools, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
            Pool *poolPtr = (Pool *)Tcl_GetHashValue(hPtr);

            if (thePoolPtr == poolPtr) {
                Proxy *proxyPtr;

                Ns_MutexLock(&poolPtr->lock);
                for (proxyPtr = poolPtr->firstPtr; proxyPtr != NULL; proxyPtr = proxyPtr->nextPtr) {
                    reap += CloseWorkerOfProxy(proxyPtr, handle, &proxyPtr->conf.twait);
                }
                Ns_MutexUnlock(&poolPtr->lock);
                break;
            }
        }
        Ns_MutexUnlock(&plock);
        if (reap != 0) {
            ReapProxies();
        }
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * ProxyObjCmd --
 *
 *      Implement the ns_proxy command.
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
ProxyObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    InterpData    *idataPtr = data;
    Pool          *poolPtr;
    Proxy         *proxyPtr;
    Err            err;
    int            opt, result = TCL_OK;
    const char    *proxyId;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Tcl_Obj       *listObj;

    static const char *opts[] = {
        "active", "cleanup", "clear", "configure", "eval",
        "free", "get", "handles", "ping", "pools", "put",
        "recv", "release", "send", "stats", "stop", "wait",
        NULL
    };
    enum {
        PActiveIdx, PCleanupIdx, PClearIdx, PConfigureIdx, PEvalIdx,
        PFreeIdx, PGetIdx, PHandlesIdx, PPingIdx, PPoolsIdx, PPutIdx,
        PRecvIdx, PReleaseIdx, PSendIdx, PStatsIdx, PStopIdx, PWaitIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?args?");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case PReleaseIdx:   NS_FALL_THROUGH; /* fall through */
    case PPutIdx:       NS_FALL_THROUGH; /* fall through */
    case PPingIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle");
            result = TCL_ERROR;
        } else {
            proxyId  = Tcl_GetString(objv[2]);
            proxyPtr = GetProxy(proxyId, idataPtr);
            if (proxyPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such handle: %s", proxyId);
                result = TCL_ERROR;
            } else if (opt == PPutIdx || opt == PReleaseIdx) {
                result = ReleaseProxy(interp, proxyPtr);
            } else /* opt == PPingIdx */ {
                result = Eval(interp, proxyPtr, NULL, NULL);
            }
        }
        break;

    case PConfigureIdx:
        result = ConfigureObjCmd(data, interp, objc, objv);
        break;

    case PCleanupIdx:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            result = TCL_ERROR;
        } else {
            ReleaseHandles(interp, idataPtr);
        }
        break;

    case PGetIdx:
        result = GetObjCmd(data, interp, objc, objv);
        break;

    case PSendIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle script");
            result = TCL_ERROR;
        } else {
            proxyId = Tcl_GetString(objv[2]);
            proxyPtr = GetProxy(proxyId, idataPtr);
            if (proxyPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such handle: %s", proxyId);
                result = TCL_ERROR;
            } else {
                err = Send(interp, proxyPtr, Tcl_GetString(objv[3]));
                result = (err == ENone) ? TCL_OK : TCL_ERROR;
            }
        }
        break;

    case PWaitIdx:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle ?timeout?");
            result = TCL_ERROR;
        } else {
            Ns_Time *timeoutPtr = NULL;

            proxyId = Tcl_GetString(objv[2]);
            proxyPtr = GetProxy(proxyId, idataPtr);
            if (proxyPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such handle: %s", proxyId);
                result = TCL_ERROR;
            } else if (objc > 3 && Ns_TclGetTimePtrFromObj(interp, objv[3], &timeoutPtr) != TCL_OK) {
                result = TCL_ERROR;
            }
            if (result == TCL_OK) {
                err = Wait(interp, proxyPtr, timeoutPtr);
                result = (err == ENone) ? TCL_OK : TCL_ERROR;
            }
        }
        break;

    case PRecvIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle");
            result = TCL_ERROR;
        } else {
            proxyId = Tcl_GetString(objv[2]);
            proxyPtr = GetProxy(proxyId, idataPtr);
            if (proxyPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such handle: %s", proxyId);
                result = TCL_ERROR;
            } else {
                err = Recv(interp, proxyPtr, &result);
                result = (err == ENone) ? result : TCL_ERROR;
                Ns_Log(Debug, "Receive calls GetStats");

                GetStats(proxyPtr);
            }
        }
        break;

    case PEvalIdx:
        if (objc != 4 && objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle script");
            result = TCL_ERROR;
        } else {
            Ns_Time *timeoutPtr = NULL;

            proxyId = Tcl_GetString(objv[2]);
            proxyPtr = GetProxy(proxyId, idataPtr);
            if (proxyPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such handle: %s", proxyId);
                result = TCL_ERROR;
            } else if (objc > 4 && Ns_TclGetTimePtrFromObj(interp, objv[4], &timeoutPtr) != TCL_OK) {
                    result = TCL_ERROR;
            }
            if (result == TCL_OK) {
                result = Eval(interp, proxyPtr, Tcl_GetString(objv[3]), timeoutPtr);
            }
        }
        break;

    case PFreeIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool");
            result = TCL_ERROR;
        } else {
            listObj = Tcl_NewListObj(0, NULL);
            poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
            Ns_MutexLock(&poolPtr->lock);
            proxyPtr = poolPtr->firstPtr;
            while (proxyPtr != NULL) {
                Tcl_ListObjAppendElement(interp, listObj, StringObj(proxyPtr->id));
                proxyPtr = proxyPtr->nextPtr;
            }
            Ns_MutexUnlock(&poolPtr->lock);
            Tcl_SetObjResult(interp, listObj);
        }
        break;

    case PHandlesIdx:
        if (objc == 3) {
            poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
        } else {
            poolPtr = NULL;
        }
        listObj = Tcl_NewListObj(0, NULL);
        hPtr = Tcl_FirstHashEntry(&idataPtr->ids, &search);
        while (hPtr != NULL) {
            proxyPtr = (Proxy *)Tcl_GetHashValue(hPtr);
            if (poolPtr == NULL || poolPtr == proxyPtr->poolPtr) {
                Tcl_ListObjAppendElement(interp, listObj, StringObj(proxyPtr->id));
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_SetObjResult(interp, listObj);
        break;

    case PActiveIdx:
        if (objc < 3 || objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool ?handle?");
            result = TCL_ERROR;
        } else {
            poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
            proxyId = (objc >= 4) ? Tcl_GetString(objv[3]) : NULL;
            Ns_MutexLock(&plock);
            Ns_MutexLock(&poolPtr->lock);
            proxyPtr = poolPtr->runPtr;
            while (proxyPtr != NULL) {
                if (proxyId == NULL || STREQ(proxyId, proxyPtr->id)) {
                    FmtActiveProxy(interp, proxyPtr);
                }
                proxyPtr = proxyPtr->runPtr;
            }
            Ns_MutexUnlock(&poolPtr->lock);
            Ns_MutexUnlock(&plock);
        }
        break;

    case PStopIdx:
        result = StopObjCmd(data, interp, objc, objv);
        break;

    case PClearIdx:
        result = ClearObjCmd(data, interp, objc, objv);
        break;

    case PPoolsIdx:
        listObj = Tcl_NewListObj(0, NULL);
        Ns_MutexLock(&plock);
        hPtr = Tcl_FirstHashEntry(&pools, &search);
        while (hPtr != NULL) {
            poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
            Tcl_ListObjAppendElement(interp, listObj,  StringObj(poolPtr->name));
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&plock);
        Tcl_SetObjResult(interp, listObj);
        break;

    case PStatsIdx:
        result = StatsObjCmd(data, interp, objc, objv);
        break;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ConfigureObjCmd --
 *
 *      Sub-command to configure a proxy.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Will update one or more config options.
 *
 *----------------------------------------------------------------------
 */

static int
ConfigureObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    InterpData *idataPtr = data;
    Pool       *poolPtr;
    Proxy      *proxyPtr;
    int         flag = 0, n, result = TCL_OK, reap = 0;

    static const char *flags[] = {
        "-init", "-reinit", "-maxslaves", "-exec", "-env",
        "-gettimeout", "-evaltimeout", "-sendtimeout", "-recvtimeout",
        "-waittimeout", "-idletimeout", "-logminduration", "-maxruns",
        "-maxworkers",  NULL
    };
    enum {
        CInitIdx, CReinitIdx, CMaxslaveIdx, CExecIdx, CEnvIdx,
        CGetIdx, CEvalIdx, CSendIdx, CRecvIdx,
        CWaitIdx, CIdleIdx, CLogmindurationIdx, CMaxrunsIdx,
        CMaxworkerIdx
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "pool ?opt? ?val? ?opt val?...");
        return TCL_ERROR;
    }

    poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
    Ns_MutexLock(&poolPtr->lock);
    if (objc == 4) {
        if (Tcl_GetIndexFromObj(interp, objv[3], flags, "flags", 0,
                                &flag) != TCL_OK) {
            result = TCL_ERROR;
            goto err;
        }
    } else if (objc > 4) {
        int   i;
        const char *str;

        for (i = 3; i < (objc - 1); ++i) {
            if (Tcl_GetIndexFromObj(interp, objv[i], flags, "flags", 0,
                                    &flag)) {
                result = TCL_ERROR;
                goto err;
            }
            ++i;
            str = Tcl_GetString(objv[i]);
            switch (flag) {
            case CEvalIdx:           NS_FALL_THROUGH; /* fall through */
            case CGetIdx:            NS_FALL_THROUGH; /* fall through */
            case CIdleIdx:           NS_FALL_THROUGH; /* fall through */
            case CLogmindurationIdx: NS_FALL_THROUGH; /* fall through */
            case CRecvIdx:           NS_FALL_THROUGH; /* fall through */
            case CSendIdx:           NS_FALL_THROUGH; /* fall through */
            case CWaitIdx: {
                Ns_Time timeout;

                if (Ns_TclGetTimeFromObj(interp, objv[i], &timeout) != TCL_OK) {
                    result = TCL_ERROR;
                    goto err;
                }
                switch (flag) {
                case CRecvIdx:
                    poolPtr->conf.trecv = timeout;
                    break;
                case CSendIdx:
                    poolPtr->conf.tsend = timeout;
                    break;
                case CEvalIdx:
                    poolPtr->conf.teval = timeout;
                    break;
                case CWaitIdx:
                    poolPtr->conf.twait = timeout;
                    break;
                case CGetIdx:
                    poolPtr->conf.tget = timeout;
                    break;
                case CLogmindurationIdx:
                    poolPtr->conf.logminduration = timeout;
                    break;
                case CIdleIdx:
                    {
                        Ns_Time minIdle = { MIN_IDLE_TIMEOUT_SEC, 0 };

                        poolPtr->conf.tidle = timeout;
                        if (Ns_DiffTime(&poolPtr->conf.tidle, &minIdle, NULL) == -1) {
                            poolPtr->conf.tidle = minIdle;
                        }
                        proxyPtr = poolPtr->firstPtr;
                        while (proxyPtr != NULL) {
                            if (proxyPtr->workerPtr != NULL) {
                                SetExpire(proxyPtr->workerPtr, &proxyPtr->conf.tidle);
                            }
                            proxyPtr = proxyPtr->nextPtr;
                        }
                        reap = 1;
                        break;
                    }
                }
                break;
            }

            case CMaxslaveIdx: NS_FALL_THROUGH; /* fall through */
            case CMaxworkerIdx: NS_FALL_THROUGH; /* fall through */
            case CMaxrunsIdx:
                if (Tcl_GetIntFromObj(interp, objv[i], &n) != TCL_OK) {
                    result = TCL_ERROR;
                    goto err;
                }
                if (n < 0) {
                    Ns_TclPrintfResult(interp, "invalid %s: %s",
                                       flags[flag], str);
                    result = TCL_ERROR;
                    goto err;
                }
                switch (flag) {
                case CMaxslaveIdx:
                case CMaxworkerIdx:
                    poolPtr->maxworker = n;
                    reap = 1;
                    break;
                case CMaxrunsIdx:
                    poolPtr->conf.maxruns = n;
                    break;
                }
                break;
            case CInitIdx:
                SetOpt(str, &poolPtr->init);
                break;
            case CReinitIdx:
                SetOpt(str, &poolPtr->reinit);
                break;
            case CExecIdx:
                SetOpt(str, &poolPtr->exec);
                break;
            case CEnvIdx:
                if (poolPtr->env) {
                    Ns_SetFree(poolPtr->env);
                }
                poolPtr->env = Ns_SetCopy(Ns_TclGetSet(interp, str));
                break;
            }
        }

        /*
         * Assure number of idle and used proxies always
         * match the maximum number of configured ones.
         */

        while ((poolPtr->nfree + poolPtr->nused) < poolPtr->maxworker) {
            proxyPtr = CreateProxy(poolPtr);
            proxyPtr->nextPtr = poolPtr->firstPtr;
            poolPtr->firstPtr = proxyPtr;
            poolPtr->nfree++;
        }
    }

    /*
     * Construct command result
     */
    Tcl_ResetResult(interp);

    if (objc == 3) {
        Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

        Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(flags[CEnvIdx], -1));
        if (poolPtr->env != NULL) {
            if (unlikely(Ns_TclEnterSet(interp, poolPtr->env, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
                result = TCL_ERROR;
            } else {
                /*
                 * Ns_TclEnterSet() sets the result
                 */
            }
        }

        if (result == TCL_OK) {
            Tcl_ListObjAppendElement(interp, listObj, Tcl_GetObjResult(interp));
            AppendObj(listObj, flags[CExecIdx],     StringObj(poolPtr->exec));
            AppendObj(listObj, flags[CInitIdx],     StringObj(poolPtr->init));
            AppendObj(listObj, flags[CReinitIdx],   StringObj(poolPtr->reinit));
            AppendObj(listObj, flags[CMaxslaveIdx], Tcl_NewIntObj(poolPtr->maxworker));
            AppendObj(listObj, flags[CMaxrunsIdx],  Tcl_NewIntObj(poolPtr->conf.maxruns));
            AppendObj(listObj, flags[CGetIdx],      Ns_TclNewTimeObj(&poolPtr->conf.tget));
            AppendObj(listObj, flags[CEvalIdx],     Ns_TclNewTimeObj(&poolPtr->conf.teval));
            AppendObj(listObj, flags[CSendIdx],     Ns_TclNewTimeObj(&poolPtr->conf.tsend));
            AppendObj(listObj, flags[CRecvIdx],     Ns_TclNewTimeObj(&poolPtr->conf.trecv));
            AppendObj(listObj, flags[CWaitIdx],     Ns_TclNewTimeObj(&poolPtr->conf.twait));
            AppendObj(listObj, flags[CIdleIdx],     Ns_TclNewTimeObj(&poolPtr->conf.tidle));
            AppendObj(listObj, flags[CLogmindurationIdx], Ns_TclNewTimeObj(&poolPtr->conf.logminduration));
            Tcl_SetObjResult(interp, listObj);
        }

    } else if (objc == 4) {
        switch (flag) {
        case CExecIdx:     Tcl_SetObjResult(interp, StringObj(poolPtr->exec));
            break;
        case CInitIdx:     Tcl_SetObjResult(interp, StringObj(poolPtr->init));
            break;
        case CReinitIdx:   Tcl_SetObjResult(interp, StringObj(poolPtr->reinit));
            break;
        case CMaxslaveIdx: Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->maxworker));
            break;
        case CMaxrunsIdx:  Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.maxruns));
            break;
        case CGetIdx:      Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&poolPtr->conf.tget));
            break;
        case CEvalIdx:     Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&poolPtr->conf.teval));
            break;
        case CSendIdx:     Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&poolPtr->conf.tsend));
            break;
        case CRecvIdx:     Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&poolPtr->conf.trecv));
            break;
        case CWaitIdx:     Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&poolPtr->conf.twait));
            break;
        case CIdleIdx:     Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&poolPtr->conf.tidle));
            break;
        case CLogmindurationIdx: Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&poolPtr->conf.logminduration));
            break;
        case CEnvIdx:
            if (poolPtr->env) {
                /*
                 * Ns_TclEnterSet() sets the result
                 */
                if (unlikely(Ns_TclEnterSet(interp, poolPtr->env, NS_TCL_SET_DYNAMIC) != TCL_OK)) {
                    result = TCL_ERROR;
                }
            } else {
                /*
                 * The result is empty.
                 */
            }
            break;
        }
    } else if (objc == 5) {
        Tcl_SetObjResult(interp, objv[4]);
    }

 err:
    Ns_MutexUnlock(&poolPtr->lock);

    /*
     * Optionally, wake up reaper thread
     * to collect closing proxies or to
     * enforce pool size constraints.
     */

    if (reap != 0) {
        ReapProxies();
    }

    return result;
}


static void
SetOpt(const char *str, char const **optPtr)
{
    NS_NONNULL_ASSERT(str != NULL);
    NS_NONNULL_ASSERT(optPtr != NULL);

    if (*optPtr != NULL) {
        ns_free((char*)*optPtr);
    }
    if (*str != '\0') {
        *optPtr = ns_strdup(str);
    } else {
        *optPtr = NULL;
    }
}

static Tcl_Obj*
StringObj(const char* chars) {
    Tcl_Obj *resultObj;

    if (chars != NULL) {
        resultObj = Tcl_NewStringObj(chars, -1);
    } else {
        resultObj = Tcl_NewStringObj("", 0);
    }
    return resultObj;
}

static void
AppendObj(Tcl_Obj *listObj, const char *flag, Tcl_Obj *obj)
{
    NS_NONNULL_ASSERT(listObj != NULL);
    NS_NONNULL_ASSERT(flag != NULL);

    Tcl_ListObjAppendElement(NULL, listObj, StringObj(flag));
    Tcl_ListObjAppendElement(NULL, listObj, obj);
}


/*
 *----------------------------------------------------------------------
 *
 * GetObjCmd --
 *
 *      Sub-command to handle ns_proxy get option.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      May allocate one or more handles.
 *
 *----------------------------------------------------------------------
 */

static int
GetObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    InterpData    *idataPtr = data;
    Proxy         *proxyPtr, *firstPtr;
    Tcl_HashEntry *cntPtr, *idPtr;
    int            isNew, nwant = 1, result = TCL_OK;
    Ns_Time        *timeoutPtr = NULL;
    Err            err;
    Pool          *poolPtr;
    Ns_ObjvSpec    lopts[] = {
        {"-timeout", Ns_ObjvTime, &timeoutPtr, NULL},
        {"-handles", Ns_ObjvInt,  &nwant,      NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (objc < 3 || (objc % 2) != 1) {
        Tcl_WrongNumArgs(interp, 2, objv, "pool ?-opt val -opt val ...?");
        return TCL_ERROR;
    }

    assert(idataPtr != NULL);
    poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
    assert(poolPtr != NULL);

    cntPtr = Tcl_CreateHashEntry(&idataPtr->cnts, (char *) poolPtr, &isNew);
    if ((intptr_t) Tcl_GetHashValue(cntPtr) > 0) {
        err = EDeadlock;
        goto errout;
    }

    if (Ns_ParseObjv(lopts, NULL, interp, 3, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    if (timeoutPtr == NULL) {
        Ns_MutexLock(&poolPtr->lock);
        timeoutPtr = &poolPtr->conf.tget;
        Ns_MutexUnlock(&poolPtr->lock);
    }

    /*
     * Get some number of proxies from the pool
     */

    err = PopProxy(poolPtr, &firstPtr, nwant, timeoutPtr);
    if (err != 0) {
    errout:
        Ns_TclPrintfResult(interp, "could not allocate from pool \"%s\": %s",
                           poolPtr->name, errMsg[err]);
        ProxyError(interp, err);
        return TCL_ERROR;
    }

    /*
     * Set total owned count and create handle ids.
     */

    Tcl_SetHashValue(cntPtr, INT2PTR(nwant));
    proxyPtr = firstPtr;
    while (proxyPtr != NULL) {
        idPtr = Tcl_CreateHashEntry(&idataPtr->ids, proxyPtr->id, &isNew);
        if (isNew == 0) {
            Ns_Fatal("nsproxy: duplicate proxy entry");
        }
        Tcl_SetHashValue(idPtr, proxyPtr);
        proxyPtr->cntPtr = cntPtr;
        proxyPtr->idPtr  = idPtr;
        proxyPtr = proxyPtr->nextPtr;
    }

    /*
     * Check each proxy for valid connection.
     */

    err = ENone;
    proxyPtr = firstPtr;
    while (err == ENone && proxyPtr != NULL) {
        err = CheckProxy(interp, proxyPtr);
        proxyPtr = proxyPtr->nextPtr;
    }
    if (err != ENone) {
        while ((proxyPtr = firstPtr) != NULL) {
            firstPtr = proxyPtr->nextPtr;
            PushProxy(proxyPtr);
        }
        result = TCL_ERROR;
    }

    if (result == TCL_OK) {
        /*
         * Generate accessor commands for the returned proxies.
         */
        Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

        proxyPtr = firstPtr;
        while (proxyPtr != NULL) {
            proxyPtr->cmdToken = Tcl_CreateObjCommand(interp, proxyPtr->id,
                                                      RunProxyCmd, proxyPtr,
                                                      DelProxyCmd);
            if (proxyPtr->cmdToken == NULL) {
                result = TCL_ERROR;
                break;
            }
            proxyPtr->interp = interp;
            Tcl_ListObjAppendElement(interp, listObj,  StringObj(proxyPtr->id));
            proxyPtr = proxyPtr->nextPtr;
        }

        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, listObj);
        } else {
            Tcl_DecrRefCount(listObj);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * PopProxy --
 *
 *      Pops number of free proxies from the pool.
 *
 * Results:
 *      Error message or NULL if all went fine..
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Err
PopProxy(Pool *poolPtr, Proxy **proxyPtrPtr, int nwant, const Ns_Time *timePtr)
{
    Proxy        *proxyPtr;
    Err           err;
    Ns_ReturnCode status = NS_OK;
    Ns_Time       waitTimeout;

    NS_NONNULL_ASSERT(poolPtr != NULL);
    NS_NONNULL_ASSERT(proxyPtrPtr != NULL);

    if (timePtr != NULL) {
        Ns_GetTime(&waitTimeout);
        Ns_IncrTime(&waitTimeout, timePtr->sec, timePtr->usec);
    }

    Ns_MutexLock(&poolPtr->lock);
    while (status == NS_OK && poolPtr->waiting > 0) {
        if (timePtr != NULL) {
            status = Ns_CondTimedWait(&poolPtr->cond, &poolPtr->lock, &waitTimeout);
        } else {
            Ns_CondWait(&poolPtr->cond, &poolPtr->lock);
        }
    }
    if (status != NS_OK) {
        err = EGetTimeout;
    } else {
        poolPtr->waiting = 1;
        while (status == NS_OK
               && poolPtr->nfree < nwant && poolPtr->maxworker >= nwant) {
            if (timePtr != NULL) {
                status = Ns_CondTimedWait(&poolPtr->cond, &poolPtr->lock,
                                          &waitTimeout);
            } else {
                Ns_CondWait(&poolPtr->cond, &poolPtr->lock);
            }
        }
        if (status != NS_OK) {
            err = EGetTimeout;
        } else if (poolPtr->maxworker == 0 || poolPtr->maxworker < nwant) {
            err = ERange;
        } else {
            int i;

            poolPtr->nfree -= nwant;
            poolPtr->nused += nwant;

            for (i = 0, *proxyPtrPtr = NULL; i < nwant; ++i) {
                proxyPtr = poolPtr->firstPtr;
                poolPtr->firstPtr = proxyPtr->nextPtr;
                proxyPtr->nextPtr = *proxyPtrPtr;
                *proxyPtrPtr = proxyPtr;
                proxyPtr->conf = poolPtr->conf;
            }
            err = ENone;
        }
        poolPtr->waiting = 0;
        Ns_CondBroadcast(&poolPtr->cond);
    }
    Ns_MutexUnlock(&poolPtr->lock);

    return err;
}


/*
 *----------------------------------------------------------------------
 *
 * FmtActiveProxy --
 *
 *      Fills in the interp result with list of proxy values..
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
FmtActiveProxy(Tcl_Interp *interp, const Proxy *proxyPtr)
{
    Tcl_DString ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    Tcl_DStringInit(&ds);
    Tcl_DStringGetResult(interp, &ds);

    Tcl_DStringStartSublist(&ds);
    Ns_DStringPrintf(&ds, "handle %s slave %ld start %" PRId64 ".%06ld script",
                     proxyPtr->id,
                     (long) ((proxyPtr->workerPtr != NULL) ? proxyPtr->workerPtr->pid : 0),
                     (int64_t) proxyPtr->when.sec,
                     proxyPtr->when.usec);

    Tcl_DStringAppendElement(&ds, Tcl_DStringValue(&proxyPtr->in) + sizeof(Req));
    Tcl_DStringEndSublist(&ds);

    Tcl_DStringResult(interp, &ds);
}


/*
 *----------------------------------------------------------------------
 *
 * GetPool --
 *
 *      Get a pool by name.
 *
 * Results:
 *      Pool pointer.
 *
 * Side effects:
 *      Will update given poolPtrPtr with pointer to Pool.
 *
 *----------------------------------------------------------------------
 */

static Pool*
GetPool(const char *poolName, const InterpData *idataPtr)
{
    Tcl_HashEntry *hPtr;
    Pool          *poolPtr;
    Proxy         *proxyPtr;
    int            isNew;

    NS_NONNULL_ASSERT(poolName != NULL);

    Ns_MutexLock(&plock);
    hPtr = Tcl_CreateHashEntry(&pools, poolName, &isNew);
    if (isNew == 0) {
        poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
    } else {
        const char *path = "", *exec;
        int i;

        poolPtr = ns_calloc(1u, sizeof(Pool));
        Tcl_SetHashValue(hPtr, poolPtr);
        poolPtr->name = Tcl_GetHashKey(&pools, hPtr);
        if (idataPtr != NULL && idataPtr->server != NULL && idataPtr->module != NULL) {
          path = Ns_ConfigGetPath(idataPtr->server, idataPtr->module, (char *)0L);
        }
        if (*path != '\0' && (exec = Ns_ConfigGetValue(path, "exec")) != NULL) {
            SetOpt(exec, &poolPtr->exec);
        } else {
            SetOpt(Tcl_DStringValue(&defexec), &poolPtr->exec);
        }
        Ns_ConfigTimeUnitRange(path, "gettimeout",
                               "0ms", 0, 0, INT_MAX, 0,
                               &poolPtr->conf.tget);

        Ns_ConfigTimeUnitRange(path, "evaltimeout",
                               "0ms", 0, 0, INT_MAX, 0,
                               &poolPtr->conf.teval);

        Ns_ConfigTimeUnitRange(path, "sendtimeout",
                               "5s", 0, 0, INT_MAX, 0,
                               &poolPtr->conf.tsend);

        Ns_ConfigTimeUnitRange(path, "recvtimeout",
                               "5s", 0, 0, INT_MAX, 0,
                               &poolPtr->conf.trecv);

        Ns_ConfigTimeUnitRange(path, "waittimeout",
                               "1s", 0, 0, INT_MAX, 0,
                               &poolPtr->conf.twait);

        Ns_ConfigTimeUnitRange(path, "idletimeout",
                               "5m", MIN_IDLE_TIMEOUT_SEC, 0, INT_MAX, 0,
                               &poolPtr->conf.tidle);

        {
            int max = Ns_ConfigInt(path, "maxworker", -1);
            if (max == -1) {
                max = Ns_ConfigInt(path, "maxslaves", -1);
            }
            if (max == -1) {
                max = 8;
            }
            poolPtr->maxworker  = max;
        }

        Ns_ConfigTimeUnitRange(path, "logminduration",
                               "1s", 0, 0, INT_MAX, 0,
                               &poolPtr->conf.logminduration);

        for (i = 0; i < poolPtr->maxworker; i++) {
            proxyPtr = CreateProxy(poolPtr);
            proxyPtr->nextPtr = poolPtr->firstPtr;
            poolPtr->firstPtr = proxyPtr;
            poolPtr->nfree++;
        }
        Ns_CondInit(&poolPtr->cond);
        Ns_MutexInit(&poolPtr->lock);
        Ns_MutexSetName2(&poolPtr->lock, "nsproxy", poolName);
    }
    Ns_MutexUnlock(&plock);

    return poolPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateProxy --
 *
 *      Create new proxy handle.
 *
 * Results:
 *      Proxy handle.
 *
 * Side effects:
 *      Assumes pool lock is held.
 *
 *----------------------------------------------------------------------
 */

static Proxy*
CreateProxy(Pool *poolPtr)
{
    Proxy *proxyPtr;
    char buf[TCL_INTEGER_SPACE];
    size_t nameLength;
    int idLength;

    NS_NONNULL_ASSERT(poolPtr != NULL);

    idLength = snprintf(buf, sizeof(buf), "%" PRIuPTR, poolPtr->nextid++);
    nameLength = strlen(poolPtr->name);

    proxyPtr = ns_calloc(1u, sizeof(Proxy));
    proxyPtr->poolPtr = poolPtr;

    proxyPtr->id = ns_calloc(1u, strlen(buf) + nameLength + 2u);
    memcpy(proxyPtr->id, poolPtr->name, nameLength);
    *(proxyPtr->id + nameLength) = '-';
    memcpy(proxyPtr->id + nameLength + 1u, buf, (size_t)idLength + 1u);

    Tcl_DStringInit(&proxyPtr->in);
    Tcl_DStringInit(&proxyPtr->out);

    return proxyPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * GetProxy --
 *
 *      Get a previously allocated proxy handle.
 *
 * Results:
 *      Pointer to the proxy.
 *
 * Side effects:
 *      Imposes maxruns limit.
 *
 *----------------------------------------------------------------------
 */

static Proxy*
GetProxy(const char *proxyId, InterpData *idataPtr)
{
    const Tcl_HashEntry *hPtr;
    Proxy                *result = NULL;

    hPtr = Tcl_FindHashEntry(&idataPtr->ids, proxyId);
    if (likely(hPtr != NULL)) {
        result = (Proxy *)Tcl_GetHashValue(hPtr);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * CheckProxy --
 *
 *      Check a proxy, pinging the proc and creating a new worker processes as
 *      needed.
 *
 * Results:
 *      ENone if proxy OK, other error if worker process could not be created.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Err
CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Err err = ENone;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    if ((proxyPtr->workerPtr != NULL)
        && (Eval(interp, proxyPtr, NULL, NULL) != TCL_OK)
        ) {
        CloseProxy(proxyPtr);
        Tcl_ResetResult(interp);
    }
    if (proxyPtr->workerPtr == NULL) {
        err = CreateWorker(interp, proxyPtr);
    }

    return err;
}


/*
 *----------------------------------------------------------------------
 *
 * CreateWorker --
 *
 *      Create new proxy worker process
 *
 * Results:
 *      ENone if proxy OK, other error if worker could not be created.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */

static Err
CreateWorker(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Pool        *poolPtr;
    Err          err;
    int          init;
    Tcl_DString  ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    poolPtr = proxyPtr->poolPtr;

    Tcl_DStringInit(&ds);
    Ns_MutexLock(&poolPtr->lock);
    init = proxyPtr->poolPtr->init != NULL;
    if (init != 0) {
        Tcl_DStringAppend(&ds, poolPtr->init, -1);
    }
    Ns_MutexUnlock(&poolPtr->lock);
    proxyPtr->workerPtr = ExecWorker(interp, proxyPtr);
    if (proxyPtr->workerPtr == NULL) {
        err = EExec;
    } else if (init != 0
               && (Eval(interp, proxyPtr, Tcl_DStringValue(&ds), NULL) != TCL_OK)
               ) {
        CloseProxy(proxyPtr);
        err = EInit;
    } else if (Eval(interp, proxyPtr, NULL, NULL) != TCL_OK) {
        CloseProxy(proxyPtr);
        err = EInit;
    } else {
        err = ENone;
        Tcl_ResetResult(interp);
    }
    Tcl_DStringFree(&ds);
    if (err != EExec) {
        ReapProxies();
    }

    return err;
}


/*
 *----------------------------------------------------------------------
 *
 * ResetProxy --
 *
 *      Reset a proxy preparing it for the next request.
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
ResetProxy(Proxy *proxyPtr)
{
    Pool  *poolPtr;
    Proxy *runPtr, *prevPtr;

    NS_NONNULL_ASSERT(proxyPtr);

    poolPtr = proxyPtr->poolPtr;
    /*
     * Non-idle proxies will be closed forcefully
     */

    if (proxyPtr->state != Idle) {
        CloseProxy(proxyPtr);
        proxyPtr->state = Idle;
    }

    /*
     * Splice out of the run queue
     */

    Ns_MutexLock(&poolPtr->lock);
    runPtr = prevPtr = poolPtr->runPtr;
    while (runPtr != NULL && runPtr != proxyPtr) {
        prevPtr = runPtr;
        runPtr  = runPtr->runPtr;
    }
    if (runPtr != NULL) {
        if (runPtr == poolPtr->runPtr) {
            poolPtr->runPtr = runPtr->runPtr;
        } else {
            prevPtr->runPtr = runPtr->runPtr;
        }
    } else if (prevPtr != NULL) {
        prevPtr->runPtr = NULL;
    }
    Ns_MutexUnlock(&poolPtr->lock);

    Tcl_DStringSetLength(&proxyPtr->in, 0);
    Tcl_DStringSetLength(&proxyPtr->out, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * CloseWorker --
 *
 *      Close the given proc handle.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Puts the proc structure to the close list so the reaper thread
 *      can eventually close it. Assumes global lock is held.
 *
 *----------------------------------------------------------------------
 */

static void
CloseWorker(Worker *workerPtr, const Ns_Time *timePtr)
{
    NS_NONNULL_ASSERT(workerPtr != NULL);

    Ns_Log(Ns_LogNsProxyDebug, "nsproxy [%s]: close worker %ld (expire %ld ms)",
           workerPtr->poolPtr->name, (long) workerPtr->pid,
           timePtr != NULL ? Ns_TimeToMilliseconds(timePtr) : -1);

    /*
     * Set the time to kill the worker process. Reaper thread will use passed
     * time to wait for the worker process to exit gracefully.  Otherwise, it
     * will start attempts to stop the worker by sending signals to it (polite
     * and unpolite).
     */

    SetExpire(workerPtr, timePtr);

    /*
     * Closing the write pipe should normally make proxy exit.
     */

    ns_close(workerPtr->wfd);
    workerPtr->signal  = 0;
    workerPtr->sigsent = 0;

    /*
     * Put on the head of the close list so it's handled by
     * the reaper thread.
     */

    workerPtr->nextPtr = firstClosePtr;
    firstClosePtr = workerPtr;

    Ns_Log(Ns_LogNsProxyDebug, "nsproxy [%s]: worker %ld closed",
           workerPtr->poolPtr->name, (long) workerPtr->pid);
}


static int
CloseWorkerOfProxy(Proxy *proxyPtr, const char *proxyId, const Ns_Time *timePtr)
{
    int reap = 0;

    NS_NONNULL_ASSERT(proxyPtr != NULL);

    if (proxyId == NULL || STREQ(proxyId, proxyPtr->id)) {
        if (proxyPtr->workerPtr != NULL) {
            CloseWorker(proxyPtr->workerPtr, timePtr);
            proxyPtr->workerPtr = NULL;
            reap = 1;
        }
    }
    return reap;
}


/*
 *----------------------------------------------------------------------
 *
 * CloseProxy --
 *
 *      Close the given proxy handle.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Starts the thread which reaps worker processes.
 *
 *----------------------------------------------------------------------
 */

static void
CloseProxy(Proxy *proxyPtr)
{
    NS_NONNULL_ASSERT(proxyPtr);

    if (proxyPtr->workerPtr != NULL) {
        Ns_MutexLock(&plock);
        CloseWorker(proxyPtr->workerPtr, &proxyPtr->conf.twait);
        proxyPtr->workerPtr = NULL;
        proxyPtr->numruns  = 0;
        Ns_MutexUnlock(&plock);
        ReapProxies();
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ReaperThread --
 *
 *      Detached thread which closes expired workers or workers
 *      explicitly put on the close list.
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
ReaperThread(void *UNUSED(arg))
{
    Tcl_HashSearch  search;
    Proxy          *proxyPtr, *prevPtr, *nextPtr;
    Pool           *poolPtr;
    Worker           *workerPtr, *tmpWorkerPtr;
    Ns_Time         timeout, now, diff;
    long            ntotal;

    Ns_ThreadSetName("-nsproxy:reap-");
    Ns_Log(Notice, "starting");

    Ns_MutexLock(&plock);

    reaperState = Running;
    Ns_CondSignal(&pcond); /* Wakeup starter thread */

    while (1) {
        Tcl_HashEntry *hPtr;
        Worker          *prevWorkerPtr;

        Ns_GetTime(&now);

        timeout.sec  = TIME_T_MAX;
        timeout.usec = 0;

        Ns_Log(Ns_LogNsProxyDebug, "reaper run");

        /*
         * Check all proxy pools and see if there are
         * idle processes we can get rid off. Also
         * adjust the time to wait until the next
         * run of the loop.
         */

        hPtr = Tcl_FirstHashEntry(&pools, &search);
        while (hPtr != NULL) {

            /*
             * Get max time to wait for the whole pool
             */

            poolPtr = (Pool *)Tcl_GetHashValue(hPtr);

            Ns_Log(Ns_LogNsProxyDebug, "reaper checks pool %s", poolPtr->name);

            Ns_MutexLock(&poolPtr->lock);
            if (poolPtr->conf.tidle.sec != 0 && poolPtr->conf.tidle.usec != 0) {
                diff = now;
                Ns_IncrTime(&diff, poolPtr->conf.tidle.sec, poolPtr->conf.tidle.usec);
                if (Ns_DiffTime(&diff, &timeout, NULL) < 0) {
                    timeout = diff;
                    Ns_Log(Ns_LogNsProxyDebug, "reaper sets timeout based on idle diff %ld.%06ld of pool %s",
                           timeout.sec, timeout.usec, poolPtr->name);
                }
            }

            /*
             * Get max time to wait for one of the worker process.
             * This is less then time for the whole pool.
             */

            proxyPtr = poolPtr->firstPtr;
            prevPtr = NULL;
            while (proxyPtr != NULL) {
                bool expired;

                nextPtr  = proxyPtr->nextPtr;
                workerPtr = proxyPtr->workerPtr;
                ntotal   = poolPtr->nfree + poolPtr->nused;
                if (workerPtr != NULL) {
                    expired = (Ns_DiffTime(&workerPtr->expire, &now, NULL) <= 0);
                    Ns_Log(Ns_LogNsProxyDebug, "pool %s worker %ld expired %d",
                           poolPtr->name, (long)workerPtr->pid, expired);

                    if (!expired && Ns_DiffTime(&workerPtr->expire, &timeout, NULL) <= 0) {
                        timeout = workerPtr->expire;
                        Ns_Log(Ns_LogNsProxyDebug, "reaper sets timeout based on expire %ld.%06ld pool %s worker %ld",
                               timeout.sec, timeout.usec, poolPtr->name, (long)workerPtr->pid);
                    }
                } else {
                    expired = NS_FALSE;
                }
                if (poolPtr->maxworker < ntotal) {
                    /*
                     * Prune the excessive proxy and close the worker.
                     */
                    if (prevPtr != NULL) {
                        prevPtr->nextPtr = proxyPtr->nextPtr;
                    }
                    if (proxyPtr == poolPtr->firstPtr) {
                        poolPtr->firstPtr = proxyPtr->nextPtr;
                    }
                    if (workerPtr != NULL) {
                        CloseWorker(workerPtr, &proxyPtr->conf.twait);
                    }
                    FreeProxy(proxyPtr);
                    proxyPtr = NULL;
                    poolPtr->nfree--;
                } else if (expired) {
                    /*
                     * Close the worker but leave the proxy.
                     */
                    CloseWorker(proxyPtr->workerPtr, &proxyPtr->conf.twait);
                    proxyPtr->workerPtr = NULL;
                }
                if (proxyPtr != NULL) {
                    prevPtr = proxyPtr;
                }
                proxyPtr = nextPtr;
            }
            Ns_MutexUnlock(&poolPtr->lock);
            hPtr = Tcl_NextHashEntry(&search);
        }

        /*
         * Check any closing procs. Also adjust the time
         * to wait until the next run of the loop.
         */

        workerPtr = firstClosePtr;
        prevWorkerPtr = NULL;

        while (workerPtr != NULL) {
            if (Ns_DiffTime(&now, &workerPtr->expire, NULL) > 0) {

                /*
                 * Stop time expired, add new quantum and signal
                 * the process to exit. After first quantum has
                 * expired, be polite and try the TERM signal.
                 * If this does not get the process down within
                 * the second quantum, try the KILL signal.
                 * If this does not get the process down within
                 * the third quantum, abort - we have a zombie.
                 */

                Ns_IncrTime(&workerPtr->expire,
                            workerPtr->poolPtr->conf.twait.sec,
                            workerPtr->poolPtr->conf.twait.usec);
                switch (workerPtr->signal) {
                case 0:       workerPtr->signal = SIGTERM; break;
                case SIGTERM: workerPtr->signal = SIGKILL; break;
                case SIGKILL: workerPtr->signal = -1;      break;
                }
            }

            if (workerPtr->signal == -1
                || workerPtr->rfd == NS_INVALID_FD
                || WaitFd(workerPtr->rfd, POLLIN, 0)) {

                /*
                 * We either have timeouted eval (rfd==NS_INVALID_FD), a
                 * zombie or the process has exited ok so splice it out the
                 * list.
                 */

                if (prevWorkerPtr != NULL) {
                    prevWorkerPtr->nextPtr = workerPtr->nextPtr;
                } else {
                    firstClosePtr = workerPtr->nextPtr;
                }

                if (workerPtr->signal == -1) {
                    Ns_Log(Warning, "nsproxy: zombie: %ld", (long)workerPtr->pid);
                } else {
                    int waitStatus = 0;

                    /*
                     * Pass waitStatus ptr to Ns_WaitForProcessStatus() to
                     * indicate that we want to handle the signal here and to
                     * suppress warning entries in the error.log.
                     *
                     * The following wait operation should not really wait
                     * but it is better to play safe.
                     */

                    Ns_MutexUnlock(&plock);
                    (void) Ns_WaitForProcessStatus(workerPtr->pid, NULL, &waitStatus);
                    Ns_MutexLock(&plock);
#ifdef WTERMSIG
                    if (workerPtr->signal != 0 && WTERMSIG(waitStatus) != 0) {
                        Ns_LogSeverity severity;

                        if (WTERMSIG(waitStatus) != workerPtr->signal) {
                            severity = Warning;
                        } else {
                            severity = Notice;
                        }
                        Ns_Log(severity, "nsproxy process %d killed with signal %d (%s)",
                               workerPtr->pid,
                               WTERMSIG(waitStatus), strsignal(WTERMSIG(waitStatus)));
                    }
#endif
                }

                tmpWorkerPtr = workerPtr->nextPtr;
                if (workerPtr->rfd != NS_INVALID_FD) {
                    ns_close(workerPtr->rfd);
                }
                ns_free(workerPtr);
                workerPtr = tmpWorkerPtr;

            } else {

                /*
                 * Process is still around, try killing it but leave it
                 * in the list. Calculate the latest time we'll visit
                 * this one again.
                 */

                if (Ns_DiffTime(&workerPtr->expire, &timeout, NULL) < 0) {
                    Ns_Log(Ns_LogNsProxyDebug, "reaper shortens timeout to %ld.%06ld based on expire in pool %s worker %ld kill %d",
                           timeout.sec, timeout.usec, workerPtr->poolPtr->name, (long)workerPtr->pid, workerPtr->signal);
                    timeout = workerPtr->expire;
                }
                if (workerPtr->signal != workerPtr->sigsent) {
                    Ns_Log(Warning, "[%s]: pid %ld won't die, send signal %d",
                           workerPtr->poolPtr->name, (long)workerPtr->pid,
                           workerPtr->signal);
                    if (kill(workerPtr->pid, workerPtr->signal) != 0 && errno != ESRCH) {
                        Ns_Log(Error, "kill(%ld, %d) failed: %s",
                               (long)workerPtr->pid, workerPtr->signal, strerror(errno));
                    }
                    workerPtr->sigsent = workerPtr->signal;
                }
                prevWorkerPtr = workerPtr;
                workerPtr = workerPtr->nextPtr;
            }
        }

        /*
         * Here we wait until signaled, or at most the
         * time we need to expire next worker or kill
         * some of them found on the close list.
         */

        if (Ns_DiffTime(&timeout, &now, &diff) > 0) {
            reaperState = Sleeping;
            Ns_CondBroadcast(&pcond);
            if (timeout.sec == TIME_T_MAX && timeout.usec == 0) {
                Ns_Log(Ns_LogNsProxyDebug, "reaper waits unlimited for cond");
                Ns_CondWait(&pcond, &plock);
            } else {
                Ns_Log(Ns_LogNsProxyDebug, "reaper waits for cond with timeout %ld.%06ld",
                       timeout.sec, timeout.usec);
                (void) Ns_CondTimedWait(&pcond, &plock, &timeout);
            }
            if (reaperState == Stopping) {
                break;
            }
            reaperState = Running;
        }
    }

    reaperState = Stopped;
    Ns_CondSignal(&pcond);
    Ns_MutexUnlock(&plock);

    Ns_Log(Notice, "exiting");
}


/*
 *----------------------------------------------------------------------
 *
 * FreeProxy --
 *
 *      Disposes a proxy handle.
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
FreeProxy(Proxy *proxyPtr)
{
    NS_NONNULL_ASSERT(proxyPtr);

    Tcl_DStringFree(&proxyPtr->in);
    Tcl_DStringFree(&proxyPtr->out);
    ns_free(proxyPtr->id);
    ns_free(proxyPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * FreePool --
 *
 *      Disposes a pool handle (call only on server exit).
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
FreePool(Pool *poolPtr)
{
    NS_NONNULL_ASSERT(poolPtr != NULL);

    if (poolPtr->exec != NULL) {
        ns_free((char *)poolPtr->exec);
    }
    if (poolPtr->init != NULL) {
        ns_free((char *)poolPtr->init);
    }
    if (poolPtr->reinit != NULL) {
        ns_free((char *)poolPtr->reinit);
    }
    if (poolPtr->env) {
        Ns_SetFree(poolPtr->env);
    }

    Ns_CondDestroy(&poolPtr->cond);
    Ns_MutexDestroy(&poolPtr->lock);

    ns_free((char *)poolPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * PushProxy --
 *
 *      Return a proxy to the pool.
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
PushProxy(Proxy *proxyPtr)
{
    Pool     *poolPtr;

    NS_NONNULL_ASSERT(proxyPtr != NULL);

    poolPtr = proxyPtr->poolPtr;
    /*
     * Clears the proxy for the next use
     */

    ResetProxy(proxyPtr);

    /*
     * Divorce from the per-interpreter tables
     */

    if (proxyPtr->cntPtr != NULL) {
        intptr_t  nhave = (intptr_t) Tcl_GetHashValue(proxyPtr->cntPtr);

        nhave--;
        Tcl_SetHashValue(proxyPtr->cntPtr, (ClientData) nhave);
        if (proxyPtr->idPtr != NULL) {
            Tcl_DeleteHashEntry(proxyPtr->idPtr);
            proxyPtr->idPtr = NULL;
        }
        proxyPtr->cntPtr = NULL;
    }

    /*
     * Return the proxy to the pool, pruning it if
     * its addition to the pool will break limits.
     */

    Ns_MutexLock(&poolPtr->lock);
    poolPtr->nused--;
    if ((poolPtr->nused + poolPtr->nfree) <= poolPtr->maxworker) {
        proxyPtr->nextPtr = poolPtr->firstPtr;
        poolPtr->firstPtr = proxyPtr;
        if (proxyPtr->workerPtr != NULL) {
            SetExpire(proxyPtr->workerPtr, &proxyPtr->conf.tidle);
        }
        proxyPtr->conf = poolPtr->conf;
        proxyPtr = NULL;
        poolPtr->nfree++;
        Ns_CondBroadcast(&poolPtr->cond);
    }
    Ns_MutexUnlock(&poolPtr->lock);

    /*
     * Check for an excessive proxy
     */

    if (proxyPtr != NULL) {
        CloseProxy(proxyPtr);
        FreeProxy(proxyPtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ReleaseProxy --
 *
 *      Release a proxy from the per-interp table.
 *
 * Results:
 *      Result of reinit call or TCL_OK if no reinit.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ReleaseProxy(Tcl_Interp *interp, Proxy *proxyPtr)
{
    int         result = TCL_OK;
    Tcl_CmdInfo cmdinfo;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    if (proxyPtr->state == Idle) {
        Tcl_DString ds;
        int         reinit;

        Tcl_DStringInit(&ds);
        Ns_MutexLock(&proxyPtr->poolPtr->lock);
        reinit = proxyPtr->poolPtr->reinit != NULL;
        if (reinit != 0) {
            Tcl_DStringAppend(&ds, proxyPtr->poolPtr->reinit, -1);
        }
        Ns_MutexUnlock(&proxyPtr->poolPtr->lock);
        if (reinit != 0) {
            result = Eval(interp, proxyPtr, Tcl_DStringValue(&ds), NULL);
        }
        Tcl_DStringFree(&ds);

    } else if ( (proxyPtr->state == Busy) && (proxyPtr->workerPtr != NULL) ) {
        proxyPtr->workerPtr->signal = 0;
        Ns_Log(Notice, "releasing busy proxy %s", proxyPtr->id);

        /*
         * In case the proxy is busy, make sure to drain the pipe, otherwise
         * the proxy might be hanging in a send operation. Closing our end
         * causes in the worker process an exception and terminates the
         * potentially blocking write operation.
         */
        ns_close(proxyPtr->workerPtr->rfd);
        proxyPtr->workerPtr->rfd = NS_INVALID_FD;
    }
    if (proxyPtr->cmdToken != NULL) {
        /*
         * Modify command definition inline so it does not
         * attempt to call us recursively when deleted.
         */
        Tcl_GetCommandInfoFromToken(proxyPtr->cmdToken, &cmdinfo);
        cmdinfo.deleteProc = NULL;
        cmdinfo.deleteData = NULL;
        Tcl_SetCommandInfoFromToken(proxyPtr->cmdToken, &cmdinfo);
        Tcl_DeleteCommand(interp, proxyPtr->id);
    }

    PushProxy(proxyPtr);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * RunProxyCmd --
 *
 *      Activated when somebody calls proxy command.
 *
 * Results:
 *      Result of the script as with Tcl eval.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
RunProxyCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char       *scriptString;
    int         result;
    Ns_Time          *timeoutPtr = NULL;
    Ns_ObjvSpec args[] = {
        {"script",    Ns_ObjvString, &scriptString, NULL},
        {"?timeout",  Ns_ObjvTime,   &timeoutPtr,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Proxy *proxyPtr = (Proxy *)clientData;

        result = Eval(interp, proxyPtr, scriptString, timeoutPtr);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * DelProxyCmd --
 *
 *      Release a proxy from the per-interp table.
 *
 * Results:
 *      Result of reinit call or TCL_OK if no reinit.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
DelProxyCmd(ClientData clientData)
{
    Proxy *proxyPtr = (Proxy *)clientData;

    /*
     * Prevents the ReleaseProxy to attempt to delete the associated
     * Tcl command, which would call us recursively otherwise.
     */

    proxyPtr->cmdToken = NULL;
    ReleaseProxy(proxyPtr->interp, proxyPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * ReleaseHandles --
 *
 *      Release any remaining handles in the per-interp table.
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
ReleaseHandles(Tcl_Interp *interp, InterpData *idataPtr)
{
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(idataPtr != NULL);

    hPtr = Tcl_FirstHashEntry(&idataPtr->ids, &search);
    while (hPtr != NULL) {
        Proxy  *proxyPtr = (Proxy *)Tcl_GetHashValue(hPtr);

        (void) ReleaseProxy(interp, proxyPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DeleteData --
 *
 *      Tcl assoc data cleanup for ns_proxy.
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
DeleteData(ClientData clientData, Tcl_Interp *interp)
{
    InterpData *idataPtr = clientData;

    ReleaseHandles(interp, idataPtr);
    Tcl_DeleteHashTable(&idataPtr->ids);
    Tcl_DeleteHashTable(&idataPtr->cnts);
    ns_free(idataPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * ReapProxies --
 *
 *      Wakes up the reaper thread and waits until it does
 *      it's job and goes sleeping again.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will start the reaper thread if not already running.
 *
 *----------------------------------------------------------------------
 */

static void
ReapProxies(void)
{
    Ns_MutexLock(&plock);
    if (reaperState == Stopped) {
        reaperState = Starting;
        Ns_ThreadCreate(ReaperThread, NULL, 0, NULL);
    } else {
        reaperState = Awaken;
        Ns_CondSignal(&pcond);
    }
    while (reaperState != Sleeping) {
        Ns_CondWait(&pcond, &plock);
    }
    Ns_MutexUnlock(&plock);
}


/*
 *----------------------------------------------------------------------
 *
 * GetTimeDiff --
 *
 *      Returns time difference in milliseconds between current time
 *      and time given in passed structure. If the current time is
 *      later than the passed time, the result is negative.
 *
 * Results:
 *      Number of milliseconds (may be negative!)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static long
GetTimeDiff(const Ns_Time *timePtr)
{
    Ns_Time now, diff;

    NS_NONNULL_ASSERT(timePtr != NULL);

    Ns_GetTime(&now);
    Ns_DiffTime(timePtr, &now, &diff);
    return (long)Ns_TimeToMilliseconds(&diff);
}

/*
 *----------------------------------------------------------------------
 *
 * ProxyError --
 *
 *      Formats an extended error message.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will set the errorCode global variable.
 *
 *----------------------------------------------------------------------
 */

static void
ProxyError(Tcl_Interp *interp, Err err)
{
    const char *sysmsg;

    NS_NONNULL_ASSERT(interp != NULL);

    sysmsg = NULL;
    Tcl_SetErrorCode(interp, "NSPROXY", errCode[err], errMsg[err], sysmsg, (char *)0L);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
