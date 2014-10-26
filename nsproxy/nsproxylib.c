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

#include <grp.h>
#include <poll.h>

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
    char          *name;     /* Name of pool */
    struct Proxy  *firstPtr; /* First in list of avail proxies */
    struct Proxy  *runPtr;   /* First in list of running proxies */
    char          *exec;     /* Slave executable */
    char          *init;     /* Init script to eval on proxy start */
    char          *reinit;   /* Re-init scripts to eval on proxy put */
    int            waiting;  /* Thread waiting for handles */
    int            maxslaves;/* Max number of allowed proxies */
    int            nfree;    /* Current number of available proxy handles */
    int            nused;    /* Current number of used proxy handles */
    int            nextid;   /* Next in proxy unique ids */
    ProxyConf      conf;     /* Collection of config options to pass to proxy */
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

static Pool*  GetPool(char *poolName, InterpData *idataPtr);
static void   FreePool(Pool *poolPtr);

static Proxy* CreateProxy(Pool *poolPtr);
static Err    PopProxy(Pool *poolPtr, Proxy **proxyPtrPtr, int nwant, int ms);
static void   PushProxy(Proxy *proxyPtr);
static Proxy* GetProxy(Tcl_Interp *interp, char *proxyId, InterpData *idataPtr);

static int    Eval(Tcl_Interp *interp, Proxy *proxyPtr, char *script, int ms);

static Err    Send(Tcl_Interp *interp, Proxy *proxyPtr, char *script);
static Err    Wait(Tcl_Interp *interp, Proxy *proxyPtr, int ms);
static Err    Recv(Tcl_Interp *interp, Proxy *proxyPtr, int *resultPtr);

static Err    CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr);
static int    ReleaseProxy(Tcl_Interp *interp, Proxy *proxyPtr);
static void   CloseProxy(Proxy *proxyPtr);
static void   FreeProxy(Proxy *proxyPtr);
static void   ResetProxy(Proxy *proxyPtr);
static char*  ProxyError(Tcl_Interp *interp, Err err);
static void   FmtActiveProxy(Tcl_Interp *interp, Proxy *proxyPtr);

static void   ReleaseHandles(Tcl_Interp *interp, InterpData *idataPtr);
static Slave* ExecSlave(Tcl_Interp *interp, Proxy *proxyPtr);
static Err    CreateSlave(Tcl_Interp *interp, Proxy *proxyPtr);

static void   SetExpire(Slave *slavePtr, int ms);
static int    SendBuf(Slave *slavePtr, int ms, Tcl_DString *dsPtr);
static int    RecvBuf(Slave *slavePtr, int ms, Tcl_DString *dsPtr);
static int    WaitFd(int fd, int events, int ms);

static int    Import(Tcl_Interp *interp, Tcl_DString *dsPtr, int *resultPtr);
static void   Export(Tcl_Interp *interp, int code, Tcl_DString *dsPtr);

static void   UpdateIov(struct iovec *iov, int n);
static void   SetOpt(char *str, char **optPtr);
static void   ReaperThread(void *ignored);
static void   CloseSlave(Slave *slavePtr, int ms);
static void   ReapProxies(void);
static void   Kill(pid_t pid, int sig);
static int    GetTimeDiff(Ns_Time *tsPtr);

static void   AppendStr(Tcl_Interp *interp, CONST char *flag, char *val);
static void   AppendInt(Tcl_Interp *interp, CONST char *flag, int i);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable pools;     /* Tracks proxy pools */

ReaperState reaperState = Stopped;

static Ns_Cond  pcond;          /* Those are used to control access to */
static Ns_Mutex plock;          /* the list of Slave structures of slave */
static Slave    *firstClosePtr;  /* processes which are being closed. */

