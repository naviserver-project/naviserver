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
 *        o. Chroot of the slave
 *        o. Limit duration of the execution in the slave
 *        o. ...
 *
 *      Add -onexit for slave to run on teardown
 *      Add channels to proxy, so we can talk to it
 */

#include "nsproxy.h"

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
 * The following structure defines a running proxy slave process.
 */

typedef struct Slave {
    int           rfd;
    int           wfd;
    int           signal;
    int           sigsent;
    int           twait;
    pid_t         pid;
    Ns_Time       expire;
    struct Pool  *poolPtr;
    struct Slave  *nextPtr;
} Slave;

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
    int            tget;     /* Timeout (ms) when getting proxy handles */
    int            teval;    /* Timeout (ms) when evaluating scripts */
    int            tsend;    /* Timeout (ms) to send data to proxy over pipe */
    int            trecv;    /* Timeout (ms) to receive results over pipe */
    int            twait;    /* Timeout (ms) to wait for slaves to die */
    int            tidle;    /* Timeout (ms) for slave to be idle */
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
    Slave         *slavePtr; /* Running slave, if any */
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
    Running,   /* Operating on pools and tearing down slaves */
    Sleeping,  /* Sleeping on cond var and waiting for work */
    Awaken,    /* Help state to distinguish from running */
    Stopping   /* Teardown of the thread initiated */
} ReaperState;

typedef struct Pool {
    const char    *name;     /* Name of pool */
    struct Proxy  *firstPtr; /* First in list of avail proxies */
    struct Proxy  *runPtr;   /* First in list of running proxies */
    const char    *exec;     /* Slave executable */
    const char    *init;     /* Init script to eval on proxy start */
    const char    *reinit;   /* Re-init scripts to eval on proxy put */
    int            waiting;  /* Thread waiting for handles */
    int            maxslaves;/* Max number of allowed proxies */
    int            nfree;    /* Current number of available proxy handles */
    int            nused;    /* Current number of used proxy handles */
    uintptr_t      nextid;   /* Next in proxy unique ids */
    ProxyConf      conf;     /* Collection of config options to pass to proxy */
    Ns_Set         *env;     /* Set with environment to pass to proxy */
    Ns_Mutex       lock;     /* Lock around the pool */
    Ns_Cond        cond;     /* Cond for use while allocating handles */
} Pool;

#define MIN_IDLE_TIMEOUT 10000 /* == 10 seconds */

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

Ns_LogSeverity Ns_LogNsProxyDebug = 0;


/*
 * Static functions defined in this file.
 */

static Tcl_ObjCmdProc ProxyObjCmd;
static Tcl_ObjCmdProc ConfigureObjCmd;
static Tcl_ObjCmdProc RunProxyCmd;
static Tcl_ObjCmdProc GetObjCmd;

static Tcl_CmdDeleteProc DelProxyCmd;
static Tcl_InterpDeleteProc DeleteData;

static Ns_ShutdownProc Shutdown;

static Pool*  GetPool(const char *poolName, InterpData *idataPtr) NS_GNUC_NONNULL(1);
static void   FreePool(Pool *poolPtr) NS_GNUC_NONNULL(1);

