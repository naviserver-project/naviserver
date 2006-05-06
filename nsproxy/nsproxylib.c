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

#include <poll.h>

#define MAJOR_VERSION 1
#define MINOR_VERSION 1

/*
 * The following structure defines a running proxy slave process.
 */

typedef struct Proc {
    struct Pool *poolPtr;
    struct Proc *nextPtr;
    int   rfd;
    int   wfd;
    pid_t pid;
    Ns_Time expire;
} Proc;

/*
 * The following structures defines a proxy request and response.
 * The lengths are in network order to support later proxy
 * operation over a socket connection.
 */

typedef struct Req {
    uint32_t len;
    uint16_t major;
    uint16_t minor;
} Req;

typedef struct Res {
    uint32_t code;
    uint32_t clen;
    uint32_t ilen;
    uint32_t rlen;
} Res;

/*
 * The following structure defines a proxy connection allocated
 * from a pool.
 */

typedef struct Proxy {
    struct Proxy *nextPtr;  /* Next in list of avail proxies. */
    struct Proxy *runPtr;   /* Next in list of active proxies. */
    struct Pool *poolPtr;   /* Pointer to proxy's pool. */
    char id[16];            /* Proxy unique string id. */
    Proc *procPtr;          /* Running slave, if any. */
    Tcl_HashEntry *idPtr;   /* Pointer to proxy table entry. */
    Tcl_HashEntry *cntPtr;  /* Pointer to count of proxies allocated. */
    Tcl_DString in;         /* Request dstring. */ 
    Tcl_DString out;        /* Response dstring. */
    Tcl_Command cmdToken;   /* Proxy Tcl command */
    Tcl_Interp *interp;     /* Interp holding the Tcl command */
} Proxy;

/*
 * The following structure defines a proxy pool.
 */

typedef struct Pool {
    char *name;             /* Name of pool. */
    struct Proxy *firstPtr; /* First in list of avail proxies. */
    struct Proxy *runPtr;   /* First in list of active proxies. */
    char *exec;             /* Slave executable. */
    char *init;             /* Init script to eval on proxy start. */
    char *reinit;           /* Re-init scripts to eval on proxy put. */
    int   waiting;          /* Thread waiting for handles. */
    int   max;              /* Max # of proxy handles. */
    int   min;              /* Min # of proxy handles. */
    int   avail;            /* Current # of available proxy handles. */
    int   nextid;           /* Next in proxy unique ids. */
    int   tget;
    int   teval;
    int   tsend;
    int   trecv;
    int   twait;
    int   tidle;
    Ns_Mutex lock;          /* Lock around pool. */
    Ns_Cond cond;           /* Cond for use while allocating handles. */
} Pool;

/*
 * The following structure is allocated per-interp to manage
 * the currently allocated handles.
 */

typedef struct InterpData {
    Tcl_HashTable ids;
    Tcl_HashTable cnts;
} InterpData;

typedef enum {
    stopped,
    running,
    stopping
} ReaperState;

/*
 * Static functions defined in this file.
 */

static Tcl_ObjCmdProc ProxyObjCmd;
static Tcl_ObjCmdProc ConfigObjCmd;
static Tcl_ObjCmdProc RunProxyCmd;
static Tcl_ObjCmdProc GetObjCmd;
static Tcl_CmdDeleteProc DelProxyCmd;

static Tcl_InterpDeleteProc DeleteData;

static void  PutProxy(Proxy *proxyPtr);
static int   GetProxy(InterpData *idataPtr, char *proxyId, Proxy **proxyPtrPtr);
static int   CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr);
static int   ReleaseProxy(Tcl_Interp *interp, Proxy *proxyPtr);
static void  CloseProxy(Proxy *proxyPtr);
static void  FreeProxy(Proxy *proxyPtr);

static Pool *GetPool(InterpData *idataPtr, char *poolName);
static void  FreePool(Pool *poolPtr);

static void  ReleaseHandles(Tcl_Interp *interp, InterpData *idataPtr);
static Proc *Exec(Tcl_Interp *interp, Proxy *proxyPtr);
static int   Call(Tcl_Interp *interp, Proxy *proxyPtr, char *script, int ms);

