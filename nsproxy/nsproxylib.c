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
 */

#include "nsproxy.h"

NS_RCSID("@(#) $Header$");

#include <grp.h>
#include <poll.h>

/*
 * It is pain in the neck to get a satisfactory definition of
 * u_int_XX_t or uintXX_t as different OS'es do that in different
 * header files and sometimes even do not define such types at all.
 * We choose to define them ourselves here and stop the blues.
 */

typedef unsigned int   uint32;
typedef unsigned short uint16;

#define MAJOR_VERSION 1
#define MINOR_VERSION 1

/*
 * The following structure defines a running proxy slave process.
 */

typedef struct Proc {
    struct Pool *poolPtr;
    struct Proc *nextPtr;
    int          rfd;
    int          wfd;
    int          signal;
    int          sigsent;
    pid_t        pid;
    Ns_Time      stop;
    Ns_Time      expire;
} Proc;

/*
 * The following structures defines a proxy request and response.
 * The lengths are in network order to support later proxy
 * operation over a socket connection.
 */

typedef struct Req {
    uint32 len;
    uint16 major;
    uint16 minor;
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

typedef struct Proxy {
    struct Proxy  *nextPtr;  /* Next in list of proxies */
    struct Proxy  *runPtr;   /* Next in list of active proxies */
    struct Pool   *poolPtr;  /* Pointer to proxy's pool */
    char           id[16];   /* Proxy unique string id */
    ProxyState     state;
    Proc          *procPtr;  /* Running slave, if any */
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
    Running,   /* Operating on pools and tearing down slaves */
    Sleeping,  /* Sleeping on cond var and waiting for work */
    Awaken,    /* Help state to distinguish from running */
    Stopping   /* Teardown of the thread initiated */
} ReaperState;

typedef struct Pool {
    char          *name;     /* Name of pool */
    struct Proxy  *firstPtr; /* First in list of avail proxies */
    struct Proxy  *runPtr;   /* First in list of active proxies */
    char          *exec;     /* Slave executable */
    char          *init;     /* Init script to eval on proxy start */
    char          *reinit;   /* Re-init scripts to eval on proxy put */
    int            waiting;  /* Thread waiting for handles */
    int            max;      /* Max number of allowed proxies slaves alive */
    int            min;      /* Min number of proxy slaves alive */
    int            nfree;    /* Current number of available proxy handles */
    int            nused;    /* Current number of used proxy handles */
    int            nextid;   /* Next in proxy unique ids */
    int            tget;     /* Timeout (ms) when getting proxy handles */
    int            teval;    /* Timeout (ms) when evaluating scripts */
    int            tsend;    /* Timeout (ms) to send data to proxy over pipe */
    int            trecv;    /* Timeout (ms) to receive results over pipe */
    int            twait;    /* Timeout (ms) to wait for slaves to die */
    int            tidle;    /* Timeout (ms) for slave to be idle */
    Ns_Mutex       lock;     /* Lock around the pool */
    Ns_Cond        cond;     /* Cond for use while allocating handles */
} Pool;

#define MIN_IDLE_TIMEOUT 1000 /* == 1 second */

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

static Pool*  GetPool(char *poolName, InterpData *idataPtr);
static void   FreePool(Pool *poolPtr);

static Proxy* CreateProxy(Pool *poolPtr);
static Err    PopProxy(Pool *poolPtr, Proxy **proxyPtrPtr, int nwant, int ms);
static void   PushProxy(Proxy *proxyPtr);
static int    GetProxy(InterpData *idataPtr, char *proxyId, Proxy **proxyPtrPtr);

static int    Eval(Tcl_Interp *interp, Proxy *proxyPtr, char *script, int ms);
static int    Send(Tcl_Interp *interp, Proxy *proxyPtr, char *script);
static int    Wait(Tcl_Interp *interp, Proxy *proxyPtr, int ms);
static int    Recv(Tcl_Interp *interp, Proxy *proxyPtr);

static Err    CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr);
static int    ReleaseProxy(Tcl_Interp *interp, Proxy *proxyPtr);
static void   CloseProxy(Proxy *proxyPtr);
static void   FreeProxy(Proxy *proxyPtr);
static void   ResetProxy(Proxy *proxyPtr);
static char*  ProxyError(Tcl_Interp *interp, Err err);

static void   ReleaseHandles(Tcl_Interp *interp, InterpData *idataPtr);
static Proc*  ExecSlave(Tcl_Interp *interp, Proxy *proxyPtr);

static void   SetExpire(Proc *procPtr);
static int    SendBuf(Proc *procPtr, int ms, Tcl_DString *dsPtr);
static int    RecvBuf(Proc *procPtr, int ms, Tcl_DString *dsPtr);
static int    WaitFd(int fd, int events, int ms);

static int    Import(Tcl_Interp *interp, Tcl_DString *dsPtr, int *resultPtr);
static void   Export(Tcl_Interp *interp, int code, Tcl_DString *dsPtr);

static void   UpdateIov(struct iovec *iov, int n);
static void   SetOpt(char *str, char **optPtr);
static void   ReaperThread(void *ignored);
static void   CloseProc(Proc *procPtr);
static void   ReapProxies(void);
static void   Kill(int pid, int sig);

static void   AppendStr(Tcl_Interp *interp, CONST char *flag, char *val);
static void   AppendInt(Tcl_Interp *interp, CONST char *flag, int i);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable pools;     /* Tracks proxy pools */

ReaperState reaperState = Stopped;

static Ns_Cond  pcond;          /* Those are used to control access to */
static Ns_Mutex plock;          /* the list of Proc structures of slave */
static Proc    *firstClosePtr;  /* processes which are being closed. */

static Ns_DString defexec;      /* Stores full path of the proxy executable */