static Proxy* CreateProxy(Pool *poolPtr) NS_GNUC_NONNULL(1);
static Err    PopProxy(Pool *poolPtr, Proxy **proxyPtrPtr, int nwant, int ms)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void   PushProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static Proxy* GetProxy(const char *proxyId, InterpData *idataPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int    Eval(Tcl_Interp *interp, Proxy *proxyPtr, const char *script, int ms)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Err    Send(Tcl_Interp *interp, Proxy *proxyPtr, const char *script) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Err    Wait(Tcl_Interp *interp, Proxy *proxyPtr, int ms) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Err    Recv(Tcl_Interp *interp, Proxy *proxyPtr, int *resultPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static Err    CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int    ReleaseProxy(Tcl_Interp *interp, Proxy *proxyPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void   CloseProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static void   FreeProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static void   ResetProxy(Proxy *proxyPtr) NS_GNUC_NONNULL(1);
static void   ProxyError(Tcl_Interp *interp, Err err) NS_GNUC_NONNULL(1);
static void   FmtActiveProxy(Tcl_Interp *interp, Proxy *proxyPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void   ReleaseHandles(Tcl_Interp *interp, InterpData *idataPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Slave* ExecSlave(Tcl_Interp *interp, Proxy *proxyPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Err    CreateSlave(Tcl_Interp *interp, Proxy *proxyPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void   SetExpire(Slave *slavePtr, int ms) NS_GNUC_NONNULL(1);
static bool   SendBuf(Slave *slavePtr, int ms, Tcl_DString *dsPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static bool   RecvBuf(Slave *slavePtr, int ms, Tcl_DString *dsPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static int    WaitFd(int fd, short events, long ms);

static int    Import(Tcl_Interp *interp, Tcl_DString *dsPtr, int *resultPtr) \
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static void   Export(Tcl_Interp *interp, int code, Tcl_DString *dsPtr) NS_GNUC_NONNULL(3);

static void   UpdateIov(struct iovec *iov, size_t n) NS_GNUC_NONNULL(1);
static void   SetOpt(const char *str, char const **optPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void   ReaperThread(void *UNUSED(arg));
static void   CloseSlave(Slave *slavePtr, int ms) NS_GNUC_NONNULL(1);
static void   ReapProxies(void);
static long   GetTimeDiff(Ns_Time *timePtr) NS_GNUC_NONNULL(1);

static void   AppendStr(Tcl_Obj *listObj, const char *flag, const char *val) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void   AppendInt(Tcl_Obj *listObj, const char *flag, int i) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static Tcl_Obj* StringObj(const char* chars);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable pools;     /* Tracks proxy pools */

ReaperState reaperState = Stopped;

static Ns_Cond  pcond = NULL;          /* Those are used to control access to */
static Ns_Mutex plock = NULL;          /* the list of Slave structures of slave */
static Slave    *firstClosePtr = NULL; /* processes which are being closed. */

static Ns_DString defexec;             /* Stores full path of the proxy executable */


/*
 *----------------------------------------------------------------------
 *
 * Nsproxy_Init --
 *
 *      libnsproxy initialisation.
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

        Ns_DStringInit(&defexec);
        Ns_BinPath(&defexec, "nsproxy", NULL);
        Tcl_InitHashTable(&pools, TCL_STRING_KEYS);

        Ns_RegisterAtShutdown(Shutdown, NULL);
        Ns_RegisterProcInfo((Ns_Callback *)Shutdown, "nsproxy:shutdown", NULL);

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
 *      Main loop for nsproxy slave processes. Initialize Tcl interp
 *      and loop processing requests. On communication errors or
 *      when the peer closes it's write-pipe, slave exits gracefully.
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
Ns_ProxyMain(int argc, char **argv, Tcl_AppInitProc *init)
{
    Tcl_Interp  *interp;
    Slave        proc;
    int          result, max;
    Tcl_DString  in, out;
    const char  *script, *dots, *uarg = NULL, *user = NULL;
    char        *group = NULL, *active;
    uint16       major, minor;

    /*
     * The call to Tcl_FindExecutable() must be done before we ever
     * attempt any Tcl related call.
     */
    Tcl_FindExecutable(argv[0]);

    Nsproxy_LibInit();

    if (argc > 4 || argc < 3) {
        char *pgm = strrchr(argv[0], INTCHAR('/'));
        Ns_Fatal("usage: %s pool id ?command?", (pgm != NULL) ? ++pgm : argv[0]);
    }
    if (argc < 4) {
        active = NULL;
        max = -1;
    } else {
        active = argv[3];
        max = (int)strlen(active) - 8;
        if (max < 0) {
            active = NULL;
        }
    }

    /*
     * Initialize Slave structure
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
    if (ns_open("/dev/null", O_RDONLY, 0) != 0) {
        Ns_Fatal("nsproxy: open: %s", strerror(errno));
    }
    ns_close(1);
    if (ns_dup(2) != 1) {
        Ns_Fatal("nsproxy: dup: %s", strerror(errno));
    }

    /*
     * Make sure possible child processes do not inherit this one.
     * As, when the user evalutes the "exec" command, the child
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
        uarg = ns_strdup(++user);
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

    while (RecvBuf(&proc, -1, &in) == NS_TRUE) {
        Req *reqPtr;
	uint32_t len;

        if (Tcl_DStringLength(&in) < (int)sizeof(Req)) {
            break;
        }
        reqPtr = (Req *) Tcl_DStringValue(&in);
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
                    dots = "";
                } else {
                    dots = " ...";
                    n = max;
                }
                sprintf(active, "{%.*s%s}", n, script, dots);
            }
            result = Tcl_EvalEx(interp, script, (int)len, 0);
            Export(interp, result, &out);
            if (active != NULL) {
                memset(active, ' ', (size_t)max);
            }
        } else {
            Ns_Fatal("nsproxy: invalid length");
        }
        if (SendBuf(&proc, -1, &out) == NS_FALSE) {
            break;
        }
        Tcl_DStringTrunc(&in, 0);
        Tcl_DStringTrunc(&out, 0);
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
Shutdown(const Ns_Time *toutPtr, void *UNUSED(arg))
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

    if (toutPtr == NULL) {
        Tcl_HashEntry *hPtr;

        Ns_MutexLock(&plock);
        hPtr = Tcl_FirstHashEntry(&pools, &search);
        while (hPtr != NULL) {
            poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
            Ns_MutexLock(&poolPtr->lock);
            poolPtr->maxslaves = 0; /* Disable creation of new slaves */
            proxyPtr = poolPtr->firstPtr;
            while (proxyPtr != NULL) {
                if (proxyPtr->slavePtr != NULL) {
                    CloseSlave(proxyPtr->slavePtr, proxyPtr->conf.twait);
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
        status = Ns_CondTimedWait(&pcond, &plock, toutPtr);
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
Ns_ProxyGet(Tcl_Interp *interp, const char *poolName, PROXY* handlePtr, int ms)
{
    Pool  *poolPtr;
    Proxy *proxyPtr;
    Err    err;
    int    result;

    /*
     * Get just one proxy from the pool
     */
    poolPtr = GetPool(poolName, NULL);

    err = PopProxy(poolPtr, &proxyPtr, 1, ms);
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

int Ns_ProxyEval(Tcl_Interp *interp, PROXY handle, const char *script, int ms)
{
    return Eval(interp, (Proxy *)handle, script, ms);
}

/*
 *----------------------------------------------------------------------
 *
 * ExecSlave --
 *
 *      Create a new proxy slave.
 *
 * Results:
 *      Pointer to new Slave or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Slave *
ExecSlave(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Pool  *poolPtr;
    char  *argv[5];
    char   active[100];
    Slave *slavePtr;
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

    slavePtr = ns_calloc(1u, sizeof(Slave));
    slavePtr->poolPtr = proxyPtr->poolPtr;
    slavePtr->pid = pid;
    slavePtr->rfd = wpipe[0];
    slavePtr->wfd = rpipe[1];

    SetExpire(slavePtr, proxyPtr->conf.tidle);

    Ns_Log(Ns_LogNsProxyDebug, "nsproxy: slave %ld started", (long) slavePtr->pid);

    return slavePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * SetExpire --
 *
 *      Sets the absolute expire time for the slave.
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
SetExpire(Slave *slavePtr, int ms)
{
    NS_NONNULL_ASSERT(slavePtr != NULL);

    Ns_Log(Ns_LogNsProxyDebug, "set expire in %d ms for pool %s slave %ld",
           ms, slavePtr->poolPtr->name, (long)slavePtr->pid);

    if (ms > 0) {
        Ns_GetTime(&slavePtr->expire);
        Ns_IncrTime(&slavePtr->expire, (ms / 1000), ((ms % 1000) * 1000) );
    } else {
        slavePtr->expire.sec  = TIME_T_MAX;
        slavePtr->expire.usec = 0;
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
Eval(Tcl_Interp *interp, Proxy *proxyPtr, const char *script, int ms)
{
    Err err;
    int status = TCL_ERROR;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    err = Send(interp, proxyPtr, script);
    if (err == ENone) {
        err = Wait(interp, proxyPtr, ms);
        if (err == ENone) {
            (void) Recv(interp, proxyPtr, &status);
        }
    }

    return status;
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

    if (proxyPtr->slavePtr == NULL) {
        err = EDead;
    } else if (proxyPtr->state != Idle) {
        err = EBusy;
    } else {
        proxyPtr->numruns++;
        if (proxyPtr->conf.maxruns > 0
            && proxyPtr->numruns > proxyPtr->conf.maxruns) {
            Ns_Log(Ns_LogNsProxyDebug, "proxy maxrun reached pool %s slave %ld",
                   proxyPtr->poolPtr->name, (long)proxyPtr->slavePtr->pid);
            CloseProxy(proxyPtr);
            err = CreateSlave(interp, proxyPtr);
        }
        if (err == ENone) {
	    size_t len = script == NULL ? 0u : strlen(script);

            req.len   = htonl((uint32_t)len);
            req.major = htons(MAJOR_VERSION);
            req.minor = htons(MINOR_VERSION);
            Tcl_DStringTrunc(&proxyPtr->in, 0);
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
                Ns_Log(Ns_LogNsProxyDebug, "proxy send pool %s slave %ld: %s",
                       proxyPtr->poolPtr->name, (long)proxyPtr->slavePtr->pid, script);
            }

            if (SendBuf(proxyPtr->slavePtr, proxyPtr->conf.tsend,
                         &proxyPtr->in) == NS_FALSE) {
                err = ESend;
            }
        }
    }

    if (err != ENone) {
        Ns_TclPrintfResult(interp, "could not send script \"%s\" to proxy \"%s\": %s",
                           script == NULL ? "" : script,
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
Wait(Tcl_Interp *interp, Proxy *proxyPtr, int ms)
{
    Err err = ENone;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    if (proxyPtr->state == Idle) {
        err = EIdle;
    } else if (proxyPtr->slavePtr == NULL) {
        err = EDead;
    } else if (proxyPtr->state != Done) {
        if (ms <= 0) {
            ms = proxyPtr->conf.teval;
        }
        if (ms <= 0) {
            ms = -1;
        }
        if (WaitFd(proxyPtr->slavePtr->rfd, POLLIN, ms) == 0) {
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
        Tcl_DStringTrunc(&proxyPtr->out, 0);
        if (RecvBuf(proxyPtr->slavePtr, proxyPtr->conf.trecv,
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
 *      Send a dstring buffer to the specified slave.
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
SendBuf(Slave *slavePtr, int ms, Tcl_DString *dsPtr)
{
    ssize_t      n;
    uint32       ulen;
    struct iovec iov[2];
    Ns_Time      end;
    bool         success = NS_TRUE;

    NS_NONNULL_ASSERT(slavePtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (ms > 0) {
        Ns_GetTime(&end);
        Ns_IncrTime(&end, ms/1000, (ms % 1000) * 1000);
    }

    ulen = htonl((unsigned int)dsPtr->length);
    iov[0].iov_base = (void *)&ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = (size_t)dsPtr->length;

    while ((iov[0].iov_len + iov[1].iov_len) > 0u) {
        do {
            n = writev(slavePtr->wfd, iov, 2);
        } while (n == -1 && errno == NS_EINTR);

        if (n == -1) {
            long waitMs;

            if ((errno != EAGAIN) && (errno != NS_EWOULDBLOCK)) {
                success = NS_FALSE;
                break;

            } else if (ms > 0) {
                waitMs = GetTimeDiff(&end);
                if (waitMs < 0) {
                    success = NS_FALSE;
                    break;
                }
            } else {
                waitMs = ms;
            }
            if (WaitFd(slavePtr->wfd, POLLOUT, waitMs) == 0) {
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
RecvBuf(Slave *slavePtr, int ms, Tcl_DString *dsPtr)
{
    uint32       ulen;
    ssize_t      n;
    size_t       avail;
    struct iovec iov[2];
    Ns_Time      end;
    bool         success = NS_TRUE;

    NS_NONNULL_ASSERT(slavePtr != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    if (ms > 0) {
        Ns_GetTime(&end);
        Ns_IncrTime(&end, ms/1000, (ms % 1000) * 1000);
    }

    avail = (size_t)dsPtr->spaceAvl - 1u;
    iov[0].iov_base = (void *)&ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = avail;

    while (iov[0].iov_len > 0) {
        do {
            n = readv(slavePtr->rfd, iov, 2);
        } while ((n == -1) && (errno == NS_EINTR));

        if (n == 0) {
            success = NS_FALSE; /* EOF */
            break;

        } else if (n < 0) {
            long  waitMs;

            if (errno != EAGAIN && errno != NS_EWOULDBLOCK) {
                success = NS_FALSE;
                break;

            } else if (ms > 0) {
                waitMs = GetTimeDiff(&end);
                if (waitMs < 0) {
                    success = NS_FALSE;
                    break;
                }
            } else {
                waitMs = ms;
            }
            if (WaitFd(slavePtr->rfd, POLLIN, waitMs) == 0) {
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
                n = ns_read(slavePtr->rfd, ptr, (size_t)len);
            } while ((n == -1) && (errno == NS_EINTR));

            if (n == 0) {
                success = NS_FALSE; /* EOF */
                break;

            } else if (n < 0) {
                long waitMs;

                if (errno != EAGAIN && errno != NS_EWOULDBLOCK) {
                    success = NS_FALSE;
                    break;

                } else if (ms > 0) {
                    waitMs = GetTimeDiff(&end);
                    if (waitMs < 0) {
                        success = NS_FALSE;
                        break;
                    }
                } else {
                    waitMs = ms;
                }
                if (WaitFd(slavePtr->rfd, POLLIN, waitMs) == 0) {
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
 *      Waits for the given event on the slave pipe.
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
 *      Tcl result code from remote slave.
 *
 * Side effects:
 *      Will set interp result and error data as needed.
 *
 *----------------------------------------------------------------------
 */

static int
Import(Tcl_Interp *interp, Tcl_DString *dsPtr, int *resultPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(resultPtr != NULL);

    if (dsPtr->length < (int)sizeof(Res)) {
        result = TCL_ERROR;

    } else {
        Res        *resPtr = (Res *) dsPtr->string;
        const char *str    = dsPtr->string + sizeof(Res);
        size_t      rlen, clen, ilen;

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
ProxyObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    InterpData    *idataPtr = data;
    Pool          *poolPtr;
    Proxy         *proxyPtr;
    Err            err;
    int            ms, opt, result = TCL_OK;
    const char    *proxyId;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Tcl_Obj       *listObj;

    static const char *const opts[] = {
        "get", "put", "release", "eval", "cleanup", "configure",
        "ping", "free", "active", "handles", "clear", "stop",
        "send", "wait", "recv", "pools", NULL
    };
    enum {
        PGetIdx, PPutIdx, PReleaseIdx, PEvalIdx, PCleanupIdx, PConfigureIdx,
        PPingIdx, PFreeIdx, PActiveIdx, PHandlesIdx, PClearIdx, PStopIdx,
        PSendIdx, PWaitIdx, PRecvIdx, PPoolsIdx
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
    case PReleaseIdx:
    case PPutIdx:
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
                result = Eval(interp, proxyPtr, NULL, -1);
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
            proxyId = Tcl_GetString(objv[2]);
            proxyPtr = GetProxy(proxyId, idataPtr);
            if (proxyPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such handle: %s", proxyId);
                result = TCL_ERROR;
            } else if (objc == 3) {
                ms = -1;
            } else if (Tcl_GetIntFromObj(interp, objv[3], &ms) != TCL_OK) {
                result = TCL_ERROR;
            }
            if (result == TCL_OK) {
                err = Wait(interp, proxyPtr, ms);
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
            }
        }
        break;

    case PEvalIdx:
        if (objc != 4 && objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle script");
            result = TCL_ERROR;
        } else {
            proxyId = Tcl_GetString(objv[2]);
            proxyPtr = GetProxy(proxyId, idataPtr);
            if (proxyPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such handle: %s", proxyId);
                result = TCL_ERROR;
            } else if (objc == 4) {
                ms = -1;
            } else if (Tcl_GetIntFromObj(interp, objv[4], &ms) != TCL_OK) {
                result = TCL_ERROR;
            }
            if (result == TCL_OK) {
                result = Eval(interp, proxyPtr, Tcl_GetString(objv[3]), ms);
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
    case PClearIdx:
        if (objc < 3 || objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool ?handle?");
            result = TCL_ERROR;
        } else {
            Pool *thePoolPtr;
            int   reap;

            thePoolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
            proxyId = (objc >= 4) ? Tcl_GetString(objv[3]) : NULL;
            reap = 0;
            Ns_MutexLock(&plock);
            hPtr = Tcl_FirstHashEntry(&pools, &search);
            while (hPtr != NULL) {
                poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
                if (thePoolPtr == poolPtr) {
                    Ns_MutexLock(&poolPtr->lock);
                    switch (opt) {
                    default:
                    case PStopIdx:  proxyPtr = poolPtr->runPtr;   break;
                    case PClearIdx: proxyPtr = poolPtr->firstPtr; break;
                    }
                    while (proxyPtr != NULL) {
                        if (proxyId == NULL || STREQ(proxyId, proxyPtr->id)) {
                            if (proxyPtr->slavePtr != NULL) {
                                CloseSlave(proxyPtr->slavePtr,proxyPtr->conf.twait);
                                proxyPtr->slavePtr = NULL;
                                reap++;
                            }
                        }
                        switch (opt) {
                        default:
                        case PStopIdx:  proxyPtr = proxyPtr->runPtr;  break;
                        case PClearIdx: proxyPtr = proxyPtr->nextPtr; break;
                        }
                    }
                    Ns_MutexUnlock(&poolPtr->lock);
                }
                hPtr = Tcl_NextHashEntry(&search);
            }
            Ns_MutexUnlock(&plock);
            if (reap != 0) {
                ReapProxies();
            }
        }
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
ConfigureObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    InterpData *idataPtr = data;
    Pool       *poolPtr;
    Proxy      *proxyPtr;
    int         flag, n, result = TCL_OK, reap = 0;

    static const char *const flags[] = {
        "-init", "-reinit", "-maxslaves", "-exec", "-env",
        "-gettimeout", "-evaltimeout", "-sendtimeout", "-recvtimeout",
        "-waittimeout", "-idletimeout", "-maxruns", NULL
    };
    enum {
        CInitIdx, CReinitIdx, CMaxslaveIdx, CExecIdx, CEnvIdx, CGetIdx,
        CEvalIdx, CSendIdx, CRecvIdx, CWaitIdx, CIdleIdx, CMaxrunsIdx
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
            case CGetIdx:
            case CEvalIdx:
            case CSendIdx:
            case CRecvIdx:
            case CWaitIdx:
            case CIdleIdx:
            case CMaxslaveIdx:
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
                switch ((int) flag) {
                case CGetIdx:
                    poolPtr->conf.tget = n;
                    break;
                case CEvalIdx:
                    poolPtr->conf.teval = n;
                    break;
                case CSendIdx:
                    poolPtr->conf.tsend = n;
                    break;
                case CRecvIdx:
                    poolPtr->conf.trecv = n;
                    break;
                case CWaitIdx:
                    poolPtr->conf.twait = n;
                    break;
                case CMaxslaveIdx:
                    poolPtr->maxslaves = n;
                    reap = 1;
                    break;
                case CMaxrunsIdx:
                    poolPtr->conf.maxruns = n;
                    break;
                case CIdleIdx:
                    poolPtr->conf.tidle = n;
                    if (poolPtr->conf.tidle < MIN_IDLE_TIMEOUT) {
                        poolPtr->conf.tidle = MIN_IDLE_TIMEOUT;
                    }
                    proxyPtr = poolPtr->firstPtr;
                    while (proxyPtr != NULL) {
                        if (proxyPtr->slavePtr != NULL) {
                            SetExpire(proxyPtr->slavePtr, proxyPtr->conf.tidle);
                        }
                        proxyPtr = proxyPtr->nextPtr;
                    }
                    reap = 1;
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

        while ((poolPtr->nfree + poolPtr->nused) < poolPtr->maxslaves) {
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
            AppendStr(listObj, flags[CExecIdx],     poolPtr->exec);
            AppendStr(listObj, flags[CInitIdx],     poolPtr->init);
            AppendStr(listObj, flags[CReinitIdx],   poolPtr->reinit);
            AppendInt(listObj, flags[CMaxslaveIdx], poolPtr->maxslaves);
            AppendInt(listObj, flags[CMaxrunsIdx],  poolPtr->conf.maxruns);
            AppendInt(listObj, flags[CGetIdx],      poolPtr->conf.tget);
            AppendInt(listObj, flags[CEvalIdx],     poolPtr->conf.teval);
            AppendInt(listObj, flags[CSendIdx],     poolPtr->conf.tsend);
            AppendInt(listObj, flags[CRecvIdx],     poolPtr->conf.trecv);
            AppendInt(listObj, flags[CWaitIdx],     poolPtr->conf.twait);
            AppendInt(listObj, flags[CIdleIdx],     poolPtr->conf.tidle);
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
        case CMaxslaveIdx: Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->maxslaves));
            break;
        case CMaxrunsIdx:  Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.maxruns));
            break;
        case CGetIdx:      Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.tget));
            break;
        case CEvalIdx:     Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.teval));
            break;
        case CSendIdx:     Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.tsend));
            break;
        case CRecvIdx:     Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.trecv));
            break;
        case CWaitIdx:     Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.twait));
            break;
        case CIdleIdx:     Tcl_SetObjResult(interp, Tcl_NewIntObj(poolPtr->conf.tidle));
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
AppendInt(Tcl_Obj *listObj, const char *flag, int i)
{
    NS_NONNULL_ASSERT(listObj != NULL);
    NS_NONNULL_ASSERT(flag != NULL);

    Tcl_ListObjAppendElement(NULL, listObj, StringObj(flag));
    Tcl_ListObjAppendElement(NULL, listObj, Tcl_NewIntObj(i));
}

static void
AppendStr(Tcl_Obj *listObj, const char *flag, const char *val)
{
    NS_NONNULL_ASSERT(listObj != NULL);
    NS_NONNULL_ASSERT(flag != NULL);

    Tcl_ListObjAppendElement(NULL, listObj, StringObj(flag));
    Tcl_ListObjAppendElement(NULL, listObj, StringObj(val));
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
GetObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    InterpData    *idataPtr = data;
    Proxy         *proxyPtr, *firstPtr;
    Tcl_HashEntry *cntPtr, *idPtr;
    int            isNew, nwant = 1, timeoutMs = -1, ms, result = TCL_OK;
    Err            err;
    Pool          *poolPtr;
    Ns_ObjvSpec    lopts[] = {
        {"-timeout", Ns_ObjvInt, &timeoutMs,  NULL},
        {"-handles", Ns_ObjvInt, &nwant,      NULL},
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

    if (timeoutMs == -1) {
        Ns_MutexLock(&poolPtr->lock);
        ms = poolPtr->conf.tget;
        Ns_MutexUnlock(&poolPtr->lock);
    } else {
        ms = timeoutMs;
    }

    /*
     * Get some number of proxies from the pool
     */

    err = PopProxy(poolPtr, &firstPtr, nwant, ms);
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
PopProxy(Pool *poolPtr, Proxy **proxyPtrPtr, int nwant, int ms)
{
    Proxy        *proxyPtr;
    Err           err;
    Ns_ReturnCode status = NS_OK;
    Ns_Time       tout;

    NS_NONNULL_ASSERT(poolPtr != NULL);
    NS_NONNULL_ASSERT(proxyPtrPtr != NULL);

    if (ms > 0) {
        Ns_GetTime(&tout);
        Ns_IncrTime(&tout, ms/1000, (ms/1000) * 1000);
    }

    Ns_MutexLock(&poolPtr->lock);
    while (status == NS_OK && poolPtr->waiting) {
        if (ms > 0) {
            status = Ns_CondTimedWait(&poolPtr->cond, &poolPtr->lock, &tout);
        } else {
            Ns_CondWait(&poolPtr->cond, &poolPtr->lock);
        }
    }
    if (status != NS_OK) {
        err = EGetTimeout;
    } else {
        poolPtr->waiting = 1;
        while (status == NS_OK
               && poolPtr->nfree < nwant && poolPtr->maxslaves >= nwant) {
            if (ms > 0) {
                status = Ns_CondTimedWait(&poolPtr->cond, &poolPtr->lock,
                                          &tout);
            } else {
                Ns_CondWait(&poolPtr->cond, &poolPtr->lock);
            }
        }
        if (status != NS_OK) {
            err = EGetTimeout;
        } else if (poolPtr->maxslaves == 0 || poolPtr->maxslaves < nwant) {
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
FmtActiveProxy(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Tcl_DString ds;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(proxyPtr != NULL);

    Tcl_DStringInit(&ds);
    Tcl_DStringGetResult(interp, &ds);

    Tcl_DStringStartSublist(&ds);
    Ns_DStringPrintf(&ds, "handle %s slave %ld start %" PRIu64 ":%ld script",
                     proxyPtr->id,
                     (long) ((proxyPtr->slavePtr != NULL) ? proxyPtr->slavePtr->pid : 0),
                     (int64_t) proxyPtr->when.sec, proxyPtr->when.usec);

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
GetPool(const char *poolName, InterpData *idataPtr)
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
        const char *path = NULL, *exec = NULL;
        int i;

        poolPtr = ns_calloc(1u, sizeof(Pool));
        Tcl_SetHashValue(hPtr, poolPtr);
        poolPtr->name = Tcl_GetHashKey(&pools, hPtr);
        if (idataPtr && idataPtr->server && idataPtr->module) {
	  path = Ns_ConfigGetPath(idataPtr->server, idataPtr->module, (char *)0);
        }
        if (path != NULL && (exec = Ns_ConfigGetValue(path, "exec")) != NULL) {
            SetOpt(exec, &poolPtr->exec);
        } else {
            SetOpt(Ns_DStringValue(&defexec), &poolPtr->exec);
        }
        if (path == NULL) {
            poolPtr->conf.tget  = 0;
            poolPtr->conf.teval = 0;
            poolPtr->conf.tsend = 5000;
            poolPtr->conf.trecv = 5000;
            poolPtr->conf.twait = 1000;
            poolPtr->conf.tidle = 5*60*1000;
            poolPtr->maxslaves = 8;
        } else {
            poolPtr->conf.tget  = Ns_ConfigInt(path, "gettimeout",  0);
            poolPtr->conf.teval = Ns_ConfigInt(path, "evaltimeout", 0);
            poolPtr->conf.tsend = Ns_ConfigInt(path, "sendtimeout", 5000);
            poolPtr->conf.trecv = Ns_ConfigInt(path, "recvtimeout", 5000);
            poolPtr->conf.twait = Ns_ConfigInt(path, "waittimeout", 1000);
            poolPtr->conf.tidle = Ns_ConfigInt(path, "idletimeout", 5*60*1000);
            poolPtr->maxslaves  = Ns_ConfigInt(path, "maxslaves", 8);
        }
        for (i = 0; i < poolPtr->maxslaves; i++) {
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
 *      Check a proxy, pinging the proc and creating a new slave
 *      as needed.
 *
 * Results:
 *      ENone if proxy OK, other error if slave couldn't be created.
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

    if ((proxyPtr->slavePtr != NULL)
        && (Eval(interp, proxyPtr, NULL, -1) != TCL_OK)
        ) {
        CloseProxy(proxyPtr);
        Tcl_ResetResult(interp);
    }
    if (proxyPtr->slavePtr == NULL) {
        err = CreateSlave(interp, proxyPtr);
    }

    return err;
}


/*
 *----------------------------------------------------------------------
 *
 * CreateSlave --
 *
 *      Create new proxy slave process
 *
 * Results:
 *      ENone if proxy OK, other error if slave couldn't be created.
 *
 * Side effects:
 *
 *
 *----------------------------------------------------------------------
 */

static Err
CreateSlave(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Pool        *poolPtr;
    Err          err = ENone;
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
    proxyPtr->slavePtr = ExecSlave(interp, proxyPtr);
    if (proxyPtr->slavePtr == NULL) {
        err = EExec;
    } else if (init != 0
               && (Eval(interp, proxyPtr, Tcl_DStringValue(&ds), -1) != TCL_OK)
               ) {
        CloseProxy(proxyPtr);
        err = EInit;
    } else if (Eval(interp, proxyPtr, NULL, -1) != TCL_OK) {
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

    Tcl_DStringTrunc(&proxyPtr->in, 0);
    Tcl_DStringTrunc(&proxyPtr->out, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * CloseSlave --
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
CloseSlave(Slave *slavePtr, int ms)
{
    NS_NONNULL_ASSERT(slavePtr != NULL);

    Ns_Log(Ns_LogNsProxyDebug, "nsproxy [%s]: close slave %ld (expire %d ms)",
           slavePtr->poolPtr->name, (long) slavePtr->pid, ms);

    /*
     * Set the time to kill the slave. Reaper thread will
     * use passed time to wait for the slave to exit gracefully.
     * Otherwise, it will start attempts to stop the slave
     * by sending singnals to it (polite and unpolite).
     */

    SetExpire(slavePtr, ms);

    /*
     * Closing the write pipe should normally make proxy exit.
     */

    ns_close(slavePtr->wfd);
    slavePtr->signal  = 0;
    slavePtr->sigsent = 0;

    /*
     * Put on the head of the close list so it's handled by
     * the reaper thread.
     */

    slavePtr->nextPtr = firstClosePtr;
    firstClosePtr = slavePtr;

    Ns_Log(Ns_LogNsProxyDebug, "nsproxy [%s]: slave %ld closed", slavePtr->poolPtr->name, (long) slavePtr->pid);
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
 *      Starts the thread which reaps slaves.
 *
 *----------------------------------------------------------------------
 */

static void
CloseProxy(Proxy *proxyPtr)
{
    NS_NONNULL_ASSERT(proxyPtr);

    if (proxyPtr->slavePtr != NULL) {
        Ns_MutexLock(&plock);
        CloseSlave(proxyPtr->slavePtr, proxyPtr->conf.twait);
        proxyPtr->slavePtr = NULL;
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
 *      Detached thread which closes expired slaves or slaves
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
    Slave           *slavePtr, *tmpSlavePtr;
    Ns_Time         tout, now, diff;
    int             ms, ntotal;

    Ns_ThreadSetName("-nsproxy:reap-");
    Ns_Log(Notice, "starting");

    Ns_MutexLock(&plock);

    reaperState = Running;
    Ns_CondSignal(&pcond); /* Wakeup starter thread */

    while (1) {
        Tcl_HashEntry *hPtr;
	Slave          *prevSlavePtr;

        Ns_GetTime(&now);

        tout.sec  = TIME_T_MAX;
        tout.usec = 0;

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
            if (poolPtr->conf.tidle != 0) {
                diff = now;
                ms = poolPtr->conf.tidle;
                Ns_IncrTime(&diff, ms/1000, (ms%1000) * 1000);
                if (Ns_DiffTime(&diff, &tout, NULL) < 0) {
                    tout = diff;
                    Ns_Log(Ns_LogNsProxyDebug, "reaper sets timeout based on idle diff %ld.%06ld of pool %s",
                           tout.sec, tout.usec, poolPtr->name);
                }
            }

            /*
             * Get max time to wait for one of the slaves.
             * This is less then time for the whole pool.
             */

            proxyPtr = poolPtr->firstPtr;
            prevPtr = NULL;
            while (proxyPtr != NULL) {
                bool expired;

                nextPtr  = proxyPtr->nextPtr;
                slavePtr = proxyPtr->slavePtr;
                ntotal   = poolPtr->nfree + poolPtr->nused;
                if (slavePtr != NULL) {
                    expired = (Ns_DiffTime(&slavePtr->expire, &now, NULL) <= 0);
                    Ns_Log(Ns_LogNsProxyDebug, "pool %s slave %ld expired %d",
                           poolPtr->name, (long)slavePtr->pid, expired);

                    if (!expired && Ns_DiffTime(&slavePtr->expire, &tout, NULL) <= 0) {
                        tout = slavePtr->expire;
                        Ns_Log(Ns_LogNsProxyDebug, "reaper sets timeout based on expire %ld.%06ld pool %s slave %ld",
                               tout.sec, tout.usec, poolPtr->name, (long)slavePtr->pid);
                    }
                } else {
                    expired = NS_FALSE;
                }
                if (poolPtr->maxslaves < ntotal) {
                    /*
                     * Prune the excessive proxy and close the slave
                     */
                    if (prevPtr != NULL) {
                        prevPtr->nextPtr = proxyPtr->nextPtr;
                    }
                    if (proxyPtr == poolPtr->firstPtr) {
                        poolPtr->firstPtr = proxyPtr->nextPtr;
                    }
                    if (slavePtr != NULL) {
                        CloseSlave(slavePtr, proxyPtr->conf.twait);
                    }
                    FreeProxy(proxyPtr);
                    proxyPtr = NULL;
                    poolPtr->nfree--;
                } else if (expired) {
                    /*
                     * Close the slave but leave the proxy
                     */
                    CloseSlave(proxyPtr->slavePtr, proxyPtr->conf.twait);
                    proxyPtr->slavePtr = NULL;
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

        slavePtr = firstClosePtr;
        prevSlavePtr = NULL;

        while (slavePtr != NULL) {
            if (Ns_DiffTime(&now, &slavePtr->expire, NULL) > 0) {

                /*
                 * Stop time expired, add new quantum and signal
                 * the process to exit. After first quantum has
                 * expired, be polite and try the TERM signal.
                 * If this does not get the process down within
                 * the second quantum, try the KILL signal.
                 * If this does not get the process down within
                 * the third quantum, abort - we have a zombie.
                 */

                Ns_IncrTime(&slavePtr->expire, slavePtr->poolPtr->conf.twait/1000,
                            (slavePtr->poolPtr->conf.twait % 1000) * 1000);
                switch (slavePtr->signal) {
                case 0:       slavePtr->signal = SIGTERM; break;
                case SIGTERM: slavePtr->signal = SIGKILL; break;
                case SIGKILL: slavePtr->signal = -1;      break;
                }
            }
                            
            if (slavePtr->signal == -1
                || slavePtr->rfd == NS_INVALID_FD
                || WaitFd(slavePtr->rfd, POLLIN, 0)) {
                
                /*
                 * We either have timeouted eval (rfd==NS_INVALID_FD), a
                 * zombie or the process has exited ok so splice it out the
                 * list.
                 */
                if (slavePtr->signal >= 0) {
                    int waitStatus = 0;
                    
                    /*
                     * Pass waitStatus ptr to Ns_WaitForProcessStatus() to
                     * indicate that we want to handle the signal here and to
                     * suppress warning entries in the error.log.
                     *
                     * The following wait operation should not really wait.
                     */
                    (void) Ns_WaitForProcessStatus(slavePtr->pid, NULL, &waitStatus);
#ifdef WTERMSIG
                    if (slavePtr->signal != 0 && WTERMSIG(waitStatus) != 0) {
                        Ns_LogSeverity severity;

                        if (WTERMSIG(waitStatus) != slavePtr->signal) {
                            severity = Warning;
                        } else {
                            severity = Notice;
                        }
                        Ns_Log(severity, "nsproxy process %d killed with signal %d (%s)",
                               slavePtr->pid,
                               WTERMSIG(waitStatus), strsignal(WTERMSIG(waitStatus)));
                    }
#endif
                } else {
                    Ns_Log(Warning, "nsproxy: zombie: %ld", (long)slavePtr->pid);
                }
                if (prevSlavePtr != NULL) {
                    prevSlavePtr->nextPtr = slavePtr->nextPtr;
                } else {
                    firstClosePtr = slavePtr->nextPtr;
                }

                tmpSlavePtr = slavePtr->nextPtr;
                if (slavePtr->rfd != NS_INVALID_FD) {
                    ns_close(slavePtr->rfd);
                }
                ns_free(slavePtr);
                slavePtr = tmpSlavePtr;

            } else {

                /*
                 * Process is still around, try killing it but leave it
                 * in the list. Calculate the latest time we'll visit
                 * this one again.
                 */

                if (Ns_DiffTime(&slavePtr->expire, &tout, NULL) < 0) {
                    Ns_Log(Ns_LogNsProxyDebug, "reaper shortens timeout to %ld.%06ld based on expire in pool %s slave %ld kill %d",
                           tout.sec, tout.usec, slavePtr->poolPtr->name, (long)slavePtr->pid, slavePtr->signal);
                    tout = slavePtr->expire;
                }
                if (slavePtr->signal != slavePtr->sigsent) {
                    Ns_Log(Warning, "[%s]: pid %ld won't die, send signal %d",
                           slavePtr->poolPtr->name, (long)slavePtr->pid,
                           slavePtr->signal);
                    if (kill(slavePtr->pid, slavePtr->signal) != 0 && errno != ESRCH) {
                        Ns_Log(Error, "kill(%ld, %d) failed: %s",
                               (long)slavePtr->pid, slavePtr->signal, strerror(errno));
                    }
                    slavePtr->sigsent = slavePtr->signal;
                }
                prevSlavePtr = slavePtr;
                slavePtr = slavePtr->nextPtr;
            }
        }

        /*
         * Here we wait until signalled, or at most the
         * time we need to expire next slave or kill
         * some of them found on the close list.
         */

        if (Ns_DiffTime(&tout, &now, &diff) > 0) {
            reaperState = Sleeping;
            Ns_CondBroadcast(&pcond);
            if (tout.sec == TIME_T_MAX && tout.usec == 0) {
                Ns_Log(Ns_LogNsProxyDebug, "reaper waits unlimited for cond");
                Ns_CondWait(&pcond, &plock);
            } else {
                Ns_Log(Ns_LogNsProxyDebug, "reaper waits for cond with timeout %ld.%06ld",
                       tout.sec, tout.usec);
                (void) Ns_CondTimedWait(&pcond, &plock, &tout);
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

    Ns_DStringFree(&proxyPtr->in);
    Ns_DStringFree(&proxyPtr->out);
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
    if ((poolPtr->nused + poolPtr->nfree) <= poolPtr->maxslaves) {
        proxyPtr->nextPtr = poolPtr->firstPtr;
        poolPtr->firstPtr = proxyPtr;
        if (proxyPtr->slavePtr != NULL) {
            SetExpire(proxyPtr->slavePtr, proxyPtr->conf.tidle);
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
            result = Eval(interp, proxyPtr, Tcl_DStringValue(&ds), -1);
        }
        Tcl_DStringFree(&ds);

    } else if ( (proxyPtr->state == Busy) && (proxyPtr->slavePtr != NULL) ) {
        proxyPtr->slavePtr->signal = 0;
        Ns_Log(Notice, "releasing busy proxy %s", proxyPtr->id);

        /*
         * In case the proxy is busy, make sure to drain the pipe, otherwise
         * the proxy might be hanging in a send operation. Closing our end
         * causes in the slave an exception and terminates the potentially
         * blocking write operation.
         */
        ns_close(proxyPtr->slavePtr->rfd);
        proxyPtr->slavePtr->rfd = NS_INVALID_FD;
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
RunProxyCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    char       *scriptString;
    int         ms = -1, result = TCL_OK;
    Ns_ObjvSpec args[] = {
        {"script",    Ns_ObjvString, &scriptString, NULL},
        {"?timeout",  Ns_ObjvInt,    &ms,           NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Proxy *proxyPtr = (Proxy *)clientData;

        result = Eval(interp, proxyPtr, scriptString, ms);
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
 *      Returns time difference in miliseconds between current time
 *      and time given in passed structure. If the current time is
 *      later then the passed time, the result is negative.
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
GetTimeDiff(Ns_Time *timePtr)
{
    Ns_Time now, diff;

    NS_NONNULL_ASSERT(timePtr != NULL);

    Ns_GetTime(&now);
    return Ns_DiffTime(timePtr, &now, &diff) * (diff.sec/1000 + diff.usec*1000);
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