static void  SetExpire(Proc *procPtr);
static int   SendBuf(Proc *procPtr, int ms, Tcl_DString *dsPtr);
static int   RecvBuf(Proc *procPtr, int ms, Tcl_DString *dsPtr, int *errnumPtr);
static int   WaitFd(int fd, int events, int ms);

static int   Import(Tcl_Interp *interp, Tcl_DString *dsPtr, int *resultPtr);
static void  Export(Tcl_Interp *interp, int code, Tcl_DString *dsPtr);

static void  UpdateIov(struct iovec *iov, int n);
static void  SetOpt(char *str, char **optPtr);
static void  ReaperThread(void *ignored);
static void  CloseProc(Proc *procPtr);
static void  WakeupReaper(void);

static void  AppendStr(Tcl_Interp *interp, CONST char *flag, char *val);
static void  AppendInt(Tcl_Interp *interp, CONST char *flag, int i);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable pools;    /* Tracks process pools */
static char *assoc = "nsproxy:data"; /* Tracks proxy handles per interp */

ReaperState reaperState = stopped;

static Ns_Cond pcond;          /* Those are used to control access to */
static Ns_Mutex plock;         /* the list of Proc structures of slave */
static Proc *firstClosePtr;    /* processes which are being closed. */

static Ns_DString defexec;     /* Stores full path of the proxy executable */


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

    idataPtr = ns_malloc(sizeof(InterpData));
    Tcl_InitHashTable(&idataPtr->ids, TCL_STRING_KEYS);
    Tcl_InitHashTable(&idataPtr->cnts, TCL_ONE_WORD_KEYS);
    Tcl_SetAssocData(interp, assoc, DeleteData, idataPtr);
    Tcl_CreateObjCommand(interp, "ns_proxy", ProxyObjCmd, idataPtr, NULL);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ProxyMain --
 *
 *      Main loop for nsproxy slave processes.
 *
 * Results:
 *      Always zero.
 *
 * Side effects:
 *      Will initialize Tcl interp and loop processing requests
 *      indefinitely. On communication errors or when the peer
 *      closes it's write-pipe, slave exits gracefully.
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
    uint16_t     major, minor;

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
        Ns_Fatal("nsproxy: dup: '%s'", strerror(errno));
    }
    proc.wfd = dup(1);
    if (proc.wfd < 0) {
        Ns_Fatal("nsproxy: dup: '%s'", strerror(errno));
    }
    close(0);
    if (open("/dev/null", O_RDONLY) != 0) {
        Ns_Fatal("nsproxy: open: '%s'", strerror(errno));
    }
    close(1);
    if (dup(2) != 1) {
        Ns_Fatal("nsproxy: dup: '%s'", strerror(errno));
    }

    /*
     * Create the interp and initialize with user init proc, if any.
     */

    interp = Ns_TclCreateInterp();
    if (init != NULL) {
        if ((*init)(interp) != TCL_OK) {
            Ns_Fatal("nsproxy: init: '%s'", interp->result);
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
     * The code below is a replication from the Ns_Main().
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
                Ns_Fatal("nsproxy: initgroups(%s, %d) failed: '%s'", user,
                         gid, strerror(errno));
            }
        } else {
            if (setgroups(0, NULL) != 0) {
                Ns_Fatal("nsproxy: setgroups(0, NULL) failed: '%s'",
                         strerror(errno));
            }
        }
        if (gid != (int)getgid() && setgid((gid_t)gid) != 0) {
            Ns_Fatal("nsproxy: setgid(%d) failed: '%s'", gid, strerror(errno));
        }
        if (uid != (int)getuid() && setuid((uid_t)uid) != 0) {
            Ns_Fatal("nsproxy: setuid(%d) failed: '%s'", uid, strerror(errno));
        }
    }
    
    /*
     * Loop continuously processing proxy request.
     */

    Tcl_DStringInit(&in);
    Tcl_DStringInit(&out);

    while (RecvBuf(&proc, -1, &in, NULL)) {
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
 *      Tcl trace to release any proxy handles held.
 *
 * Results:
 *      TCL_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ProxyCleanup(Tcl_Interp *interp, void *ignored)
{
    InterpData *idataPtr = Tcl_GetAssocData(interp, assoc, NULL);

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
 *      Server trace to shitdown proxy system including stopping 
 *      the reaper thread.
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
Ns_ProxyExit(void *ignored)
{
    Proc           *procPtr;
    Pool           *poolPtr;
    Proxy          *proxyPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    int             start;

    /*
     * Cleanup all known pools. This will put all idle
     * slaves on the close list. At this point, there
     * should be no running nor detached proxies.
     */

    Ns_MutexLock(&plock);
    hPtr = Tcl_FirstHashEntry(&pools, &search);
    while (hPtr != NULL) {
        poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
        Ns_MutexLock(&poolPtr->lock);
        proxyPtr = poolPtr->firstPtr;
        while (proxyPtr != NULL) {
            procPtr = proxyPtr->procPtr;
            if (procPtr) {
                CloseProc(procPtr);
                FreeProxy(proxyPtr);
            }
            proxyPtr = proxyPtr->nextPtr;
        }
        Ns_MutexUnlock(&poolPtr->lock);
        FreePool(poolPtr);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    start = reaperState == stopped && firstClosePtr != NULL;
    Ns_MutexUnlock(&plock);

    /*
     * If the close thread is not running start it now
     * so it can close slaves placed on the close list
     * by the code above.
     */

    if (start) {
        WakeupReaper();
    }

    /*
     * Wait for close thread to close all slaves and exit.
     */

    Ns_MutexLock(&plock);
    if (reaperState == running) {
        reaperState = stopping;
        Ns_CondSignal(&pcond);
        while (reaperState != stopped) {
            Ns_CondWait(&pcond, &plock);
        }
    }
    Ns_MutexUnlock(&plock);
}


/*
 *----------------------------------------------------------------------
 *
 * Exec --
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
Exec(Tcl_Interp *interp, Proxy *proxyPtr)
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

    procPtr = ns_malloc(sizeof(Proc));
    procPtr->poolPtr = proxyPtr->poolPtr;
    procPtr->nextPtr = NULL;
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

static void SetExpire(Proc *procPtr) {

    Pool *poolPtr = procPtr->poolPtr;
    int idle = poolPtr->tidle;

    if (idle > 0) {
        Ns_GetTime(&procPtr->expire);
        Ns_IncrTime(&procPtr->expire, idle/1000, idle%1000);
    } else {
        procPtr->expire.sec = INT_MAX;
        procPtr->expire.usec = LONG_MAX;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Call --
 *
 *      Invoke a proxy call.
 *
 * Results:
 *      Depends on script.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Call(Tcl_Interp *interp, Proxy *proxyPtr, char *script, int ms)
{
    Pool   *poolPtr = proxyPtr->poolPtr;
    Proc   *procPtr = proxyPtr->procPtr;
    Proxy **runPtrPtr;
    int     len, result, errnum;
    char   *code, *err;
    Req     req;

    if (ms < 0) {
        ms = poolPtr->teval;
    }
    if (procPtr == NULL) {
        code = "DEAD";
        err = "no running process";
    } else {
        len = script ? strlen(script) : 0;
        req.len = htonl(len);
        req.major = htons(MAJOR_VERSION);
        req.minor = htons(MINOR_VERSION);
        Tcl_DStringAppend(&proxyPtr->in, (char *) &req, sizeof(req));
        if (len > 0) {
            Tcl_DStringAppend(&proxyPtr->in, script, len);
        }
        Ns_MutexLock(&poolPtr->lock);
        proxyPtr->runPtr = poolPtr->runPtr;
        poolPtr->runPtr = proxyPtr;
        Ns_MutexUnlock(&poolPtr->lock);
        if (!SendBuf(procPtr, poolPtr->tsend, &proxyPtr->in)) {
            code = "SENDFAIL";
            err = strerror(errno);
        } else if (ms > 0 && !WaitFd(procPtr->rfd, POLLIN, ms)) {
            code = "TIMEOUT";
            err = "timeout waiting for response";
        } else if (!RecvBuf(procPtr, poolPtr->trecv, &proxyPtr->out, &errnum)) {
            code = "RECVFAIL";
            if (errnum == 0) {
                err = "pipe closed";
            } else {
                err = strerror(errnum);
            }
        } else if (Import(interp, &proxyPtr->out, &result) != TCL_OK) {
            code = "INVALID";
            err = "invalid proxy response";
        } else {
            code = NULL;
        }
        Tcl_DStringTrunc(&proxyPtr->in, 0);
        Tcl_DStringTrunc(&proxyPtr->out, 0);
        Ns_MutexLock(&poolPtr->lock);
        runPtrPtr = &poolPtr->runPtr;
        while (*runPtrPtr != proxyPtr) {
            runPtrPtr = &(*runPtrPtr)->runPtr;
        }
        *runPtrPtr = proxyPtr->runPtr;
        proxyPtr->runPtr = NULL;
        Ns_MutexUnlock(&poolPtr->lock);
    }
    if (code != NULL) {
        CloseProxy(proxyPtr);
        Tcl_SetErrorCode(interp, "NSPROXY", code, err, NULL);
        Tcl_AppendResult(interp, "proxy call failed: ", err, NULL);
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
    uint32_t     ulen;
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
RecvBuf(Proc *procPtr, int ms, Tcl_DString *dsPtr, int *errnumPtr)
{
    uint32_t      ulen;
    struct iovec  iov[2];
    char         *ptr;
    int           n, len, avail;

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
            goto err;
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
            goto err;
        }
        len -= n;
        ptr += n;
    }

    return 1;

 err:
    if (errnumPtr != NULL) {
        *errnumPtr = errno;
    }

    return 0;
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
    pfd.events = event;
    pfd.revents = 0;
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
 *      Tcl result code from remote.
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
    int            ms, result = TCL_OK;
    char          *proxyId;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    static CONST char *opts[] = {
        "get", "put", "release", "eval", "cleanup", "config", "ping",
        "active", "handles", NULL
    };
    enum {
        PGetIdx, PPutIdx, PReleaseIdx, PEvalIdx, PCleanupIdx, PConfigIdx,
        PPingIdx, PActiveIdx, PHandlesIdx
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
            result = Call(interp, proxyPtr, NULL, -1);
        }
        break;

    case PConfigIdx:
        result = ConfigObjCmd(data, interp, objc, objv);
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
        result = Call(interp, proxyPtr, Tcl_GetString(objv[3]), ms);
        break;

    case PActiveIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "pool");
            return TCL_ERROR;
        }
        poolPtr = GetPool(idataPtr, proxyId);
        Ns_MutexLock(&poolPtr->lock);
        proxyPtr = poolPtr->runPtr;
        while (proxyPtr != NULL) {
            Tcl_AppendElement(interp, proxyPtr->id);
            Tcl_AppendElement(interp, proxyPtr->in.string + sizeof(Req));   
            proxyPtr = proxyPtr->runPtr;
        }
        Ns_MutexUnlock(&poolPtr->lock);
        break;

    case PHandlesIdx:
        hPtr = Tcl_FirstHashEntry(&idataPtr->ids, &search);
        while (hPtr != NULL) {
            proxyPtr = (Proxy *)Tcl_GetHashValue(hPtr);
            Tcl_AppendElement(interp, proxyPtr->id);
            hPtr = Tcl_NextHashEntry(&search);
        }
        break;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ConfigObjCmd --
 *
 *      Sub-command to configure a proxy.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Will update on or more config options.
 *
 *----------------------------------------------------------------------
 */

static int
ConfigObjCmd(ClientData data, Tcl_Interp *interp, int objc, 
             Tcl_Obj *CONST objv[])
{
    InterpData *idataPtr = data;
    Pool       *poolPtr;
    Proc       *procPtr;
    Proxy      *proxyPtr;
    char       *str;
    int         i, incr, n, nrun, result, wakeup = 0;

    static CONST char *flags[] = {
        "-init", "-reinit", "-min", "-max", "-exec", "-getimeout", 
        "-evaltimeout", "-sendtimeout", "-recvtimeout", "-waittimeout",
        "-idletimeout", NULL
    };
    enum {
        CInitIdx, CReinitIdx, CMinIdx, CMaxIdx, CExecIdx, CGetIdx,
        CEvalIdx, CSendIdx, CRecvIdx, CWaitIdx, CIdleIdx
    } flag;

    if (objc < 3 || (objc % 2) != 1) {
        Tcl_WrongNumArgs(interp, 2, objv, "pool ?-opt val -opt val ...?");
        return TCL_ERROR;
    }
    result = TCL_ERROR;
    poolPtr = GetPool(idataPtr, Tcl_GetString(objv[2]));
    Ns_MutexLock(&poolPtr->lock);
    nrun = poolPtr->max - poolPtr->avail;
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
                break;
            case CMaxIdx:
                poolPtr->max = n;
                break;
            case CIdleIdx:
                poolPtr->tidle = n;
                proxyPtr = poolPtr->firstPtr;
                while (proxyPtr != NULL) {
                    procPtr = proxyPtr->procPtr;
                    if (procPtr) {
                        SetExpire(procPtr);
                    }
                    proxyPtr = proxyPtr->nextPtr;
                }
                wakeup = 1;
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
     * Adjust limits and dump any lingering proxies.  Note
     * "avail" can be negative if "max" was adjusted down
     * below the number of currently running proxies.  This
     * will be corrected as those active proxies are returned.
     */

    if (poolPtr->min > poolPtr->max) {
        poolPtr->min = poolPtr->max;
    }
    poolPtr->avail = poolPtr->max - nrun;
    while ((proxyPtr = poolPtr->firstPtr) != NULL) {
        poolPtr->firstPtr = proxyPtr->nextPtr;
        CloseProxy(proxyPtr);
        FreeProxy(proxyPtr);
    }

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
    result = TCL_OK;

 err:
    Ns_MutexUnlock(&poolPtr->lock);

    if (wakeup) {
        WakeupReaper();
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
    Tcl_AppendElement(interp, (char *)flag);
    Tcl_AppendElement(interp, val ? val : "");
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
    int            i, status, new, nwant, n, ms, ok;
    Ns_Time        timeout;
    char           *arg, *err;
    Pool           *poolPtr;

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
    poolPtr = GetPool(idataPtr, Tcl_GetString(objv[2]));
    cntPtr = Tcl_CreateHashEntry(&idataPtr->cnts, (char *) poolPtr, &new);
    if ((int)Tcl_GetHashValue(cntPtr) > 0) {
        err = "interp already owns handles";
        Tcl_AppendResult(interp, err, " from pool \"", poolPtr->name, "\"", NULL);
        Tcl_SetErrorCode(interp, "NSPROXY", "DEADLOCK", err, NULL);
        return TCL_ERROR;
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
     * Wait to be the exclusive handle waiter and then wait for the
     * handles.
     */

    Ns_GetTime(&timeout);
    Ns_IncrTime(&timeout, 0, ms * 1000);
    err = NULL;
    status = NS_OK;
    Ns_MutexLock(&poolPtr->lock);
    while (status == NS_OK && poolPtr->waiting) {
        status = Ns_CondTimedWait(&poolPtr->cond, &poolPtr->lock, &timeout);
    }
    if (status != NS_OK) {
        err = "queue timeout";
    } else {
        poolPtr->waiting = 1;
        while (status == NS_OK
               && poolPtr->max >= nwant
               && poolPtr->avail < nwant) {
            status = Ns_CondTimedWait(&poolPtr->cond, &poolPtr->lock, &timeout);
        }
        if (poolPtr->max == 0) {
            err = "pool disabled";
        } else if (poolPtr->max < nwant) {
            err = "insufficient handles";
        } else if (status != NS_OK) {
            err = "proxy timeout";
        } else {
            firstPtr = NULL;
            poolPtr->avail -= nwant;
            for (i = 0; i < nwant; ++i) {
                proxyPtr = poolPtr->firstPtr;
                if (proxyPtr != NULL) {
                    poolPtr->firstPtr = proxyPtr->nextPtr;
                } else {
                    proxyPtr = ns_malloc(sizeof(Proxy));
                    memset(proxyPtr, 0, sizeof(Proxy));
                    sprintf(proxyPtr->id, "proxy%d", poolPtr->nextid++);
                    proxyPtr->poolPtr = poolPtr;
                    Tcl_DStringInit(&proxyPtr->in);
                    Tcl_DStringInit(&proxyPtr->out);
                }
                proxyPtr->nextPtr = firstPtr;
                firstPtr = proxyPtr;
            }
        }
        poolPtr->waiting = 0;
        Ns_CondBroadcast(&poolPtr->cond);
    }
    Ns_MutexUnlock(&poolPtr->lock);
    if (err != NULL) {
        Tcl_AppendResult(interp, "could not allocate from pool \"",
                         poolPtr->name, "\": ", err, NULL);
        Tcl_SetErrorCode(interp, "NSPROXY", "NOHANDLE", err, NULL);
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

    ok = 1;
    proxyPtr = firstPtr;
    while (ok && proxyPtr != NULL) {
        ok = CheckProxy(interp, proxyPtr);
        proxyPtr = proxyPtr->nextPtr;
    }
    if (!ok) {
        while ((proxyPtr = firstPtr) != NULL) {
            firstPtr = proxyPtr->nextPtr;
            PutProxy(proxyPtr);
        }
        Ns_CondBroadcast(&poolPtr->cond);
        return TCL_ERROR;
    }

    /*
     * Generate accessor commands for the returned proxies.
     */

    proxyPtr = firstPtr;
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
 * GetPool --
 *
 *      Get a pool by name.
 *
 * Results:
 *      1 if pool found, 0 on no such pool.
 *
 * Side effects:
 *      Will update given poolPtrPtr with pointer to Pool.
 *
 *----------------------------------------------------------------------
 */

Pool *
GetPool(InterpData *idataPtr, char *poolName)
{
    Tcl_HashEntry *hPtr;
    Pool          *poolPtr;
    int            new;

    Ns_MutexLock(&plock);
    hPtr = Tcl_CreateHashEntry(&pools, poolName, &new);
    if (!new) {
        poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
    } else {
        poolPtr = ns_calloc(1, sizeof(Pool));
        Tcl_SetHashValue(hPtr, poolPtr);
        poolPtr->name  = Tcl_GetHashKey(&pools, hPtr);
        poolPtr->teval = 500;
        poolPtr->tget  = 500;
        poolPtr->tsend = 100;
        poolPtr->trecv = 100;
        poolPtr->twait = 100;
        poolPtr->tidle = 0;
        poolPtr->max   = 5;
        poolPtr->avail = 5;
        SetOpt(defexec.string, &poolPtr->exec);
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
 * GetProxy --
 *
 *      Get a previously allocated proxy handle.
 *
 * Results:
 *      1 if handle found, 0 on no such handle.
 *
 * Side effects:
 *      Will update given proxyPtrPtr with pointer to handle.
 *
 *----------------------------------------------------------------------
 */

int
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

static int
CheckProxy(Tcl_Interp *interp, Proxy *proxyPtr)
{
    Pool *poolPtr = proxyPtr->poolPtr;

    if (proxyPtr->procPtr != NULL 
        && Call(interp, proxyPtr, NULL, -1) != TCL_OK) {
        CloseProxy(proxyPtr);
        Tcl_ResetResult(interp);
    }
    if (proxyPtr->procPtr == NULL) {
        proxyPtr->procPtr = Exec(interp, proxyPtr);
        if (proxyPtr->procPtr == NULL) {
            Tcl_SetErrorCode(interp, "NSPROXY", "EXEC",
                             "process exec failed", strerror(errno), NULL);
        } else if (proxyPtr->poolPtr->init != NULL
                   && Call(interp, proxyPtr, poolPtr->init, -1) != TCL_OK) {
            CloseProxy(proxyPtr);
            Tcl_AddErrorInfo(interp, "\n    (during process init)");
        }
    }

    return (proxyPtr->procPtr == NULL) ? 0 : 1;
}

/*
 *----------------------------------------------------------------------
 *
 * CloseProc --
 *
 *      Close the given proc handle (sssumes caller holds the plock)
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Puts the proc structure to the close list so the close thread
 *      can eventually close it.
 *
 *----------------------------------------------------------------------
 */

static void
CloseProc(Proc *procPtr)
{
    close(procPtr->wfd);
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
    Proc *procPtr = proxyPtr->procPtr;

    if (procPtr != NULL) { 
        Ns_MutexLock(&plock);
        CloseProc(procPtr);
        proxyPtr->procPtr = NULL;
        Ns_MutexUnlock(&plock);
        WakeupReaper();
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
Kill(Proc *procPtr, int sig)
{
    Ns_Log(Warning, "[%s]: pid %d won't die - sending signal %d",
           procPtr->poolPtr->name, procPtr->pid, sig);
    if (kill(procPtr->pid, sig) != 0 && errno != ESRCH) {
        Ns_Log(Error, "kill(%d, %d) failed: %s", procPtr->pid, sig,
               strerror(errno));
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
    Proxy          *proxyPtr;
    Pool           *poolPtr;
    Proc           *procPtr;
    Ns_Time         tout, now, diff;
    int             ms, zombie;

    Ns_MutexLock(&plock);

    reaperState = running;
    Ns_CondSignal(&pcond); /* Wakeup starter thread */

    Ns_ThreadSetName("-nsproxy:reap-");
    Ns_Log(Notice, "starting");

    while (1) {

        Ns_GetTime(&now);
        
        if (firstClosePtr == NULL) {

            /*
             * Calculate maximum time to wait for any slave
             * to become idle (i.e. its idle timer expires)
             */

            tout.sec = INT_MAX;
            tout.usec = LONG_MAX;
            hPtr = Tcl_FirstHashEntry(&pools, &search);
            while (hPtr != NULL) {

                /*
                 * Get max time to wait for the whole pool
                 */

                poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
                Ns_MutexLock(&poolPtr->lock);
                if (poolPtr->tidle) {
                    diff = now;
                    Ns_IncrTime(&diff,poolPtr->tidle/1000,poolPtr->tidle%1000);
                    if (Ns_DiffTime(&diff, &tout, NULL) < 0) {
                        tout = diff;
                    }
                }

                /*
                 * Get max time to wait for one of the slaves.
                 * This is less then time for the whole pool.
                 * Expired slaves are closed immediately.
                 */                

                proxyPtr = poolPtr->firstPtr;
                while (proxyPtr != NULL) {
                    procPtr = proxyPtr->procPtr;
                    if (procPtr) {
                        if (Ns_DiffTime(&procPtr->expire, &now, NULL) <= 0) {
                            tout = now;
                            CloseProc(procPtr);
                            proxyPtr->procPtr = NULL;
                        } else if (   Ns_DiffTime(&now, &tout, NULL) != 0
                                   && Ns_DiffTime(&procPtr->expire, &tout,
                                                  NULL) < 0) {
                            tout = procPtr->expire;
                        }
                    }
                    proxyPtr = proxyPtr->nextPtr;
                }
                Ns_MutexUnlock(&poolPtr->lock);
                hPtr = Tcl_NextHashEntry(&search);
            }
            if (Ns_DiffTime(&tout, &now, &diff) > 0) {

                /*
                 * No slaves expired yet. Wait for somebody
                 * to close any other slave or until some 
                 * of them get expired by themselves.
                 */

                if (reaperState == stopping) {
                    break;
                }
                Ns_CondTimedWait(&pcond, &plock, &tout);
            }
        }

        if (firstClosePtr == NULL) {

            /*
             * Go look which slaves expired and put them
             * on the close list.
             */

            hPtr = Tcl_FirstHashEntry(&pools, &search);
            while (hPtr != NULL) {
                poolPtr = (Pool *)Tcl_GetHashValue(hPtr);
                Ns_MutexLock(&poolPtr->lock);
                proxyPtr = poolPtr->firstPtr;
                while (proxyPtr != NULL) {
                    procPtr = proxyPtr->procPtr;
                    if (procPtr) {
                        if (Ns_DiffTime(&procPtr->expire, &now, NULL) <= 0) {
                            CloseProc(procPtr);
                            proxyPtr->procPtr = NULL;
                        }
                    }
                    proxyPtr = proxyPtr->nextPtr;
                }
                Ns_MutexUnlock(&poolPtr->lock);
                hPtr = Tcl_NextHashEntry(&search);
            }
            continue;
        }

        /*
         * Slave is on the close list, close it.
         * Release mutex since this may take time.
         */

        procPtr = firstClosePtr;
        firstClosePtr = procPtr->nextPtr;

        Ns_MutexUnlock(&plock);
        ms = procPtr->poolPtr->twait;
        zombie = 0;
        if (!WaitFd(procPtr->rfd, POLLIN, ms)) {
            Kill(procPtr, 15 /* SIGTERM */);
            if (!WaitFd(procPtr->rfd, POLLIN, ms)) {
                Kill(procPtr, 9 /* SIGKILL */);
                if (!WaitFd(procPtr->rfd, POLLIN, ms)) {
                    zombie = 1;
                }
            }
        }
        close(procPtr->rfd);
        if (!zombie) {
            Ns_WaitProcess(procPtr->pid);
        } else {
            Ns_Log(Warning, "zombie: %d", procPtr->pid);
        }
        ns_free(procPtr);
        Ns_MutexLock(&plock);
    }

    reaperState = stopped;
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
 *      Disposes a pool handle.
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
    Ns_CondDestroy(&poolPtr->cond);
    Ns_MutexDestroy(&poolPtr->lock);

    if (poolPtr->exec) {
        ns_free(poolPtr->exec);
    }
    if (poolPtr->init) {
        ns_free(poolPtr->init);
    }
    if (poolPtr->reinit) {
        ns_free(poolPtr->reinit);
    }

    ns_free(poolPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * PutProxy --
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
PutProxy(Proxy *proxyPtr)
{
    Pool    *poolPtr = proxyPtr->poolPtr;
    Proxy  **nextPtrPtr;
    int      nhave;

    nhave = (int)Tcl_GetHashValue(proxyPtr->cntPtr);
    nhave--;
    Tcl_SetHashValue(proxyPtr->cntPtr, nhave);
    Tcl_DeleteHashEntry(proxyPtr->idPtr);
    proxyPtr->idPtr  = NULL;
    proxyPtr->cntPtr = NULL;

    Ns_MutexLock(&poolPtr->lock);
    ++poolPtr->avail;
    if (poolPtr->avail > 0) {
        if (proxyPtr->procPtr) {
            SetExpire(proxyPtr->procPtr);
        }
        nextPtrPtr = &poolPtr->firstPtr;
        while (*nextPtrPtr != NULL) {
            nextPtrPtr = &((*nextPtrPtr)->nextPtr);
        }
        proxyPtr->nextPtr = NULL;
        *nextPtrPtr = proxyPtr;
        proxyPtr = NULL;
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

    if (proxyPtr->poolPtr->reinit != NULL) {
        result = Call(interp, proxyPtr, proxyPtr->poolPtr->reinit, -1);
    }
    if (proxyPtr->cmdToken) {
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

    PutProxy(proxyPtr);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * RunProxyCmd --
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
    
    return Call(interp, proxyPtr, Tcl_GetString(objv[1]), ms);
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
     * This prevents the ReleaseProxy to attempt to
     * delete accessor Tcl command, which would call
     * us recursively.
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
 * WakeupReaper --
 *
 *      Wakes up the reaper thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will start the close thread if not already running.
 *
 *----------------------------------------------------------------------
 */

static void
WakeupReaper(void)
{
    Ns_MutexLock(&plock);
    if (reaperState == running) {
        Ns_CondSignal(&pcond);
    } else {
        Ns_ThreadCreate(ReaperThread, NULL, 0, NULL);
        while (reaperState != running) {
            Ns_CondWait(&pcond, &plock);
        }
    }
    Ns_MutexUnlock(&plock);
}