static Ns_DString defexec;      /* Stores full path of the proxy executable */


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
    static int once = 0;

    if (!once) {
        once = 1;

        Nsd_LibInit();

        Ns_DStringInit(&defexec);
        Ns_BinPath(&defexec, "nsproxy", NULL);
        Tcl_InitHashTable(&pools, TCL_STRING_KEYS);

        Ns_RegisterAtShutdown(Shutdown, NULL);
        Ns_RegisterProcInfo((Ns_Callback *)Shutdown, "nsproxy:shutdown", NULL);
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

    idataPtr = ns_calloc(1U, sizeof(InterpData));
    Tcl_InitHashTable(&idataPtr->ids, TCL_STRING_KEYS);
    Tcl_InitHashTable(&idataPtr->cnts, TCL_ONE_WORD_KEYS);
    Tcl_SetAssocData(interp, ASSOC_DATA, DeleteData, idataPtr);
    Tcl_CreateObjCommand(interp, "ns_proxy", ProxyObjCmd, idataPtr, NULL);

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
    Slave         proc;
    int          result, n, max = 0;
    Tcl_DString  in, out;
    char        *script, *active, *dots;
    char        *uarg = NULL, *user = NULL, *group = NULL;
    uint16       major, minor;

    Nsproxy_LibInit();

    if (argc > 4 || argc < 3) {
        char *pgm = strrchr(argv[0], '/');
        Ns_Fatal("usage: %s pool id ?command?", pgm ? ++pgm : argv[0]);
    }
    if (argc < 4) {
        active = NULL;
    } else {
        active = argv[3];
        max = strlen(active) - 8;
        if (max < 0) {
            active = NULL;
        }
    }

    /*
     * Move the proxy input and output fd's from 0 and 1 to avoid
     * protocal errors with scripts accessing stdin and stdout.
     * Stdin is open on /dev/null and stdout is dup'ed to stderr.
     */

    major = htons(MAJOR_VERSION);
    minor = htons(MINOR_VERSION);
    proc.pid = NS_INVALID_PID;

    proc.rfd = dup(0);
    if (proc.rfd < 0) {
        Ns_Fatal("nsproxy: dup: %s", strerror(errno));
    }
    proc.wfd = dup(1);
    if (proc.wfd < 0) {
        Ns_Fatal("nsproxy: dup: %s", strerror(errno));
    }
    close(0);
    if (open("/dev/null", O_RDONLY) != 0) {
        Ns_Fatal("nsproxy: open: %s", strerror(errno));
    }
    close(1);
    if (dup(2) != 1) {
        Ns_Fatal("nsproxy: dup: %s", strerror(errno));
    }

    /*
     * Make sure possible child processes do not inherit this one.
     * As, when the user evalutes the "exec" command, the child
     * process(es) will otherwise inherit the descriptor and keep
     * it open even if the proxy process is killed in the meantime.
     * This will of course block the caller, possibly forever.
     */

    Ns_CloseOnExec(proc.wfd);

    /*
     * Create the interp, initialize with user init proc, if any.
     */

    Tcl_FindExecutable(argv[0]);
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

    user = strchr(argv[1], ':');
    if (user != NULL) {
        uarg = ns_strdup(++user);
        user = uarg;
        group = strchr(user, ':');
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

    while (RecvBuf(&proc, -1, &in)) {
        Req *reqPtr;
	int  len;

        if (Tcl_DStringLength(&in) < sizeof(Req)) {
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
                n = len;
                if (n < max) {
                    dots = "";
                } else {
                    dots = " ...";
                    n = max;
                }
                sprintf(active, "{%.*s%s}", n, script, dots);
            }
            result = Tcl_EvalEx(interp, script, len, 0);
            Export(interp, result, &out);
            if (active != NULL) {
                memset(active, ' ', max);
            }
        } else {
            Ns_Fatal("nsproxy: invalid length");
        }
        if (!SendBuf(&proc, -1, &out)) {
            break;
        }
        Tcl_DStringTrunc(&in, 0);
        Tcl_DStringTrunc(&out, 0);
    }

    if (uarg) {
        ns_free(uarg);
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
Ns_ProxyCleanup(Tcl_Interp *interp, void *ignored)
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
Shutdown(const Ns_Time *toutPtr, void *arg)
{
    Pool           *poolPtr;
    Proxy          *proxyPtr, *tmpPtr;
    Tcl_HashSearch  search;
    int             reap, status;

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
                if (proxyPtr->slavePtr) {
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

    if (!reap) {
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
Ns_ProxyGet(Tcl_Interp *interp, char *poolName, PROXY* handlePtr, int ms)
{
    Pool  *poolPtr;
    Proxy *proxyPtr;
    Err    err;

    /*
     * Get just one proxy from the pool
     */

    poolPtr = GetPool(poolName, NULL);
    err = PopProxy(poolPtr, &proxyPtr, 1, ms);
    if (err) {
        Tcl_AppendResult(interp, "could not allocate from pool \"",
                         poolPtr->name, "\": ", ProxyError(interp, err), NULL);
        return TCL_ERROR;
    }

    /*
     * Check proxy for valid connection.
     */

    if (CheckProxy(interp, proxyPtr) != ENone) {
        PushProxy(proxyPtr);
        Ns_CondBroadcast(&poolPtr->cond);
        return TCL_ERROR;
    }

    *handlePtr = (PROXY *)proxyPtr;

    return TCL_OK;
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

int Ns_ProxyEval(Tcl_Interp *interp, PROXY handle, char *script, int ms)
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
    Pool  *poolPtr = proxyPtr->poolPtr;
    char  *argv[5], active[100];
    Slave *slavePtr;
    int    rpipe[2], wpipe[2], len;
    pid_t  pid;

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
        Tcl_AppendResult(interp, "pipe failed: ", Tcl_PosixError(interp), NULL);
        return NULL;
    }
    if (ns_pipe(wpipe) != 0) {
        Tcl_AppendResult(interp, "pipe failed: ", Tcl_PosixError(interp), NULL);
        close(rpipe[0]);
        close(rpipe[1]);
        return NULL;
    }

    pid = Ns_ExecArgv(poolPtr->exec, NULL, rpipe[0], wpipe[1], argv, NULL);

    close(rpipe[0]);
    close(wpipe[1]);

    ns_free(argv[0]);
    ns_free(argv[1]);

    if (pid == NS_INVALID_PID) {
        Tcl_AppendResult(interp, "exec failed: ", Tcl_PosixError(interp), NULL);
        close(wpipe[0]);
        close(rpipe[1]);
        return NULL;
    }

    slavePtr = ns_calloc(1U, sizeof(Slave));
    slavePtr->poolPtr = proxyPtr->poolPtr;
    slavePtr->pid = pid;
    slavePtr->rfd = wpipe[0];
    slavePtr->wfd = rpipe[1];

    SetExpire(slavePtr, proxyPtr->conf.tidle);

    Ns_Log(Debug, "nsproxy: slave %ld started", (long) slavePtr->pid);

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
    if (ms > 0) {
        Ns_GetTime(&slavePtr->expire);
        Ns_IncrTime(&slavePtr->expire, ms/1000, (ms%1000) * 1000);
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
Eval(Tcl_Interp *interp, Proxy *proxyPtr, char *script, int ms)
{
    Err err;
    int status = TCL_ERROR;

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
Send(Tcl_Interp *interp, Proxy *proxyPtr, char *script)
{
    Err err = ENone;
    Req req;

    if (proxyPtr->slavePtr == NULL) {
        err = EDead;
    } else if (proxyPtr->state != Idle) {
        err = EBusy;
    } else {
        proxyPtr->numruns++;
        if (proxyPtr->conf.maxruns > 0
            && proxyPtr->numruns > proxyPtr->conf.maxruns) {
            CloseProxy(proxyPtr);
            err = CreateSlave(interp, proxyPtr);
        }
        if (err == ENone) {
            int len = script ? strlen(script) : 0;
            req.len   = htonl(len);
            req.major = htons(MAJOR_VERSION);
            req.minor = htons(MINOR_VERSION);
            Tcl_DStringTrunc(&proxyPtr->in, 0);
            Tcl_DStringAppend(&proxyPtr->in, (char *) &req, sizeof(req));
            if (len > 0) {
                Tcl_DStringAppend(&proxyPtr->in, script, len);
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

            if (!SendBuf(proxyPtr->slavePtr, proxyPtr->conf.tsend,
                         &proxyPtr->in)) {
                err = ESend;
            }
        }
    }

    if (err != ENone) {
        Tcl_AppendResult(interp, "could not send script \"",
                         script ? script : "<empty>",
                         "\" to proxy \"", proxyPtr->id, "\": ",
                         ProxyError(interp, err), NULL);
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
        Tcl_AppendResult(interp, "could not wait for proxy \"",
                         proxyPtr->id, "\": ",
                         ProxyError(interp, err), NULL);
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

    if (proxyPtr->state == Idle) {
        err = EIdle;
    } else if (proxyPtr->state == Busy) {
        err = ENoWait;
    } else {
        Tcl_DStringTrunc(&proxyPtr->out, 0);
        if (!RecvBuf(proxyPtr->slavePtr, proxyPtr->conf.trecv,
                     &proxyPtr->out)) {
            err = ERecv;
        } else if (Import(interp, &proxyPtr->out, resultPtr) != TCL_OK) {
            err = EImport;
        } else {
            proxyPtr->state = Idle;
        }
        ResetProxy(proxyPtr);
    }

    if (err != ENone) {
        Tcl_AppendResult(interp, "could not receive from proxy \"",
                         proxyPtr->id, "\": ",
                         ProxyError(interp, err), NULL);
    }

    return err;
}

/*
 *----------------------------------------------------------------------
 *
 * SendBuf --
 *
 *      Send a dstring buffer.
 *
 * Results:
 *      1 if sent, 0 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SendBuf(Slave *slavePtr, int msec, Tcl_DString *dsPtr)
{
    int          n, ms;
    uint32       ulen;
    struct iovec iov[2];
    Ns_Time      end;

    if (msec > 0) {
        Ns_GetTime(&end);
        Ns_IncrTime(&end, msec/1000, (msec % 1000) * 1000);
    }

    ulen = htonl(dsPtr->length);
    iov[0].iov_base = (caddr_t) &ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = dsPtr->length;
    while ((iov[0].iov_len + iov[1].iov_len) > 0) {
        do {
            n = writev(slavePtr->wfd, iov, 2);
        } while (n == -1 && errno == EINTR);
        if (n == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return 0;
            }
            if (msec > 0) {
                ms = GetTimeDiff(&end);
                if (ms < 0) {
                    return 0;
                }
            } else {
                ms = msec;
            }
            if (WaitFd(slavePtr->wfd, POLLOUT, ms) == 0) {
                return 0;
            }
        } else if (n > 0) {
            UpdateIov(iov, n);
        }
    }

    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * RecvBuf --
 *
 *      Receive a dstring buffer.
 *
 * Results:
 *      1 if received, 0 on error.
 *
 * Side effects:
 *      Will resize output dstring as needed.
 *
 *----------------------------------------------------------------------
 */

static int
RecvBuf(Slave *slavePtr, int msec, Tcl_DString *dsPtr)
{
    uint32       ulen;
    char        *ptr;
    int          n, len, avail, ms;
    struct iovec iov[2];
    Ns_Time      end;

    if (msec > 0) {
        Ns_GetTime(&end);
        Ns_IncrTime(&end, msec/1000, (msec % 1000) * 1000);
    }

    avail = dsPtr->spaceAvl - 1;
    iov[0].iov_base = (caddr_t) &ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = avail;
    while (iov[0].iov_len > 0) {
        do {
            n = readv(slavePtr->rfd, iov, 2);
        } while (n == -1 && errno == EINTR);
        if (n == 0) {
            return 0; /* EOF */
        } else if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return 0;
            }
            if (msec > 0) {
                ms = GetTimeDiff(&end);
                if (ms < 0) {
                    return 0;
                }
            } else {
                ms = msec;
            }
            if (WaitFd(slavePtr->rfd, POLLIN, ms) == 0) {
                return 0;
            }
        } else if (n > 0) {
            UpdateIov(iov, n);
        }
    }
    n = avail - iov[1].iov_len;
    Tcl_DStringSetLength(dsPtr, n);
    len = ntohl(ulen);
    Tcl_DStringSetLength(dsPtr, len);
    len -= n;
    ptr  = dsPtr->string + n;
    while (len > 0) {
        do {
            n = read(slavePtr->rfd, ptr, len);
        } while (n == -1 && errno == EINTR);
        if (n == 0) {
            return 0; /* EOF */
        } else if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return 0;
            }
            if (msec > 0) {
                ms = GetTimeDiff(&end);
                if (ms < 0) {
                    return 0;
                }
            } else {
                ms = msec;
            }
            if (WaitFd(slavePtr->rfd, POLLIN, ms) == 0) {
                return 0;
            }
        } else if (n > 0) {
            len -= n;
            ptr += n;
        }
    }

    return 1;
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
WaitFd(int fd, int event, int ms)
{
    struct pollfd pfd;
    int n;

    pfd.fd = fd;
    pfd.events = event | POLLPRI | POLLERR;
    pfd.revents = pfd.events;
    do {
        n = ns_poll(&pfd, 1, ms);
    } while (n == -1 && errno == EINTR);
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
UpdateIov(struct iovec *iov, int n)
{
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
    Res   hdr;
    char *einfo = NULL, *ecode = NULL, *result = NULL;
    int   clen = 0, ilen = 0, rlen = 0;

    if (interp != NULL) {
        if (code == TCL_OK) {
            einfo = NULL;
            ecode = NULL;
        } else {
            ecode = (char *)Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
            einfo = (char *)Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
        }
        clen = ecode ? (strlen(ecode) + 1) : 0;
        ilen = einfo ? (strlen(einfo) + 1) : 0;
        result = (char *)Tcl_GetStringResult(interp);
        rlen = strlen(result);
    }
    hdr.code = htonl(code);
    hdr.clen = htonl(clen);
    hdr.ilen = htonl(ilen);
    hdr.rlen = htonl(rlen);
    Tcl_DStringAppend(dsPtr, (char *) &hdr, sizeof(hdr));
    if (clen > 0) {
        Tcl_DStringAppend(dsPtr, ecode, clen);
    }
    if (ilen > 0) {
        Tcl_DStringAppend(dsPtr, einfo, ilen);
    }
    if (rlen > 0) {
        Tcl_DStringAppend(dsPtr, result, rlen);
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
    Res  *resPtr;
    char *str;
    int   rlen, clen, ilen;

    if (dsPtr->length < sizeof(Res)) {
        return TCL_ERROR;
    }

    resPtr = (Res *) dsPtr->string;
    str = dsPtr->string + sizeof(Res);
    clen = ntohl(resPtr->clen);
    ilen = ntohl(resPtr->ilen);
    rlen = ntohl(resPtr->rlen);
    if (clen > 0) {
        Tcl_SetErrorCode(interp, str, NULL);
        str += clen;
    }
    if (ilen > 0) {
        Tcl_AddErrorInfo(interp, str);
        str += ilen;
    }
    if (rlen > 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(str, -1));
    }
    *resultPtr = ntohl(resPtr->code);

    return TCL_OK;
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
    Pool          *poolPtr, *thePoolPtr;
    Proxy         *proxyPtr;
    Err            err;
    int            ms, reap, opt, result = TCL_OK;
    char          *proxyId;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    static CONST char *opts[] = {
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
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        proxyPtr = GetProxy(interp, proxyId, idataPtr);
        if (proxyPtr == NULL) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        if (opt == PPutIdx || opt == PReleaseIdx) {
            result = ReleaseProxy(interp, proxyPtr);
        } else {
            result = Eval(interp, proxyPtr, NULL, -1);
        }
        break;

    case PConfigureIdx:
        result = ConfigureObjCmd(data, interp, objc, objv);
        break;

    case PCleanupIdx:
        if (objc != 2) {
            Tcl_WrongNumArgs(interp, 2, objv, NULL);
            return TCL_ERROR;
        }
        ReleaseHandles(interp, idataPtr);
        break;

    case PGetIdx:
        result = GetObjCmd(data, interp, objc, objv);
        break;

    case PSendIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle script");
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        proxyPtr = GetProxy(interp, proxyId, idataPtr);
        if (proxyPtr == NULL) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        err = Send(interp, proxyPtr, Tcl_GetString(objv[3]));
        result = (err == ENone) ? TCL_OK : TCL_ERROR;
        break;

    case PWaitIdx:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle ?timeout?");
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        proxyPtr = GetProxy(interp, proxyId, idataPtr);
        if (proxyPtr == NULL) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        if (objc == 3) {
            ms = -1;
        } else if (Tcl_GetIntFromObj(interp, objv[3], &ms) != TCL_OK) {
            return TCL_ERROR;
        }
        err = Wait(interp, proxyPtr, ms);
        result = (err == ENone) ? TCL_OK : TCL_ERROR;
        break;

    case PRecvIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle");
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        proxyPtr = GetProxy(interp, proxyId, idataPtr);
        if (proxyPtr == NULL) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        err = Recv(interp, proxyPtr, &result);
        result = (err == ENone) ? result : TCL_ERROR;
        break;

    case PEvalIdx:
        if (objc != 4 && objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle script");
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        proxyPtr = GetProxy(interp, proxyId, idataPtr);
        if (proxyPtr == NULL) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        if (objc == 4) {
            ms = -1;
        } else if (Tcl_GetIntFromObj(interp, objv[4], &ms) != TCL_OK) {
            return TCL_ERROR;
        }
        result = Eval(interp, proxyPtr, Tcl_GetString(objv[3]), ms);
        break;

    case PFreeIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool");
            return TCL_ERROR;
        }
        poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
        Ns_MutexLock(&poolPtr->lock);
        proxyPtr = poolPtr->firstPtr;
        while (proxyPtr != NULL) {
            Tcl_AppendElement(interp, proxyPtr->id);
            proxyPtr = proxyPtr->nextPtr;
        }
        Ns_MutexUnlock(&poolPtr->lock);
        break;

    case PHandlesIdx:
        if (objc == 3) {
            poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
        } else {
            poolPtr = NULL;
        }
        hPtr = Tcl_FirstHashEntry(&idataPtr->ids, &search);
        while (hPtr != NULL) {
            proxyPtr = (Proxy *)Tcl_GetHashValue(hPtr);
            if (poolPtr == NULL || poolPtr == proxyPtr->poolPtr) {
                Tcl_AppendElement(interp, proxyPtr->id);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        break;

    case PActiveIdx:
        if (objc < 3 || objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool ?handle?");
            return TCL_ERROR;
        }
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
        break;

    case PStopIdx:
    case PClearIdx:
        if (objc < 3 || objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool ?handle?");
            return TCL_ERROR;
        }
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
                        if (proxyPtr->slavePtr) {
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
        if (reap) {
            ReapProxies();
        }
        break;


    case PPoolsIdx:
        Ns_MutexLock(&plock);
        hPtr = Tcl_FirstHashEntry(&pools, &search);
        while (hPtr != NULL) {
            poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
            Tcl_AppendElement(interp, poolPtr->name);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&plock);
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
    int         flag, n, result, reap = 0;

    static CONST char *flags[] = {
        "-init", "-reinit", "-maxslaves", "-exec",
        "-gettimeout", "-evaltimeout", "-sendtimeout", "-recvtimeout",
        "-waittimeout", "-idletimeout", "-maxruns", NULL
    };
    enum {
        CInitIdx, CReinitIdx, CMaxslaveIdx, CExecIdx, CGetIdx,
        CEvalIdx, CSendIdx, CRecvIdx, CWaitIdx, CIdleIdx, CMaxrunsIdx
    };

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "pool ?opt? ?val? ?opt val?...");
        return TCL_ERROR;
    }
    result = TCL_ERROR;
    poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
    Ns_MutexLock(&poolPtr->lock);
    if (objc == 4) {
        if (Tcl_GetIndexFromObj(interp, objv[3], flags, "flags", 0,
                                &flag) != TCL_OK) {
            goto err;
        }
    } else if (objc > 4) {
        int   i;
	char *str;

        for (i = 3; i < (objc - 1); ++i) {
            if (Tcl_GetIndexFromObj(interp, objv[i], flags, "flags", 0,
                                    &flag)) {
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
                    goto err;
                }
                if (n < 0) {
                    Tcl_AppendResult(interp, "invalid ", flags[flag], ": ",
                                     str, NULL);
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
                        if (proxyPtr->slavePtr) {
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

    if (objc == 3) {
        AppendStr(interp, flags[CExecIdx],     poolPtr->exec);
        AppendStr(interp, flags[CInitIdx],     poolPtr->init);
        AppendStr(interp, flags[CReinitIdx],   poolPtr->reinit);
        AppendInt(interp, flags[CMaxslaveIdx], poolPtr->maxslaves);
        AppendInt(interp, flags[CMaxrunsIdx],  poolPtr->conf.maxruns);
        AppendInt(interp, flags[CGetIdx],      poolPtr->conf.tget);
        AppendInt(interp, flags[CEvalIdx],     poolPtr->conf.teval);
        AppendInt(interp, flags[CSendIdx],     poolPtr->conf.tsend);
        AppendInt(interp, flags[CRecvIdx],     poolPtr->conf.trecv);
        AppendInt(interp, flags[CWaitIdx],     poolPtr->conf.twait);
        AppendInt(interp, flags[CIdleIdx],     poolPtr->conf.tidle);
    } else if (objc == 4) {
        switch (flag) {
        case CExecIdx:
            AppendStr(interp, NULL, poolPtr->exec);
            break;
        case CInitIdx:
            AppendStr(interp, NULL, poolPtr->init);
            break;
        case CReinitIdx:
            AppendStr(interp, NULL, poolPtr->reinit);
            break;
        case CMaxslaveIdx:
            AppendInt(interp, NULL, poolPtr->maxslaves);
            break;
        case CMaxrunsIdx:
            AppendInt(interp, NULL, poolPtr->conf.maxruns);
            break;
        case CGetIdx:
            AppendInt(interp, NULL, poolPtr->conf.tget);
            break;
        case CEvalIdx:
            AppendInt(interp, NULL, poolPtr->conf.teval);
            break;
        case CSendIdx:
            AppendInt(interp, NULL, poolPtr->conf.tsend);
            break;
        case CRecvIdx:
            AppendInt(interp, NULL, poolPtr->conf.trecv);
            break;
        case CWaitIdx:
            AppendInt(interp, NULL, poolPtr->conf.twait);
            break;
        case CIdleIdx:
            AppendInt(interp, NULL, poolPtr->conf.tidle);
            break;
        }
    } else if (objc == 5) {
        Tcl_SetObjResult(interp, objv[4]);
    }
    result = TCL_OK;

 err:
    Ns_MutexUnlock(&poolPtr->lock);

    /*
     * Optionally, wake up reaper thread
     * to collect closing proxies or to
     * enforce pool size constraints.
     */

    if (reap) {
        ReapProxies();
    }

    return result;
}


static void
SetOpt(char *str, char **optPtr)
{
    if (*optPtr != NULL) {
        ns_free(*optPtr);
    }
    if (str != NULL && *str != '\0') {
        *optPtr = ns_strdup(str);
    } else {
        *optPtr = NULL;
    }
}

static void
AppendInt(Tcl_Interp *interp, CONST char *flag, int i)
{
    char buf[TCL_INTEGER_SPACE];

    snprintf(buf, sizeof(buf), "%d", i);
    AppendStr(interp, flag, buf);
}

static void
AppendStr(Tcl_Interp *interp, CONST char *flag, char *val)
{
    if (flag != NULL) {
        Tcl_AppendElement(interp, (char *)flag);
        Tcl_AppendElement(interp, val ? val : "");
    } else {
        Tcl_AppendResult(interp, val ? val : "", NULL);
    }
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
    int            i, flag, isNew, nwant, n, ms;
    char          *arg;
    Err            err;
    Pool          *poolPtr;

    static CONST char *flags[] = {
        "-timeout", "-handles", NULL
    };
    enum {
        FTimeoutIdx, FHandlesIdx
    };

    if (objc < 3 || (objc % 2) != 1) {
        Tcl_WrongNumArgs(interp, 2, objv, "pool ?-opt val -opt val ...?");
        return TCL_ERROR;
    }
    poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
    assert(idataPtr);
    cntPtr = Tcl_CreateHashEntry(&idataPtr->cnts, (char *) poolPtr, &isNew);
    if ((intptr_t) Tcl_GetHashValue(cntPtr) > 0) {
        err = EDeadlock;
        goto errout;
    }

    nwant = 1;

    Ns_MutexLock(&poolPtr->lock);
    ms = poolPtr->conf.tget;
    Ns_MutexUnlock(&poolPtr->lock);

    for (i = 3; i < objc; ++i) {
        arg = Tcl_GetString(objv[2]);
        if (Tcl_GetIndexFromObj(interp, objv[i], flags, "flags", 0,
                                &flag)) {
            return TCL_ERROR;
        }
        ++i;
        if (Tcl_GetIntFromObj(interp, objv[i], &n) != TCL_OK) {
            return TCL_ERROR;
        }
        if (n < 0) {
            Tcl_AppendResult(interp, "invalid ", flags[flag], ": ", arg, NULL);
            return TCL_ERROR;
        }
        switch (flag) {
        case FTimeoutIdx:
            ms = n;
            break;
        case FHandlesIdx:
            nwant = n;
            break;
        }
    }

    /*
     * Get some number of proxies from the pool
     */

    err = PopProxy(poolPtr, &firstPtr, nwant, ms);
    if (err) {
    errout:
        Tcl_AppendResult(interp, "could not allocate from pool \"",
                         poolPtr->name, "\": ", ProxyError(interp, err), NULL);
        return TCL_ERROR;
    }

    /*
     * Set total owned count and create handle ids.
     */

    Tcl_SetHashValue(cntPtr, (ClientData)(intptr_t) nwant);
    proxyPtr = firstPtr;
    while (proxyPtr != NULL) {
        idPtr = Tcl_CreateHashEntry(&idataPtr->ids, proxyPtr->id, &isNew);
        if (!isNew) {
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
        return TCL_ERROR;
    }

    /*
     * Generate accessor commands for the returned proxies.
     */

    proxyPtr = firstPtr;
    Tcl_ResetResult(interp);
    while (proxyPtr != NULL) {
        proxyPtr->cmdToken = Tcl_CreateObjCommand(interp, proxyPtr->id,
                                                  RunProxyCmd, proxyPtr,
                                                  DelProxyCmd);
        if (proxyPtr->cmdToken == NULL) {
            return TCL_ERROR;
        }
        proxyPtr->interp = interp;
        Tcl_AppendElement(interp, proxyPtr->id);
        proxyPtr = proxyPtr->nextPtr;
    }

    return TCL_OK;
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
    Proxy   *proxyPtr;
    Err      err;
    int      status = NS_OK;
    Ns_Time  tout;

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

    Tcl_DStringInit(&ds);

    Tcl_DStringGetResult(interp, &ds);

    Tcl_DStringStartSublist(&ds);
    Ns_DStringPrintf(&ds, "handle %s slave %ld start %" PRIu64 ":%ld script",
                     proxyPtr->id,
                     (long) (proxyPtr->slavePtr ? proxyPtr->slavePtr->pid : 0),
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
GetPool(char *poolName, InterpData *idataPtr)
{
    Tcl_HashEntry *hPtr;
    Pool          *poolPtr;
    Proxy         *proxyPtr;
    int            isNew;

    Ns_MutexLock(&plock);
    hPtr = Tcl_CreateHashEntry(&pools, poolName, &isNew);
    if (!isNew) {
        poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
    } else {
        char *path = NULL, *exec = NULL;
        int i;

        poolPtr = ns_calloc(1U, sizeof(Pool));
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
    char buf[32];

    sprintf(buf, "%d", poolPtr->nextid++);

    proxyPtr = ns_calloc(1U, sizeof(Proxy));
    proxyPtr->id = ns_calloc(1U, strlen(buf) + strlen(poolPtr->name) + 2);

    strcpy(proxyPtr->id, poolPtr->name);
    strcat(proxyPtr->id, "-");
    strcat(proxyPtr->id, buf);
    proxyPtr->poolPtr = poolPtr;

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
GetProxy(Tcl_Interp *interp, char *proxyId, InterpData *idataPtr)
{
    Tcl_HashEntry *hPtr;

    hPtr = Tcl_FindHashEntry(&idataPtr->ids, proxyId);
    if (hPtr) {
        return (Proxy *)Tcl_GetHashValue(hPtr);
    }

    return NULL;
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

    if (proxyPtr->slavePtr && Eval(interp, proxyPtr, NULL, -1) != TCL_OK) {
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
    Pool        *poolPtr = proxyPtr->poolPtr;
    Err          err = ENone;
    int          init;
    Tcl_DString  ds;

    Tcl_DStringInit(&ds);
    Ns_MutexLock(&poolPtr->lock);
    init = proxyPtr->poolPtr->init != NULL;
    if (init) {
        Tcl_DStringAppend(&ds, poolPtr->init, -1);
    }
    Ns_MutexUnlock(&poolPtr->lock);
    proxyPtr->slavePtr = ExecSlave(interp, proxyPtr);
    if (proxyPtr->slavePtr == NULL) {
        err = EExec;
    } else if (init && Eval(interp, proxyPtr,
                            Tcl_DStringValue(&ds), -1) != TCL_OK) {
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
    Pool  *poolPtr = proxyPtr->poolPtr;
    Proxy *runPtr, *prevPtr;

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

    close(slavePtr->wfd);
    slavePtr->signal  = 0;
    slavePtr->sigsent = 0;

    /*
     * Put on the head of the close list so it's handled by
     * the reaper thread.
     */

    slavePtr->nextPtr = firstClosePtr;
    firstClosePtr = slavePtr;

    Ns_Log(Debug, "nsproxy: slave %ld closed", (long) slavePtr->pid);
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
 * Kill --
 *
 *      Kill the slave with the given signal.
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
Kill(pid_t pid, int sig)
{
    if (kill(pid, sig) != 0 && errno != ESRCH) {
        Ns_Log(Error, "kill(%ld, %d) failed: %s", (long)pid, sig, strerror(errno));
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
ReaperThread(void *ignored)
{
    Tcl_HashSearch  search;
    Proxy          *proxyPtr, *prevPtr, *nextPtr;
    Pool           *poolPtr;
    Slave           *slavePtr, *tmpSlavePtr;
    Ns_Time         tout, now, diff;
    int             ms, expire, ntotal;

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
            Ns_MutexLock(&poolPtr->lock);
            if (poolPtr->conf.tidle) {
                diff = now;
                ms = poolPtr->conf.tidle;
                Ns_IncrTime(&diff, ms/1000, (ms%1000) * 1000);
                if (Ns_DiffTime(&diff, &tout, NULL) < 0) {
                    tout = diff;
                }
            }

            /*
             * Get max time to wait for one of the slaves.
             * This is less then time for the whole pool.
             */

            proxyPtr = poolPtr->firstPtr;
            prevPtr = NULL;
            expire = 0;
            while (proxyPtr != NULL) {
                nextPtr = proxyPtr->nextPtr;
                slavePtr = proxyPtr->slavePtr;
                ntotal  = poolPtr->nfree + poolPtr->nused;
                if (slavePtr) {
                    if (Ns_DiffTime(&slavePtr->expire, &tout, NULL) <= 0) {
                        tout = slavePtr->expire;
                    }
                    expire |= Ns_DiffTime(&slavePtr->expire, &now, NULL) <= 0;
                } else {
                    expire = 0;
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
                    if (slavePtr) {
                        CloseSlave(slavePtr, proxyPtr->conf.twait);
                    }
                    FreeProxy(proxyPtr);
                    proxyPtr = NULL;
                    poolPtr->nfree--;
                } else if (expire) {
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
            if (slavePtr->signal == -1 || WaitFd(slavePtr->rfd, POLLIN, 0)) {

                /*
                 * We either have a zombie or the process has exited ok
                 * so splice it out the list.
                 */

                if (slavePtr->signal >= 0) {
                    Ns_WaitProcess(slavePtr->pid); /* Should not really wait */
                } else {
                    Ns_Log(Warning, "nsproxy: zombie: %ld", (long)slavePtr->pid);
                }
                if (prevSlavePtr != NULL) {
                    prevSlavePtr->nextPtr = slavePtr->nextPtr;
                } else {
                    firstClosePtr = slavePtr->nextPtr;
                }

                tmpSlavePtr = slavePtr->nextPtr;
                close(slavePtr->rfd);
                ns_free(slavePtr);
                slavePtr = tmpSlavePtr;

            } else {

                /*
                 * Process is still arround, try killing it but leave it
                 * in the list. Calculate the latest time we'll visit
                 * this one again.
                 */

                if (Ns_DiffTime(&slavePtr->expire, &tout, NULL) < 0) {
                    tout = slavePtr->expire;
                }
                if (slavePtr->signal != slavePtr->sigsent) {
                    Ns_Log(Warning, "[%s]: pid %ld won't die, send signal %d",
                           slavePtr->poolPtr->name, (long)slavePtr->pid,
                           (int)slavePtr->signal);
                    Kill(slavePtr->pid, slavePtr->signal);
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
            if (tout.sec == TIME_T_MAX && tout.usec == LONG_MAX) {
                Ns_CondWait(&pcond, &plock);
            } else {
                Ns_CondTimedWait(&pcond, &plock, &tout);
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
    if (poolPtr->exec) {
        ns_free(poolPtr->exec);
    }
    if (poolPtr->init) {
        ns_free(poolPtr->init);
    }
    if (poolPtr->reinit) {
        ns_free(poolPtr->reinit);
    }

    Ns_CondDestroy(&poolPtr->cond);
    Ns_MutexDestroy(&poolPtr->lock);

    ns_free(poolPtr);
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
    Pool     *poolPtr = proxyPtr->poolPtr;
    
    /*
     * Clears the proxy for the next use
     */

    ResetProxy(proxyPtr);

    /*
     * Divorce from the per-interpreter tables
     */

    if (proxyPtr->cntPtr) {
        intptr_t  nhave = (intptr_t) Tcl_GetHashValue(proxyPtr->cntPtr);

        nhave--;
        Tcl_SetHashValue(proxyPtr->cntPtr, (ClientData) nhave);
        if (proxyPtr->idPtr) {
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
        if (proxyPtr->slavePtr) {
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

    if (proxyPtr->state == Idle) {
        Tcl_DString ds;
        int reinit;
        Tcl_DStringInit(&ds);
        Ns_MutexLock(&proxyPtr->poolPtr->lock);
        reinit = proxyPtr->poolPtr->reinit != NULL;
        if (reinit) {
            Tcl_DStringAppend(&ds, proxyPtr->poolPtr->reinit, -1);
        }
        Ns_MutexUnlock(&proxyPtr->poolPtr->lock);
        if (reinit) {
            result = Eval(interp, proxyPtr, Tcl_DStringValue(&ds), -1);
        }
        Tcl_DStringFree(&ds);
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
    Proxy *proxyPtr = (Proxy *)clientData;
    int ms;

    if (objc < 2 || objc > 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "script ?timeout?");
        return TCL_ERROR;
    }
    if (objc == 2) {
        ms = -1;
    } else if (Tcl_GetIntFromObj(interp, objv[2], &ms) != TCL_OK) {
        return TCL_ERROR;
    }

    return Eval(interp, proxyPtr, Tcl_GetString(objv[1]), ms);
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
DeleteData(ClientData arg, Tcl_Interp *interp)
{
    InterpData *idataPtr = arg;

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
ReapProxies()
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

static int
GetTimeDiff(Ns_Time *timePtr)
{
    Ns_Time now, diff;

    Ns_GetTime(&now);
    return Ns_DiffTime(timePtr, &now, &diff)*(diff.sec/1000+diff.usec*1000);
}

/*
 *----------------------------------------------------------------------
 *
 * ProxyError --
 *
 *      Formats an extended error message.
 *
 * Results:
 *      Pointer to error message string.
 *
 * Side effects:
 *      Will set the errorCode global variable.
 *
 *----------------------------------------------------------------------
 */

static char *
ProxyError(Tcl_Interp *interp, Err err)
{
    char *msg, *sysmsg, *code;

    sysmsg = NULL;
    switch (err) {
    case ENone:
        code = "ENone";
        msg = "no error";
        break;
    case EDeadlock:
        code = "EDeadlock";
        msg = "allocation deadlock";
        break;
    case EExec:
        code = "EExec";
        msg = "could not create child process";
        sysmsg = strerror(errno);
        break;
    case ERange:
        code = "ERange";
        msg = "insufficient handles";
        break;
    case EGetTimeout:
        code = "EGetTimeout";
        msg = "timeout waiting for handle";
        break;
    case EEvalTimeout:
        code = "EEvalTimeout";
        msg = "timeout waiting for evaluation";
        break;
    case ESend:
        code = "ESend";
        msg = "script send failed";
        sysmsg = strerror(errno);
        break;
    case ERecv:
        code = "ERecv";
        msg = "result recv failed";
        sysmsg = strerror(errno);
        break;
    case EIdle:
        code = "EIdle";
        msg = "no script evaluating";
        break;
    case EImport:
        code = "EImport";
        msg = "invalid response";
        break;
    case EInit:
        code = "EInit";
        msg = "init script failed";
        break;
    case EDead:
        code = "EDead";
        msg = "child process died";
        break;
    case ENoWait:
        code = "ENoWait";
        msg = "no wait for script result";
        break;
    case EBusy:
        code = "EBusy";
        msg = "currently evaluating a script";
        break;
    default:
        code = "EUnknown";
        msg = "unknown error";
    }

    Tcl_SetErrorCode(interp, "NSPROXY", code, msg, sysmsg, NULL);

    return msg;
}