/*
 *----------------------------------------------------------------------
 *
 * Nsproxy_Init --
 *
 *      Tcl load entry point.
 *
 * Results:
 *      See Ns_ProxyInit.
 *
 * Side effects:
 *      See Ns_ProxyInit.
 *
 *----------------------------------------------------------------------
 */

int
Nsproxy_Init(Tcl_Interp *interp)
{
    return Ns_ProxyInit(interp);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyInit --
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
Ns_ProxyInit(Tcl_Interp *interp)
{
    static int once = 0;
    InterpData *idataPtr;

    Ns_MutexLock(&plock);
    if (!once) {
        once = 1;
        Ns_DStringInit(&defexec);
        Ns_BinPath(&defexec, "nsproxy", NULL);
        Tcl_InitHashTable(&pools, TCL_STRING_KEYS);
    }
    Ns_MutexUnlock(&plock);

    idataPtr = ns_calloc(1, sizeof(InterpData));
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
    Proc         proc;
    Req         *reqPtr;
    int          uid = -1, gid = -1, result, len, n, max;
    Tcl_DString  in, out;
    char        *script, *active, *dots;
    char        *uarg = NULL, *user = NULL, *group = NULL;
    uint16       major, minor;

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
    proc.pid = -1;

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

    interp = Ns_TclCreateInterp();
    if (init != NULL) {
        if ((*init)(interp) != TCL_OK) {
            Ns_Fatal("nsproxy: init: %s", interp->result);
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

    if (user != NULL) {
        uid = Ns_GetUid(user);
        if (uid == -1) {
            int nc;
            /*
             * Hm, try see if given as numeric uid...
             */
            if (sscanf(user, "%d%n", &uid, &nc) != 1
                || nc != strlen(user)
                || Ns_GetNameForUid(NULL, (uid_t)uid) == NS_FALSE) {
                Ns_Fatal("nsproxy: unknown user '%s'", user);
            }
            /*
             * Set user-passed value to NULL, causing supplementary 
             * groups to be ignored later.
             */
            user = NULL;
        }
        if (user != NULL) {
             gid = Ns_GetUserGid(user);
        } else {
            Ns_DString ds;
            Ns_DStringInit(&ds);
            if (Ns_GetNameForUid(&ds, (uid_t)uid) == NS_TRUE) {
                gid = Ns_GetUserGid(Ns_DStringValue(&ds));
            }
            Ns_DStringFree(&ds);
        }
    }

    if (group != NULL) {
        gid = Ns_GetGid(group);
        if (gid == -1) {
            int nc;
            if (sscanf(group, "%d%n", (int*)&gid, &nc) != 1
                || nc != strlen(group)
                || Ns_GetNameForGid(NULL, (gid_t)gid) == NS_FALSE) {
                Ns_Fatal("nsproxy: unknown group '%s'", group);
            }
        }
    }

    /*
     * Switch the process uid/gid accordingly
     */

    if (uid > -1 || gid > -1) {
        if (user != NULL) {
            if (initgroups(user, gid) != 0) {
                Ns_Fatal("nsproxy: initgroups(%s, %d) failed: %s", user,
                         gid, strerror(errno));
            }
        } else {
            if (setgroups(0, NULL) != 0) {
                Ns_Fatal("nsproxy: setgroups(0, NULL) failed: %s",
                         strerror(errno));
            }
        }
        if (gid != (int)getgid() && setgid((gid_t)gid) != 0) {
            Ns_Fatal("nsproxy: setgid(%d) failed: %s", gid, strerror(errno));
        }
        if (uid != (int)getuid() && setuid((uid_t)uid) != 0) {
            Ns_Fatal("nsproxy: setuid(%d) failed: %s", uid, strerror(errno));
        }
    }

    /*
     * Loop continuously processing proxy requests.
     */

    Tcl_DStringInit(&in);
    Tcl_DStringInit(&out);

    while (RecvBuf(&proc, -1, &in)) {
        if (in.length < sizeof(Req)) {
            break;
        }
        reqPtr = (Req *) in.string;
        if (reqPtr->major != major || reqPtr->minor != minor) {
            Ns_Fatal("nsproxy: version mismatch");
        }
        len = ntohl(reqPtr->len);
        if (len == 0) {
            Export(NULL, TCL_OK, &out); 
        } else if (len > 0) {
            script = in.string + sizeof(Req);
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
                active[0] = '\0';
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
 * Ns_ProxyExit --
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
Ns_ProxyExit(void *arg)
{
    Pool           *poolPtr;
    Proxy          *proxyPtr, *tmpPtr;
    Ns_Time        *toutPtr, wait;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    int             reap, status;

    /*
     * Cleanup all known pools. This will put all idle
     * proxies on the close list. At this point, there
     * should be no running nor detached proxies. 
     * If yes, we will leak memory on exit.
     */

    Ns_MutexLock(&plock);
    hPtr = Tcl_FirstHashEntry(&pools, &search);
    while (hPtr != NULL) {
        poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
        Ns_MutexLock(&poolPtr->lock);
        proxyPtr = poolPtr->firstPtr;
        while (proxyPtr != NULL) {
            if (proxyPtr->procPtr) {
                CloseProc(proxyPtr->procPtr);
            }
            tmpPtr = proxyPtr->nextPtr;
            FreeProxy(proxyPtr);
            proxyPtr = tmpPtr;
        }
        Ns_MutexUnlock(&poolPtr->lock);
        Tcl_DeleteHashEntry(hPtr);
        FreePool(poolPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    reap = (reaperState == Stopped && firstClosePtr != NULL);
    Tcl_DeleteHashTable(&pools);
    Ns_MutexUnlock(&plock);

    /*
     * If the reaper thread is not running, start it now
     * so it can close proxies placed on the close list
     * by the code above.
     */

    if (reap) {
        ReapProxies();
    }

    /*
     * Now terminate the thread
     */
    
    toutPtr = (Ns_Time *) arg;
    if (toutPtr != NULL) {
        Ns_GetTime(&wait);
        Ns_IncrTime(&wait, toutPtr->sec, toutPtr->usec);
    }

    Ns_MutexLock(&plock);
    if (reaperState != Stopped) {
        reaperState = Stopping;
        status = NS_OK;
        Ns_CondSignal(&pcond);
        while (reaperState != Stopped && status == NS_OK) {
            if (toutPtr != NULL) {
                status = Ns_CondTimedWait(&pcond, &plock, &wait);
                if (status != NS_OK) {
                    Ns_Log(Warning, "nsproxy: timeout waiting for reaper exit");
                }
            } else {
                Ns_CondWait(&pcond, &plock);
            }
        }
    }
    Ns_MutexUnlock(&plock);
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
 *      Pointer to new Proc or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Proc *
ExecSlave(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Pool *poolPtr = proxyPtr->poolPtr;
    char *argv[5], active[100];
    Proc *procPtr;
    int   rpipe[2], wpipe[2], pid, len;

    len = sizeof(active) - 1;
    memset(active, ' ', len);
    active[len] = '\0';
    argv[0] = poolPtr->exec;
    argv[1] = poolPtr->name;
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

    if (pid < 0) {
        Tcl_AppendResult(interp, "exec failed: ", Tcl_PosixError(interp), NULL);
        close(wpipe[0]);
        close(rpipe[1]);
        return NULL;
    }

    procPtr = ns_calloc(1, sizeof(Proc));
    procPtr->poolPtr = proxyPtr->poolPtr;
    procPtr->pid = pid;
    procPtr->rfd = wpipe[0];
    procPtr->wfd = rpipe[1];

    SetExpire(procPtr);

    return procPtr;
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
SetExpire(Proc *procPtr) 
{
    Pool *poolPtr = procPtr->poolPtr;
    int idle = poolPtr->tidle;

    if (idle > 0) {
        Ns_GetTime(&procPtr->expire);
        Ns_IncrTime(&procPtr->expire, idle/1000, (idle%1000) * 1000);
    } else {
        procPtr->expire.sec  = INT_MAX;
        procPtr->expire.usec = LONG_MAX;
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
 *      Depends on script.
 *
 * Side effects:
 *  Will return proxy response or format error message in given
 *  interp.
 *
 *----------------------------------------------------------------------
 */

static int
Eval(Tcl_Interp *interp, Proxy *proxyPtr, char *script, int ms)
{
    int result;

    result = Send(interp, proxyPtr, script);
    if (result == TCL_OK) {
        result = Wait(interp, proxyPtr, ms);
        if (result == TCL_OK) {
            result = Recv(interp, proxyPtr);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Send a script to a proxy.
 *
 * Results:
 *      TCL_OK if script sent, TCL_ERROR otherwise.
 *
 * Side effects:
 *      Will format error message in given interp on failure.
 *
 *----------------------------------------------------------------------
 */

static int
Send(Tcl_Interp *interp, Proxy *proxyPtr, char *script)
{
    Err err = ENone;
    int len;
    Req req;
    Pool *poolPtr = proxyPtr->poolPtr;

    if (proxyPtr->procPtr == NULL) {
        err = EDead;
    } else if (proxyPtr->state != Idle) {
        err = EBusy;
    } else {
        len = script ? strlen(script) : 0;
        req.len = htonl(len);
        req.major = htons(MAJOR_VERSION);
        req.minor = htons(MINOR_VERSION);
        Tcl_DStringAppend(&proxyPtr->in, (char *) &req, sizeof(req));
        if (len > 0) {
            Tcl_DStringAppend(&proxyPtr->in, script, len);
        }

        proxyPtr->state = Busy;

        /*
         * Proxy is now active, put it on the run queue
         * and send the command to the slave for evaluation.
         */

        Ns_MutexLock(&poolPtr->lock);
        proxyPtr->runPtr = poolPtr->runPtr;
        poolPtr->runPtr  = proxyPtr;
        Ns_MutexUnlock(&poolPtr->lock);

        if (!SendBuf(proxyPtr->procPtr, proxyPtr->poolPtr->tsend, &proxyPtr->in)) {
            ResetProxy(proxyPtr);
            err = ESend;
        }
    }
    if (err != ENone) {
        Tcl_AppendResult(interp, "could not send script \"", 
                         script ? script : "<empty>",
                         "\" to proxy \"", proxyPtr->id, "\": ",
                         ProxyError(interp, err), NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Wait --
 *
 *      Wait for response from proxy process.
 *
 * Results:
 *      TCL_OK if wait ok, TCL_ERROR otherwise.
 *
 * Side effects:
 *      Will format error message in given interp on failure.
 *
 *----------------------------------------------------------------------
 */

static int
Wait(Tcl_Interp *interp, Proxy *proxyPtr, int ms)
{
    Err err = ENone;

    if (proxyPtr->state == Idle) {
        err = EIdle;
    } else if (proxyPtr->state != Done) {
        if (ms <= 0) {
            ms = proxyPtr->poolPtr->teval;
        }
        if (ms <= 0) {
            ms = -1;
        }
        if (!WaitFd(proxyPtr->procPtr->rfd, POLLIN, ms)) {
            err = EEvalTimeout;
        } else {
            proxyPtr->state = Done;
        }
    }
    if (err != ENone) {
        Tcl_AppendResult(interp, "wait for proxy \"", proxyPtr->id,
                         "\" failed: ", ProxyError(interp, err), NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Recv --
 *
 *      Receive proxy results.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Will append proxy results or error message to given interp.
 *
 *----------------------------------------------------------------------
 */

static int
Recv(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Pool        *poolPtr = proxyPtr->poolPtr;
    Proc        *procPtr = proxyPtr->procPtr;
    int          result;
    Err          err = ENone;

    if (proxyPtr->state == Idle) {
        err = EIdle;
    } else if (proxyPtr->state == Busy) {
        err = ENoWait;
    } else {
        Tcl_DStringTrunc(&proxyPtr->out, 0);
        if (!RecvBuf(procPtr, poolPtr->trecv, &proxyPtr->out)) {
            err = ERecv;
        } else if (Import(interp, &proxyPtr->out, &result) != TCL_OK) {
            err = EImport;
        } else {
            proxyPtr->state = Idle;
        }
        ResetProxy(proxyPtr);
    }
    if (err != ENone) {
        Tcl_AppendResult(interp, "could not receive result from proxy \"",
                         proxyPtr->id, "\": ", ProxyError(interp, err), NULL);
        return TCL_ERROR;
    }
    
    return result;
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
SendBuf(Proc *procPtr, int ms, Tcl_DString *dsPtr)
{
    int          n;
    uint32       ulen;
    struct iovec iov[2];

    ulen = htonl(dsPtr->length);
    iov[0].iov_base = (caddr_t) &ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = dsPtr->length;
    while ((iov[0].iov_len + iov[1].iov_len) > 0) {
        do {
            n = writev(procPtr->wfd, iov, 2);
        } while (n == -1 && errno == EINTR);
        if (n == -1 && errno == EAGAIN && WaitFd(procPtr->wfd, POLLOUT, ms)) {
            n = writev(procPtr->wfd, iov, 2);
        }
        if (n == -1) {
            return 0;
        }
        UpdateIov(iov, n);
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
RecvBuf(Proc *procPtr, int ms, Tcl_DString *dsPtr)
{
    uint32       ulen;
    char        *ptr;
    int          n, len, avail;
    struct iovec iov[2];

    avail = dsPtr->spaceAvl - 1;
    iov[0].iov_base = (caddr_t) &ulen;
    iov[0].iov_len  = sizeof(ulen);
    iov[1].iov_base = dsPtr->string;
    iov[1].iov_len  = avail;
    while (iov[0].iov_len > 0) {
        do {
            n = readv(procPtr->rfd, iov, 2);
        } while (n == -1 && errno == EINTR);
        if (n < 0 && errno == EAGAIN && WaitFd(procPtr->rfd, POLLIN, ms)) {
            n = readv(procPtr->rfd, iov, 2);
        }
        if (n <= 0) {
            return 0;
        }
        UpdateIov(iov, n);
    }
    n = avail - iov[1].iov_len;
    Tcl_DStringSetLength(dsPtr, n);
    len = ntohl(ulen);
    Tcl_DStringSetLength(dsPtr, len);
    len -= n;
    ptr  = dsPtr->string + n;
    while (len > 0) {
        do {
            n = read(procPtr->rfd, ptr, len);
        } while (n == -1 && errno == EINTR);
        if (n < 0 && errno == EAGAIN && WaitFd(procPtr->rfd, POLLIN, ms)) {
            n = read(procPtr->rfd, ptr, len);
        }
        if (n <= 0) {
            return 0;
        }
        len -= n;
        ptr += n;
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
    int           n;

    pfd.fd = fd;
    pfd.events = event | POLLPRI | POLLERR;
    pfd.revents = pfd.events;
    do {
        n = poll(&pfd, 1, ms);
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
        iov[0].iov_base += n;
        n = 0;
    }
    iov[1].iov_len  -= n;
    iov[1].iov_base += n;
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
    char *einfo, *ecode, *result;
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
        Tcl_SetResult(interp, str, TCL_VOLATILE);
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
ProxyObjCmd(ClientData data, Tcl_Interp *interp, int objc, 
            Tcl_Obj *CONST objv[])
{
    InterpData    *idataPtr = data;
    Pool          *poolPtr;
    Proxy         *proxyPtr;
    int            ms, reap, result = TCL_OK;
    char          *proxyId, *name;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    static CONST char *opts[] = {
        "get", "put", "release", "eval", "cleanup", "configure", 
        "ping", "active", "free", "handles", "clear",
        "send", "wait", "recv", NULL
    };
    enum {
        PGetIdx, PPutIdx, PReleaseIdx, PEvalIdx, PCleanupIdx, PConfigureIdx,
        PPingIdx, PActiveIdx, PFreeIdx, PHandlesIdx, PClearIdx,
        PSendIdx, PWaitIdx, PRecvIdx
    } opt;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?args");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            (int *) &opt) != TCL_OK) {
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
        if (!GetProxy(idataPtr, proxyId, &proxyPtr)) {
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
        if (!GetProxy(idataPtr, proxyId, &proxyPtr)) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        result = Send(interp, proxyPtr, Tcl_GetString(objv[3]));
        break;

    case PWaitIdx:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle ?timeout?");
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        if (!GetProxy(idataPtr, proxyId, &proxyPtr)) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        if (objc == 3) {
            ms = -1;
        } else if (Tcl_GetIntFromObj(interp, objv[3], &ms) != TCL_OK) {
            return TCL_ERROR;
        }
        result = Wait(interp, proxyPtr, ms);
        break;
        
    case PRecvIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle");
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        if (!GetProxy(idataPtr, proxyId, &proxyPtr)) {
            Tcl_AppendResult(interp, "no such handle: ", proxyId, NULL);
            return TCL_ERROR;
        }
        result = Recv(interp, proxyPtr);
        break;
        
    case PEvalIdx:
        if (objc != 4 && objc != 5) {
            Tcl_WrongNumArgs(interp, 2, objv, "handle script");
            return TCL_ERROR;
        }
        proxyId = Tcl_GetString(objv[2]);
        if (!GetProxy(idataPtr, proxyId, &proxyPtr)) {
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

    case PActiveIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool");
            return TCL_ERROR;
        }
        poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
        Ns_MutexLock(&poolPtr->lock);
        proxyPtr = poolPtr->runPtr;
        while (proxyPtr != NULL) {
            if (proxyPtr->state != Idle) {
                Tcl_AppendElement(interp, proxyPtr->id);
                Tcl_AppendElement(interp, proxyPtr->in.string + sizeof(Req));
            }
            proxyPtr = proxyPtr->nextPtr;
        }
        Ns_MutexUnlock(&poolPtr->lock);
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

    case PClearIdx:
        if (objc > 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "?pool?");
            return TCL_ERROR;
        }
        name = (objc == 3) ? Tcl_GetString(objv[2]) : NULL;
        reap = 0;
        Ns_MutexLock(&plock);
        hPtr = Tcl_FirstHashEntry(&pools, &search);
        while (hPtr != NULL) {
            poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
            if (objc == 2 || (objc == 3 && !strcmp(name, poolPtr->name))) {
                Ns_MutexLock(&poolPtr->lock);
                proxyPtr = poolPtr->firstPtr;
                while (proxyPtr != NULL) {
                    if (proxyPtr->procPtr) {
                        CloseProc(proxyPtr->procPtr);
                        proxyPtr->procPtr = NULL;
                        reap++;
                    }
                    proxyPtr = proxyPtr->nextPtr;
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
ConfigureObjCmd(ClientData data, Tcl_Interp *interp, int objc, 
             Tcl_Obj *CONST objv[])
{
    InterpData *idataPtr = data;
    Pool       *poolPtr;
    Proxy      *proxyPtr;
    char       *str;
    int         i, incr, n, result, reap = 0;

    static CONST char *flags[] = {
        "-init", "-reinit", "-minslaves", "-maxslaves", "-exec", 
        "-gettimeout", "-evaltimeout", "-sendtimeout", "-recvtimeout",
        "-waittimeout", "-idletimeout", NULL
    };
    enum {
        CInitIdx, CReinitIdx, CMinIdx, CMaxIdx, CExecIdx, CGetIdx,
        CEvalIdx, CSendIdx, CRecvIdx, CWaitIdx, CIdleIdx
    } flag;

    if (objc < 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "pool ?opt? ?val? ?opt val?...");
        return TCL_ERROR;
    }
    result = TCL_ERROR;
    poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
    Ns_MutexLock(&poolPtr->lock);
    if (objc == 4) {
        if (Tcl_GetIndexFromObj(interp, objv[3], flags, "flags", 0,
                                (int *) &flag) != TCL_OK) {
            goto err;
        }
    } else if (objc > 4) {
        for (i = 3; i < (objc - 1); ++i) {
            if (Tcl_GetIndexFromObj(interp, objv[i], flags, "flags", 0,
                                    (int *) &flag)) {
                goto err;
            }
            ++i;
            incr = 0;
            str = Tcl_GetString(objv[i]);
            switch (flag) {
            case CGetIdx:
            case CEvalIdx:
            case CSendIdx:
            case CRecvIdx:
            case CWaitIdx:
            case CIdleIdx:
            case CMinIdx:
            case CMaxIdx:
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
                    poolPtr->tget = n;
                    break;
                case CEvalIdx:
                    poolPtr->teval = n;
                    break;
                case CSendIdx:
                    poolPtr->tsend = n;
                    break;
                case CRecvIdx:
                    poolPtr->trecv = n;
                    break;
                case CWaitIdx:
                    poolPtr->twait = n;
                    break;
                case CMinIdx:
                    poolPtr->min = n;
                    reap = 1;
                    break;
                case CMaxIdx:
                    poolPtr->max = n;
                    reap = 1;
                    break;
                case CIdleIdx:
                    poolPtr->tidle = n;
                    if (poolPtr->tidle < MIN_IDLE_TIMEOUT) {
                        poolPtr->tidle = MIN_IDLE_TIMEOUT;
                    }
                    proxyPtr = poolPtr->firstPtr;
                    while (proxyPtr != NULL) {
                        if (proxyPtr->procPtr) {
                            SetExpire(proxyPtr->procPtr);
                        }
                        proxyPtr = proxyPtr->nextPtr;
                    }
                    proxyPtr = poolPtr->runPtr;
                    while (proxyPtr != NULL) {
                        if (proxyPtr->procPtr) {
                            SetExpire(proxyPtr->procPtr);
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

        if (poolPtr->min > poolPtr->max) {
            poolPtr->min = poolPtr->max;
        }
        
        /*
         * Assure number of idle and used proxies always
         * match the maximum number of configured ones.
         */
        
        while ((poolPtr->nfree + poolPtr->nused) < poolPtr->max) {
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
        AppendStr(interp, flags[CExecIdx],   poolPtr->exec);
        AppendStr(interp, flags[CInitIdx],   poolPtr->init);
        AppendStr(interp, flags[CReinitIdx], poolPtr->reinit);
        AppendInt(interp, flags[CMaxIdx],    poolPtr->max);
        AppendInt(interp, flags[CMinIdx],    poolPtr->min);
        AppendInt(interp, flags[CGetIdx],    poolPtr->tget);
        AppendInt(interp, flags[CEvalIdx],   poolPtr->teval);
        AppendInt(interp, flags[CSendIdx],   poolPtr->tsend);
        AppendInt(interp, flags[CRecvIdx],   poolPtr->trecv);
        AppendInt(interp, flags[CWaitIdx],   poolPtr->twait);
        AppendInt(interp, flags[CIdleIdx],   poolPtr->tidle);
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
        case CMaxIdx:
            AppendInt(interp, NULL, poolPtr->max);
            break;
        case CMinIdx:
            AppendInt(interp, NULL, poolPtr->min);
            break;
        case CGetIdx:
            AppendInt(interp, NULL, poolPtr->tget);
            break;
        case CEvalIdx:
            AppendInt(interp, NULL, poolPtr->teval);
            break;
        case CSendIdx:
            AppendInt(interp, NULL, poolPtr->tsend);
            break;
        case CRecvIdx:
            AppendInt(interp, NULL, poolPtr->trecv);
            break;
        case CWaitIdx:
            AppendInt(interp, NULL, poolPtr->twait);
            break;
        case CIdleIdx:
            AppendInt(interp, NULL, poolPtr->tidle);
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
     * enforce min/max constraints.
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
    char buf[20];

    sprintf(buf, "%d", i);
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
GetObjCmd(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    InterpData    *idataPtr = data;
    Proxy         *proxyPtr, *firstPtr;
    Tcl_HashEntry *cntPtr, *idPtr;
    int            i, new, nwant, n, ms;
    char          *arg;
    Err            err;
    Pool          *poolPtr;

    static CONST char *flags[] = {
        "-timeout", "-handles", NULL
    };
    enum {
        FTimeoutIdx, FHandlesIdx
    } flag;

    if (objc < 3 || (objc % 2) != 1) {
        Tcl_WrongNumArgs(interp, 2, objv, "pool ?-opt val -opt val ...?");
        return TCL_ERROR;
    }
    poolPtr = GetPool(Tcl_GetString(objv[2]), idataPtr);
    cntPtr = Tcl_CreateHashEntry(&idataPtr->cnts, (char *) poolPtr, &new);
    if ((int)Tcl_GetHashValue(cntPtr) > 0) {
        err = EDeadlock;
        goto errout;
    }

    nwant = 1;
    ms = poolPtr->tget;
    for (i = 3; i < objc; ++i) {
        arg = Tcl_GetString(objv[2]);
        if (Tcl_GetIndexFromObj(interp, objv[i], flags, "flags", 0,
                                (int *) &flag)) {
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

    Tcl_SetHashValue(cntPtr, nwant);
    proxyPtr = firstPtr;
    while (proxyPtr != NULL) {
        idPtr = Tcl_CreateHashEntry(&idataPtr->ids, proxyPtr->id, &new);
        if (!new) {
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
        Ns_CondBroadcast(&poolPtr->cond);
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
    int      i, status = NS_OK;
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
               && poolPtr->nfree < nwant && poolPtr->max >= nwant) {
            if (ms > 0) {
                status = Ns_CondTimedWait(&poolPtr->cond, &poolPtr->lock,
                                          &tout);
            } else {
                Ns_CondWait(&poolPtr->cond, &poolPtr->lock);
            }
        }
        if (status != NS_OK) {
            err = EGetTimeout;
        } else if (poolPtr->max == 0 || poolPtr->max < nwant) {
            err = ERange;
        } else {
            poolPtr->nfree -= nwant;
            poolPtr->nused += nwant;
            for (i = 0, *proxyPtrPtr = NULL; i < nwant; ++i) {
                proxyPtr = poolPtr->firstPtr;
                poolPtr->firstPtr = proxyPtr->nextPtr;
                proxyPtr->nextPtr = *proxyPtrPtr;
                *proxyPtrPtr = proxyPtr;
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
 * GetPool --
 *
 *      Get a pool by name.
 *
 * Results:
 *      1 if pool found, 0 if no such pool.
 *
 * Side effects:
 *      Will update given poolPtrPtr with pointer to Pool.
 *
 *----------------------------------------------------------------------
 */

Pool *
GetPool(char *poolName, InterpData *idataPtr)
{
    Tcl_HashEntry *hPtr;
    Pool          *poolPtr;
    Proxy         *proxyPtr;
    int            new, i;
    char          *path = NULL, *exec = NULL;

    Ns_MutexLock(&plock);
    hPtr = Tcl_CreateHashEntry(&pools, poolName, &new);
    if (!new) {
        poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
    } else {
        poolPtr = ns_calloc(1, sizeof(Pool));
        Tcl_SetHashValue(hPtr, poolPtr);
        poolPtr->name = Tcl_GetHashKey(&pools, hPtr);
        if (idataPtr && idataPtr->server && idataPtr->module) {
            path = Ns_ConfigGetPath(idataPtr->server, idataPtr->module, NULL);
        }
        if (path != NULL && (exec = Ns_ConfigGetValue(path, "exec")) != NULL) {
            SetOpt(exec, &poolPtr->exec);
        } else {
            SetOpt(defexec.string, &poolPtr->exec);
        }
        if (path == NULL) {
            poolPtr->teval = 0;
            poolPtr->tget  = 0;
            poolPtr->tsend = 5000;
            poolPtr->trecv = 5000;
            poolPtr->twait = 1000;
            poolPtr->max   = 4;
            poolPtr->min   = 0;
        } else {
            poolPtr->teval = Ns_ConfigInt(path, "evaltimeout", 0);
            poolPtr->tget  = Ns_ConfigInt(path, "gettimeout",  0);
            poolPtr->tsend = Ns_ConfigInt(path, "sendtimeout", 5000);
            poolPtr->trecv = Ns_ConfigInt(path, "recvtimeout", 5000);
            poolPtr->twait = Ns_ConfigInt(path, "waittimeout", 1000);
            poolPtr->max   = Ns_ConfigInt(path, "maxslaves", 4);
            poolPtr->min   = Ns_ConfigInt(path, "minslaves", 0);
        }
        for (i = 0; i < poolPtr->max; i++) {
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

static Proxy *
CreateProxy(Pool *poolPtr)
{
    Proxy *proxyPtr;

    proxyPtr = ns_calloc(1, sizeof(Proxy));
    sprintf(proxyPtr->id, "%s-%d", poolPtr->name, poolPtr->nextid++);
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
 *      1 if handle found, 0 if no such handle.
 *
 * Side effects:
 *      Will update given proxyPtrPtr with pointer to handle.
 *
 *----------------------------------------------------------------------
 */

static int
GetProxy(InterpData *idataPtr, char *proxyId, Proxy **proxyPtrPtr)
{
    Tcl_HashEntry *hPtr;

    hPtr = Tcl_FindHashEntry(&idataPtr->ids, proxyId);
    if (hPtr) {
        *proxyPtrPtr = (Proxy *)Tcl_GetHashValue(hPtr);
    }

    return hPtr != NULL;
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
 *      1 if proxy ok, 0 if proc couldn't be created.
 *
 * Side effects:
 *      May restart process if necessary.
 *
 *----------------------------------------------------------------------
 */

static Err
CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Pool *poolPtr = proxyPtr->poolPtr;
    Err err = ENone;

    if (proxyPtr->procPtr && Eval(interp, proxyPtr, NULL, -1) != TCL_OK) {
        CloseProxy(proxyPtr);
        Tcl_ResetResult(interp);
    }
    if (proxyPtr->procPtr == NULL) {
        proxyPtr->procPtr = ExecSlave(interp, proxyPtr);
        if (proxyPtr->procPtr == NULL) {
            err = EExec;
        } else if (proxyPtr->poolPtr->init != NULL
                   && Eval(interp, proxyPtr, poolPtr->init, -1) != TCL_OK) {
            CloseProxy(proxyPtr);
            err = EInit;
        } else if (Eval(interp, proxyPtr, NULL, -1) != TCL_OK) {
            CloseProxy(proxyPtr);
            err = EInit;
        } else {
            err = ENone;
            Tcl_ResetResult(interp);
        }
    }

    return err;
}

/*
 *----------------------------------------------------------------------
 *
 * CloseProc --
 *
 *      Close the given proc handle (assumes caller holds the plock)
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
CloseProc(Proc *procPtr)
{
    int ms = procPtr->poolPtr->twait;

    /*
     * Set the time to kill the process. This time us used
     * for loop calculations in the reaper thread.
     */

    Ns_GetTime(&procPtr->stop);
    Ns_IncrTime(&procPtr->stop, ms/1000, (ms%1000) * 1000);

    close(procPtr->wfd);
    procPtr->signal  = 0;
    procPtr->sigsent = 0;

    /*
     * Put on the head of the close list
     */

    procPtr->nextPtr = firstClosePtr;
    firstClosePtr = procPtr;
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
    if (proxyPtr->procPtr != NULL) {
        Ns_MutexLock(&plock);
        CloseProc(proxyPtr->procPtr);
        proxyPtr->procPtr = NULL;
        proxyPtr->state = Idle;
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
Kill(int pid, int sig)
{
    if (kill((pid_t)pid, sig) != 0 && errno != ESRCH) {
        Ns_Log(Error, "kill(%d, %d) failed: %s", pid, sig, strerror(errno));
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
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    Proxy          *proxyPtr, *prevPtr, *nextPtr;
    Pool           *poolPtr;
    Proc           *procPtr, *prevProcPtr, *tmpProcPtr;
    Ns_Time         tout, now, diff;
    int             ms, expire, ntotal, indefinite;

    Ns_MutexLock(&plock);

    reaperState = Running;
    Ns_CondSignal(&pcond); /* Wakeup starter thread */

    Ns_ThreadSetName("-nsproxy:reap-");
    Ns_Log(Notice, "starting");

    while (1) {

        Ns_GetTime(&now);

        tout.sec  = INT_MAX;
        tout.usec = LONG_MAX;

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
            if (poolPtr->tidle) {
                diff = now;
                ms = poolPtr->tidle;
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
            while (proxyPtr != NULL) {
                nextPtr = proxyPtr->nextPtr;
                procPtr = proxyPtr->procPtr;
                ntotal  = poolPtr->nfree + poolPtr->nused;
                expire  = poolPtr->max < ntotal;
                if (procPtr) {
                    if (Ns_DiffTime(&procPtr->expire, &tout, NULL) <= 0) {
                        tout = procPtr->expire;
                    }
                    expire |= Ns_DiffTime(&procPtr->expire,&now,NULL) <= 0;
                }
                if (expire && poolPtr->min < ntotal) {
                    
                    /*
                     * Excessive or timed-out slave; destroy
                     */
                    
                    if (prevPtr != NULL) {
                        prevPtr->nextPtr = proxyPtr->nextPtr;
                    }
                    if (proxyPtr == poolPtr->firstPtr) {
                        poolPtr->firstPtr = proxyPtr->nextPtr;
                    }
                    if (procPtr) {
                        CloseProc(procPtr);
                    }
                    FreeProxy(proxyPtr);
                    proxyPtr = NULL;
                    poolPtr->nfree--;
                    
                } else if (expire) {
                    
                    /* 
                     * The min constraint does not allow teardown
                     * so re-set the expiry time for later 
                     */
                    
                    SetExpire(procPtr);
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

        procPtr = firstClosePtr;
        prevProcPtr = NULL;

        while (procPtr != NULL) {
            if (Ns_DiffTime(&now, &procPtr->stop, NULL) > 0) {
                
                /*
                 * Stop time expired, add new quantum and signal
                 * the process to exit. After first quantum has 
                 * expired, be polite and try the TERM signal.
                 * If this does not get the process down within
                 * the second quantum, try the KILL signal.
                 * If this does not get the process down within
                 * the third quantum, abort - we have a zombie.
                 */

                Ns_IncrTime(&procPtr->stop, procPtr->poolPtr->twait/1000,
                            (procPtr->poolPtr->twait%1000) * 1000);
                switch (procPtr->signal) {
                case 0:       procPtr->signal = SIGTERM; break;
                case SIGTERM: procPtr->signal = SIGKILL; break;
                case SIGKILL: procPtr->signal = -1;      break;
                }
            }
            if (procPtr->signal == -1 || WaitFd(procPtr->rfd, POLLIN, 0)) {

                /*
                 * We either have a zombie or the process has exited ok
                 * so splice it out the list.
                 */
                
                if (procPtr->signal >= 0) {
                    Ns_WaitProcess(procPtr->pid); /* Should not really wait */
                } else {
                    Ns_Log(Warning, "nsproxy: zombie: %d", procPtr->pid);
                }
                if (prevProcPtr != NULL) {
                    prevProcPtr->nextPtr = procPtr->nextPtr;
                } else {
                    firstClosePtr = procPtr->nextPtr;
                }

                tmpProcPtr = procPtr->nextPtr;
                close(procPtr->rfd);
                ns_free(procPtr);
                procPtr = tmpProcPtr;

            } else {

                /*
                 * Process is still arround, try killing it but leave it
                 * in the list. Calculate the latest time we'll visit 
                 * this one again.
                 */

                if (Ns_DiffTime(&procPtr->stop, &tout, NULL) < 0) {
                    tout = procPtr->stop;
                }
                if (procPtr->signal != procPtr->sigsent) {
                    Ns_Log(Warning, "[%s]: pid %d won't die, send signal %d",
                           procPtr->poolPtr->name,procPtr->pid,procPtr->signal);
                    Kill(procPtr->pid, procPtr->signal);
                    procPtr->sigsent = procPtr->signal;
                }
                prevProcPtr = procPtr;
                procPtr = procPtr->nextPtr;
            }
        }

        /*
         * Here we wait until signalled, or at most the
         * time we need to expire next slave or kill
         * some of them found on the close list.
         */

        if (Ns_DiffTime(&tout, &now, &diff) > 0) {
            reaperState = Sleeping;
            indefinite  = (tout.sec == INT_MAX) && (tout.usec == LONG_MAX);
            Ns_CondBroadcast(&pcond);
            if (indefinite) {
                Ns_CondWait(&pcond, &plock);
            } else {
                Ns_CondTimedWait(&pcond, &plock, &tout);
            }
            if (indefinite && reaperState == Stopping) {
                break;
            }
            reaperState = Running;
        }
    }

    reaperState = Stopped;
    Ns_CondSignal(&pcond);
    Ns_Log(Notice, "exiting");
    Ns_MutexUnlock(&plock);
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
    Pool *poolPtr = proxyPtr->poolPtr;

    if (proxyPtr->cntPtr) {
        int nhave = (int)Tcl_GetHashValue(proxyPtr->cntPtr);
        nhave--;
        Tcl_SetHashValue(proxyPtr->cntPtr, nhave);
        if (proxyPtr->idPtr) {
            Tcl_DeleteHashEntry(proxyPtr->idPtr);
            proxyPtr->idPtr = NULL;
        }
        proxyPtr->cntPtr = NULL;
    }
    Ns_MutexLock(&poolPtr->lock);
    poolPtr->nused--;
    poolPtr->nfree++;
    if ((poolPtr->nused + poolPtr->nfree) <= poolPtr->max) {
        proxyPtr->nextPtr = poolPtr->firstPtr;
        poolPtr->firstPtr = proxyPtr;
        proxyPtr = NULL;
    } else {
        poolPtr->nfree--;
    }
    Ns_CondBroadcast(&poolPtr->cond);
    Ns_MutexUnlock(&poolPtr->lock);
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

    if (proxyPtr->state == Idle && proxyPtr->poolPtr->reinit != NULL) {
        result = Eval(interp, proxyPtr, proxyPtr->poolPtr->reinit, -1);
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

    ResetProxy(proxyPtr);
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
RunProxyCmd(ClientData clientData, Tcl_Interp *interp, int objc,
            Tcl_Obj *CONST objv[])
{
    Proxy *proxyPtr = (Proxy *)clientData;
    int ms;

    if (objc < 2 && objc > 3) {
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
    Proxy          *proxyPtr;

    hPtr = Tcl_FirstHashEntry(&idataPtr->ids, &search);
    while (hPtr != NULL) {
        proxyPtr = (Proxy *)Tcl_GetHashValue(hPtr);
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
 * ResetProxy --
 *
 *	    Reset a proxy for the next request.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    Will close the connection to the proxy process if the state
 *      is not idle.
 *
 *----------------------------------------------------------------------
 */

static void
ResetProxy(Proxy *proxyPtr)
{
    Pool   *poolPtr = proxyPtr->poolPtr;
    Proxy **runPtrPtr;

    if (proxyPtr->runPtr != NULL) {
        Ns_MutexLock(&poolPtr->lock);
        runPtrPtr = &poolPtr->runPtr;
        while (*runPtrPtr != proxyPtr) {
            runPtrPtr = &(*runPtrPtr)->runPtr;
        }
        *runPtrPtr = proxyPtr->runPtr;
        Ns_MutexUnlock(&poolPtr->lock);
        proxyPtr->runPtr = NULL;
    }
    if (proxyPtr->state != Idle) {
        CloseProxy(proxyPtr);
        proxyPtr->state = Idle;
    }
    Tcl_DStringTrunc(&proxyPtr->in, 0);
    Tcl_DStringTrunc(&proxyPtr->out, 0);
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
    }

    Tcl_SetErrorCode(interp, "NSPROXY", code, msg, sysmsg, NULL);
    return msg;
}
