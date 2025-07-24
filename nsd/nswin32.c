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

#ifdef _WIN32

/*
 * win32.c --
 *
 *  Win32 specific routines.
 */

#include "nsd.h"
#include <share.h>

static Ns_ThreadProc ServiceTicker;
static void StopTicker(void);
static void StartTicker(DWORD pending);
static VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
static VOID WINAPI ServiceHandler(DWORD code);
static BOOL WINAPI ConsoleHandler(DWORD code);
static void ReportStatus(DWORD state, DWORD code, DWORD hint);
static char *GetServiceName(Tcl_DString *dsPtr, char *service);
static bool SockAddrEqual(const struct sockaddr *saPtr1, const struct sockaddr *saPtr2);


/*
 * Static variables used in this file
 */

static Ns_Mutex lock = NULL;
static Ns_Cond cond = NULL;
static Ns_Thread tickThread;
static SERVICE_STATUS_HANDLE hStatus = NULL;
static SERVICE_STATUS curStatus;
static Ns_Tls tls;
static bool serviceRunning = NS_FALSE;
static bool serviceFailed = NS_FALSE;
static int tick = 0;
static unsigned int sigpending = 0u;


#define SysErrMsg() (NsWin32ErrMsg(GetLastError()))


/*
 *----------------------------------------------------------------------
 *
 * NsBlockSignal --
 *
 *      Mask one specific signal.
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
NsBlockSignal(int UNUSED(sig))
{
    return;
}


/*
 *----------------------------------------------------------------------
 *
 * NsUnblockSignal --
 *
 *      Restores one specific signal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Signal will be unblock.
 *
 *----------------------------------------------------------------------
 */

void
NsUnblockSignal(int UNUSED(sig))
{
    return;
}

int
Ns_SetGroup(const char *UNUSED(group))
{
    return -1;
}

int
Ns_SetUser(const char *UNUSED(user))
{
    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * DllMain --
 *
 *      Init routine for the nsd.dll which setups TLS for Win32 errors
 *      disables thread attach/detach calls.
 *
 * Results:
 *      TRUE or FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

BOOL APIENTRY
DllMain(HANDLE hModule, DWORD why, LPVOID UNUSED(lpReserved))
{
    BOOL    result = TRUE;
    WSADATA wsd;

    if (why == (DWORD)DLL_PROCESS_ATTACH) {
        Ns_TlsAlloc(&tls, ns_free);
        if (WSAStartup((WORD)MAKEWORD(1, 1), &wsd) != 0) {
            result = FALSE;
        } else {
            DisableThreadLibraryCalls(hModule);
            Nsd_LibInit();
        }
    } else if (why == (DWORD)DLL_PROCESS_DETACH) {
        WSACleanup();
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsWin32ErrMsg --
 *
 *      Get a string message for an error code in either the kernel or
 *      wsock dll's.
 *
 * Results:
 *      Pointer to per-thread LocalAlloc'ed memory.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
NsWin32ErrMsg(ns_sockerrno_t err)
{
    char  *msg;
    size_t len;

    msg = Ns_TlsGet(&tls);
    if (msg == NULL) {
        msg = ns_malloc(1000u);
        Ns_TlsSet(&tls, msg);
    }
    snprintf(msg, 1000u, "win32 error code: %lu: ", err);
    len = strlen(msg);

    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  msg+len, (DWORD)(1000u - len), NULL);

    return msg;
}


/*
 *----------------------------------------------------------------------
 *
 * NsConnectService --
 *
 *      Attach to the service control manager at startup.
 *
 * Results:
 *      NaviServer status code
 *
 * Side effects:
 *      Service control manager will create a new thread running
 *      ServiceMain().
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsConnectService(void)
{
    SERVICE_TABLE_ENTRY table[2];
    BOOL   ok;
    HANDLE fi;

    /*
     * Close all opened streams at this point
     */

    _fcloseall();

    /*
     * Re-route std streams to null because they will be
     * handled separately, i.e. re-routed to log file in
     * the LogReOpen() function. The stdin remains bound
     * to nul: device forever.
     */

    (void)freopen(DEVNULL, "rt", stdin);
    (void)freopen(DEVNULL, "wt", stdout);
    (void)freopen(DEVNULL, "wt", stderr);

    /*
     * Ensure that stdio handles are correctly set.
     * Fail to do this will result in Tcl library
     * thinking that no stdio handles are defined.
     */

    fi = (HANDLE) _get_osfhandle(_fileno(stdin));
    if (fi != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_INPUT_HANDLE, fi);
    }
    fi = (HANDLE) _get_osfhandle(_fileno(stdout));
    if (fi != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, fi);
    }
    fi = (HANDLE) _get_osfhandle(_fileno(stderr));
    if (fi != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_ERROR_HANDLE, fi);
    }

    Ns_Log(Notice, "nswin32: connecting to service control manager");

    serviceRunning = NS_TRUE;

    table[0].lpServiceName = PACKAGE_NAME;
    table[0].lpServiceProc = ServiceMain;
    table[1].lpServiceName = NULL;
    table[1].lpServiceProc = NULL;

    ok = StartServiceCtrlDispatcher(table);

    if (ok == 0) {
        Ns_Log(Error, "nswin32: StartServiceCtrlDispatcher(): '%s'",
               SysErrMsg());
    }

    return (ok ? NS_OK : NS_ERROR);
}


/*
 *----------------------------------------------------------------------
 *
 * NsRemoveService --
 *
 *      Remove a previously installed service.
 *
 * Results:
 *      NaviServer status code.
 *
 * Side effects:
 *      Service should stop and then disappear from the list in the
 *      services control panel.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsRemoveService(char *service)
{
    SC_HANDLE      hmgr;
    SERVICE_STATUS status;
    Tcl_DString    name;
    BOOL           ok;

    Tcl_DStringInit(&name);
    (void) GetServiceName(&name, service);
    ok = FALSE;
    hmgr = OpenSCManager(NULL, NULL, (DWORD)SC_MANAGER_ALL_ACCESS);
    if (hmgr != NULL) {
        SC_HANDLE hsrv = OpenService(hmgr, name.string, (DWORD)SERVICE_ALL_ACCESS);
        if (hsrv != NULL) {
            ControlService(hsrv, (DWORD)SERVICE_CONTROL_STOP, &status);
            ok = DeleteService(hsrv);
            CloseServiceHandle(hsrv);
        }
        CloseServiceHandle(hmgr);
    }
    if (ok != 0) {
        Ns_Log(Notice, "nswin32: removed service: %s", name.string);
    } else {
        Ns_Log(Error, "nswin32: failed to remove %s service: %s",
               name.string, SysErrMsg());
    }
    Tcl_DStringFree(&name);

    return (ok ? NS_OK : NS_ERROR);
}


/*
 *----------------------------------------------------------------------
 *
 * NsInstallService --
 *
 *      Install as an NT service.
 *
 * Results:
 *      NaviServer status code.
 *
 * Side effects:
 *      Service should appear in the list in the services control panel.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsInstallService(char *service)
{
    SC_HANDLE  hmgr, hsrv;
    bool       ok = FALSE;
    char       nsd[PATH_MAX], config[PATH_MAX];
    Tcl_DString name, cmd;

    if (_fullpath(config, nsconf.configFile, sizeof(config)) == NULL) {
        Ns_Log(Error, "nswin32: invalid config path '%s'", nsconf.configFile);
    } else if (GetModuleFileName(NULL, nsd, sizeof(nsd)) == 0u) {
        Ns_Log(Error, "nswin32: failed to find nsd.exe: '%s'", SysErrMsg());
    } else {
        Tcl_DStringInit(&name);
        Tcl_DStringInit(&cmd);
        Ns_DStringVarAppend(&cmd, "\"", nsd, "\"",
                            " -S -s ", service, " -t \"", config, "\"", NS_SENTINEL);
        (void) GetServiceName(&name, service);
        hmgr = OpenSCManager(NULL, NULL, (DWORD)SC_MANAGER_ALL_ACCESS);
        if (hmgr != NULL) {
            hsrv = CreateService(hmgr, name.string, name.string,
                                 (DWORD)SERVICE_ALL_ACCESS,
                                 (DWORD)SERVICE_WIN32_OWN_PROCESS,
                                 (DWORD)SERVICE_AUTO_START,
                                 (DWORD)SERVICE_ERROR_NORMAL,
                                 cmd.string, NULL, NULL, "TcpIp\0", NULL, NULL);
            if (hsrv != NULL) {
                CloseServiceHandle(hsrv);
                ok = TRUE;
            } else {
                Ns_Log(Error, "nswin32: failed to install service '%s': '%s'",
                       name.string, SysErrMsg());
            }
            CloseServiceHandle(hmgr);
        } else {
            Ns_Log(Error, "nswin32: failed to connect to service manager: %s", SysErrMsg());
        }
        Tcl_DStringFree(&name);
        Tcl_DStringFree(&cmd);
    }

    return (ok ? NS_OK : NS_ERROR);
}


/*
 *----------------------------------------------------------------------
 * NsRestoreSignals --
 *
 *      Noop to avoid ifdefs and make symmetrical to Unix part
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
NsRestoreSignals(void)
{
    return;
}


/*
 *----------------------------------------------------------------------
 *
 * NsHandleSignals --
 *
 *      Loop endlessly, processing HUP signals until a TERM
 *      signal arrives
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
NsHandleSignals(void)
{
    unsigned int sig;

    /*
     * If running as a service, stop the ticker thread and report
     * startup complete. Otherwise, register a handler which will
     * initiate an orderly shutdown on Ctrl-C.
     */

    if (!serviceRunning) {
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    } else {
        StopTicker();
        ReportStatus((DWORD)SERVICE_RUNNING, NO_ERROR, 0u);
    }
    Ns_MutexSetName2(&lock, "ns", "signal");
    Ns_CondInit(&cond);
    do {
        Ns_MutexLock(&lock);
        while (sigpending == 0u) {
            Ns_CondWait(&cond, &lock);
        }
        sig = sigpending;
        sigpending = 0u;
        if ((sig & (unsigned int)(1u << NS_SIGINT)) != 0u) {

           /*
            * Signalize the Service Control Manager
            * to restart the service.
            */

            serviceFailed = NS_TRUE;
        }
        Ns_MutexUnlock(&lock);
        if ((sig & (unsigned int)(1u << NS_SIGHUP)) != 0u) {
            NsRunSignalProcs();
        }
    } while ((sig & (unsigned int)(1u << NS_SIGHUP)) != 0u);

    /*
     * If running as a service, startup the ticker thread again
     * to keep updating status until shutdown is complete.
     */

    if (serviceRunning) {
        StartTicker((DWORD)SERVICE_STOP_PENDING);
    }

    return (int)sig;
}


/*
 *----------------------------------------------------------------------
 *
 * NsSendSignal --
 *
 *      Send a signal to wakeup NsHandleSignals. As on Unix, a signal
 *      sent multiple times is only received once.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Main thread will wakeup.
 *
 *----------------------------------------------------------------------
 */

void
NsSendSignal(int sig)
{
    switch (sig) {
    case NS_SIGTERM:
    case NS_SIGINT:
    case NS_SIGHUP:
        Ns_MutexLock(&lock);
        sigpending |= (1u << sig);
        Ns_CondSignal(&cond);
        Ns_MutexUnlock(&lock);
        break;
    default:
        Ns_Fatal("nswin32: invalid signal: %d", sig);
        break;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsMemMap --
 *
 *      Maps a file to memory.
 *
 * Results:
 *      NaviServer status code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsMemMap(const char *path, size_t size, int mode, FileMap *mapPtr)
{
    HANDLE        hndl, mobj;
    char          name[256];
    Ns_ReturnCode status = NS_OK;

    switch (mode) {
    case NS_MMAP_WRITE:
        hndl = CreateFile(path,
                          (DWORD)(GENERIC_READ|GENERIC_WRITE),
                          (DWORD)(FILE_SHARE_READ|FILE_SHARE_WRITE),
                          NULL,
                          (DWORD)OPEN_EXISTING,
                          (DWORD)FILE_FLAG_WRITE_THROUGH,
                          NULL);
        break;
    case NS_MMAP_READ:
        hndl = CreateFile(path,
                          GENERIC_READ,
                          (DWORD)FILE_SHARE_READ,
                          NULL,
                          (DWORD)OPEN_EXISTING,
                          0u,
                          NULL);
        break;
    default:
        return NS_ERROR;
    }

    if (hndl == NULL || hndl == INVALID_HANDLE_VALUE) {
        Ns_Log(Error, "CreateFile(%s): %ld", path, GetLastError());
        return NS_ERROR;
    }

    snprintf(name, sizeof(name), "MapObj-%s", Ns_ThreadGetName());

    mobj = CreateFileMapping(hndl,
                             NULL,
                             (mode == NS_MMAP_WRITE) ? (DWORD)(PAGE_READWRITE) : (DWORD)(PAGE_READONLY),
                             0u,
                             0u,
                             name);

    if (mobj == NULL || mobj == INVALID_HANDLE_VALUE) {
        Ns_Log(Error, "CreateFileMapping(%s): %ld", path, GetLastError());
        (void)CloseHandle(hndl);
        status = NS_ERROR;

    } else {
        LPCVOID       addr;

        addr = MapViewOfFile(mobj,
                             (mode == NS_MMAP_WRITE) ? (DWORD)(FILE_MAP_WRITE) : (DWORD)(FILE_MAP_READ),
                             0u,
                             0u,
                             size);

        if (addr == NULL) {
            Ns_Log(Warning, "MapViewOfFile(%s): %ld", path, GetLastError());
            (void)CloseHandle(mobj);
            (void)CloseHandle(hndl);
            status = NS_ERROR;

        } else {
            mapPtr->mapobj = (void *) mobj;
            mapPtr->handle = hndl;
            mapPtr->addr   = (void *) addr;
            mapPtr->size   = size;
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsMemUmap --
 *
 *      Unmaps a file.
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
NsMemUmap(const FileMap *mapPtr)
{
    UnmapViewOfFile((LPCVOID)mapPtr->addr);
    (void)CloseHandle((HANDLE)mapPtr->mapobj);
    (void)CloseHandle((HANDLE)mapPtr->handle);
}


/*
 *----------------------------------------------------------------------
 *
 * ns_socknbclose --
 *
 *      Perform a nonblocking socket close via the socket callback
 *      thread.
 *      This is only called by a timeout in Ns_SockTimedConnect.
 *
 * Results:
 *      0 or SOCKET_ERROR.
 *
 * Side effects:
 *      Socket will be closed when writable.
 *
 *----------------------------------------------------------------------
 */

int
ns_socknbclose(NS_SOCKET sock)
{
    int result;

    if (Ns_SockCloseLater(sock) != NS_OK) {
        result = SOCKET_ERROR;
    } else {
        result = 0;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ns_sockdup --
 *
 *      Duplicate a socket. This is used in the old ns_sock Tcl cmds.
 *
 * Results:
 *      New handle to underlying socket.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
ns_sockdup(NS_SOCKET sock)
{
    HANDLE    hp, src, dup;
    NS_SOCKET result;

    src = (HANDLE) sock;
    hp = GetCurrentProcess();
    if (DuplicateHandle(hp, src, hp, &dup, 0u, FALSE, (DWORD)DUPLICATE_SAME_ACCESS) == 0) {
        result = NS_INVALID_SOCKET;
    } else {
        result = (NS_SOCKET) dup;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 * ns_sock_set_blocking --
 *
 *      Set a channel blocking or nonblocking
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Change blocking state of a channel
 *
 *----------------------------------------------------------------------
 */

int
ns_sock_set_blocking(NS_SOCKET sock, bool blocking)
{
    u_long state = (!blocking) ? 1u : 0u;

    return ioctlsocket(sock, (long)FIONBIO, &state);
}


/*
 *----------------------------------------------------------------------
 *
 * ns_pipe --
 *
 *      Create a pipe marked close-on-exec.
 *
 * Results:
 *      0 if ok, -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
ns_pipe(int *fds)
{
    return _pipe(fds, 4096u, _O_NOINHERIT|_O_BINARY);
}


/*
 *----------------------------------------------------------------------
 *
 * ns_mkstemp --
 *
 *      Create a temporary file based on the provided character template and
 *      return its fd.  This is a cheap replacement for mkstemp() under
 *      unix-like systems.
 *
 * Results:
 *      fd if ok, NS_INVALID_FD on error.
 *
 * Side effects:
 *      Opens a temporary file.
 *
 *----------------------------------------------------------------------
 */

int
ns_mkstemp(char *charTemplate)
{
    int err, fd = NS_INVALID_FD;

    err = _mktemp_s(charTemplate, strlen(charTemplate)+1);

    if (err == 0) {
        /*
         * We had for a while _O_TEMPORARY here as well, which deletes the
         * file, when he file when the last file descriptor is
         * closed. It is removed here for compatibility reasons.
         *
         * Note that O_TMPFILE (since Linux 3.11) has different semantics.
         */
        err = _sopen_s(&fd, charTemplate,
                       O_RDWR | O_CREAT | O_EXCL,
                       _SH_DENYRW,
                       _S_IREAD | _S_IWRITE);
    }

    if (err != 0) {
        fd = NS_INVALID_FD;
    }

    return fd;
}


/*
 *----------------------------------------------------------------------
 *
 * SockAddrEqual --
 *
 *      Compare two sockaddr structures. This is just a helper for ns_sockpair
 *      (we have here a windows only version based um u.Word, see
 *      https://msdn.microsoft.com/en-us/library/windows/desktop/ms738560%28v=vs.85%29.aspx )
 *
 * Results:
 *      NS_TRUE if the two structures are equal
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
SockAddrEqual(const struct sockaddr *saPtr1, const struct sockaddr *saPtr2)
{
    bool equal = NS_TRUE;

#ifdef HAVE_IPV6
    if (saPtr1->sa_family != saPtr2->sa_family) {
        /*
         * Different families.
         */
        equal = NS_FALSE;
    } else if (saPtr1->sa_family == AF_INET) {
        /*
         * IPv4, we can directly compare the IP addresses.
         */
        if (((struct sockaddr_in *)saPtr1)->sin_addr.s_addr !=
            ((struct sockaddr_in *)saPtr2)->sin_addr.s_addr) {
            equal = NS_FALSE;
        }
    } else if (saPtr1->sa_family == AF_INET6) {
        const struct in6_addr *sa1Bits = &(((struct sockaddr_in6 *)saPtr1)->sin6_addr);
        const struct in6_addr *sa2Bits = &(((struct sockaddr_in6 *)saPtr2)->sin6_addr);
        int i;

        /*
         * Compare the eight words
         */
        for (i = 0; i < 8; i++) {
            if (sa1Bits->u.Word[i] != sa2Bits->u.Word[i]) {
                equal = NS_FALSE;
                break;
            }
        }
    } else {
        equal = NS_FALSE;
    }
#else
    /*
     * Handle here just IPv4.
     */
    if (((struct sockaddr_in *)saPtr1)->sin_addr.s_addr !=
        ((struct sockaddr_in *)saPtr2)->sin_addr.s_addr) {
        equal = NS_FALSE;
    }
#endif
    return equal;
}


/*
 *----------------------------------------------------------------------
 *
 * ns_sockpair --
 *
 *      Create a pair of connected sockets via brute force.
 *      Sock pairs are used as trigger pipes in various subsystems.
 *
 * Results:
 *      0 if ok, -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
ns_sockpair(NS_SOCKET socks[2])
{
    NS_SOCKET sock;
    struct NS_SOCKADDR_STORAGE ia[2];
    int size, result = 0;

    size = (int)sizeof(struct NS_SOCKADDR_STORAGE);
    sock = Ns_SockListen(NS_IP_LOOPBACK, 0);
    if (sock == NS_INVALID_SOCKET ||
        getsockname(sock, (struct sockaddr *) &ia[0], &size) != 0) {
        result = -1;
    } else {
        size = (int)sizeof(struct NS_SOCKADDR_STORAGE);
        socks[1] = Ns_SockConnect(NS_IP_LOOPBACK, Ns_SockaddrGetPort((struct sockaddr *)&ia[0]));
        if (socks[1] == NS_INVALID_SOCKET ||
            getsockname(socks[1], (struct sockaddr *) &ia[1], &size) != 0) {
            ns_sockclose(sock);
            result = -1;
        } else {
            size = (int)sizeof(struct NS_SOCKADDR_STORAGE);
            socks[0] = accept(sock, (struct sockaddr *) &ia[0], &size);
            ns_sockclose(sock);
            if (socks[0] == NS_INVALID_SOCKET) {
                ns_sockclose(socks[1]);
                result = -1;

            } else if ((!(SockAddrEqual((struct sockaddr *)&ia[0],
                                        (struct sockaddr *)&ia[1]))) ||
                       (Ns_SockaddrGetPort((struct sockaddr *)&ia[0]) != Ns_SockaddrGetPort((struct sockaddr *)&ia[1]))
                       ) {
                ns_sockclose(socks[0]);
                ns_sockclose(socks[1]);
                result = -1;
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListen --
 *
 *      Simple socket listen implementation for Win32 without
 *      privileged port issues.
 *
 * Results:
 *      Socket descriptor or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListenEx(const char *address, unsigned short port, int backlog, bool reuseport)
{
    NS_SOCKET sock;
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr *saPtr = (struct sockaddr *)&sa;

    if (Ns_GetSockAddr(saPtr, address, port) != NS_OK) {
        sock = NS_INVALID_SOCKET;
    } else {
        sock = Ns_SockBind(saPtr, reuseport);
        if (sock != NS_INVALID_SOCKET && listen(sock, backlog) != 0) {
            ns_sockclose(sock);
            sock = NS_INVALID_SOCKET;
        }
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * ConsoleHandler --
 *
 *      Callback when the Ctrl-C is pressed.
 *
 * Results:
 *      TRUE.
 *
 * Side effects:
 *      Shutdown is initiated.
 *
 *----------------------------------------------------------------------
 */

static BOOL WINAPI
ConsoleHandler(DWORD UNUSED(code))
{
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    NsSendSignal(NS_SIGTERM);

    return TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * GetServiceName --
 *
 *      Construct the service name for the corresponding service.
 *
 * Results:
 *      Pointer to given dstring string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
GetServiceName(Tcl_DString *dsPtr, char *service)
{
    Ns_DStringVarAppend(dsPtr, PACKAGE_NAME, "-", service, NS_SENTINEL);
    return dsPtr->string;
}


/*
 *----------------------------------------------------------------------
 *
 * StartTicker, StopTicker --
 *
 *      Start and stop the background ticker thread which keeps the
 *      service control manager informed of during startup and
 *      shutdown.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread is created or signaled to stop and joined.
 *
 *----------------------------------------------------------------------
 */

static void
StartTicker(DWORD pending)
{
    Ns_MutexLock(&lock);
    tick = 1;
    Ns_MutexUnlock(&lock);
    Ns_ThreadCreate(ServiceTicker, UINT2PTR(pending), 0, &tickThread);
}

static void
StopTicker(void)
{
    Ns_Log(Notice, "StopTicker -ticker-");
    Ns_MutexLock(&lock);
    tick = 0;
    Ns_CondBroadcast(&cond);
    Ns_MutexUnlock(&lock);
    Ns_ThreadJoin(&tickThread, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * ServiceTicker --
 *
 *      Background ticker created by StartTicker which does nothing
 *      but send the message repeatedly until signaled to stop.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Service control manager is kept informed of progress.
 *
 *----------------------------------------------------------------------
 */

static void
ServiceTicker(void *arg)
{
    Ns_Time timeout;
    DWORD pending = (DWORD) PTR2UINT(arg);

    Ns_ThreadSetName("-ticker-");

    Ns_Log(Notice, "starting, SERVICE_START_PENDING %d", pending);

    Ns_MutexLock(&lock);
    do {
        ReportStatus(pending, NO_ERROR, 2000u);
        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, 1, 0);
        (void) Ns_CondTimedWait(&cond, &lock, &timeout);
    } while (tick != 0);
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * ServiceMain --
 *
 *      Startup routine created by the service control manager.
 *      This routine initializes the structure for reporting status,
 *      starts the ticker, and then re-enters Ns_Main() where it
 *      was left off when NsServiceConnect() was called.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Server startup continues.
 *
 *----------------------------------------------------------------------
 */

static VOID WINAPI
ServiceMain(DWORD argc, LPTSTR *argv)
{
    hStatus = RegisterServiceCtrlHandler(argv[0], ServiceHandler);
    if (hStatus == NULL) {
        Ns_Fatal("nswin32: RegisterServiceCtrlHandler() failed: '%s'",
                 SysErrMsg());
    }
    curStatus.dwServiceType = (DWORD)SERVICE_WIN32_OWN_PROCESS;
    curStatus.dwServiceSpecificExitCode = 0u;

    StartTicker((DWORD)SERVICE_START_PENDING);
    (void) Ns_Main((int)argc, argv, NULL);
    StopTicker();
    ReportStatus((DWORD)SERVICE_STOP_PENDING, NO_ERROR, 100u);
    if (!serviceFailed) {
        Ns_Log(Notice, "nswin32: notifying SCM about exit");
        ReportStatus((DWORD)SERVICE_STOPPED, 0u, 0u);
    }
    Ns_Log(Notice, "nswin32: service exiting");
    if (serviceFailed) {
        exit(-1);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ServiceHandler --
 *
 *      Callback when the service control manager wants to signal the
 *      server (i.e., when service is stopped via services control
 *      panel).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Signal may be sent.
 *
 *----------------------------------------------------------------------
 */

static VOID WINAPI
ServiceHandler(DWORD code)
{
    if (code == (DWORD)SERVICE_CONTROL_STOP || code == (DWORD)SERVICE_CONTROL_SHUTDOWN) {
        NsSendSignal(NS_SIGTERM);
    } else {
        ReportStatus(code, NO_ERROR, 0u);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ReportStatus --
 *
 *      Update the service control manager with the current state.
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
ReportStatus(DWORD state, DWORD code, DWORD hint)
{
    if (state == (DWORD)SERVICE_START_PENDING) {
        curStatus.dwControlsAccepted = 0u;
    } else {
        curStatus.dwControlsAccepted =
            (DWORD)SERVICE_ACCEPT_STOP | (DWORD)SERVICE_ACCEPT_SHUTDOWN;
    }
    curStatus.dwCurrentState = state;
    curStatus.dwWin32ExitCode = code;
    if (code == (DWORD)ERROR_SERVICE_SPECIFIC_ERROR) {
        curStatus.dwServiceSpecificExitCode = code;
    }
    curStatus.dwWaitHint = hint;
    if (state == (DWORD)SERVICE_RUNNING || state == (DWORD)SERVICE_STOPPED) {
        curStatus.dwCheckPoint = 0u;
    } else {
        static DWORD check = 1u;
        curStatus.dwCheckPoint = check++;
    }
    if (hStatus != NULL && SetServiceStatus(hStatus, &curStatus) != TRUE) {
        Ns_Fatal("nswin32: SetServiceStatus(%ld) failed: '%s'", state,
                 SysErrMsg());
    }
}
/*
 * -----------------------------------------------------------------
 *  Copyright 1994 University of Washington
 *
 *  Permission is hereby granted to copy this software, and to
 *  use and redistribute it, except that this notice may not be
 *  removed.  The University of Washington does not guarantee
 *  that this software is suitable for any purpose and will not
 *  be held liable for any damage it may cause.
 * -----------------------------------------------------------------
 *
 *  Modified to work properly on Darwin 10.2 or less.
 *  Also, heavily reformatted to be more readable.
 */

int
ns_poll(struct pollfd *fds, NS_POLL_NFDS_TYPE nfds, long timo)
{
    struct timeval        timeout;
    const struct timeval *toPtr;
    fd_set                ifds, ofds, efds;
    unsigned long int     i;
    NS_SOCKET             n = NS_INVALID_SOCKET;
    int                   rc;

    FD_ZERO(&ifds);
    FD_ZERO(&ofds);
    FD_ZERO(&efds);

    for (i = 0u; i < nfds; ++i) {
        if (fds[i].fd == NS_INVALID_SOCKET) {
            continue;
        }
#ifndef _MSC_VER
        /* winsock ignores the first argument of select() */
        if (fds[i].fd > n) {
            n = fds[i].fd;
        }
#endif
        if ((fds[i].events & POLLIN) == POLLIN) {
            FD_SET(fds[i].fd, &ifds);
        }
        if ((fds[i].events & POLLOUT) == POLLOUT) {
            FD_SET(fds[i].fd, &ofds);
        }
        if ((fds[i].events & POLLPRI) == POLLPRI) {
            FD_SET(fds[i].fd, &efds);
        }
    }
    if (timo < 0) {
        toPtr = NULL;
    } else {
        toPtr = &timeout;
        timeout.tv_sec = timo / 1000;
        timeout.tv_usec = (timo - timeout.tv_sec * 1000) * 1000;
    }
    rc = select((int)++n, &ifds, &ofds, &efds, toPtr);
    if (rc > 0) {
        for (i = 0u; i < nfds; ++i) {
            fds[i].revents = 0;
            if (fds[i].fd == NS_INVALID_SOCKET) {
                continue;
            }
            if (FD_ISSET(fds[i].fd, &ifds)) {
                fds[i].revents |= (short)POLLIN;
            }
            if (FD_ISSET(fds[i].fd, &ofds)) {
                fds[i].revents |= POLLOUT;
            }
            if (FD_ISSET(fds[i].fd, &efds)) {
                fds[i].revents |= POLLPRI;
            }
        }
    }

    return (rc != SOCKET_ERROR) ? rc : -1;
}

/*
 *----------------------------------------------------------------------
 *
 * ns_open, ns_close, ns_write, ns_read, ns_lseek  --
 *
 *      Elementary operations on file descriptors. The interfaces are the same
 *      as in a Unix environment.
 *
 *      These functions are implemented in C due to slightly different
 *      interfaces under windows, but most prominently, to link the _* system
 *      calls to the main .dll file, such that external modules (such as
 *      e.g. nslog) do link against these instead of their own version.
 *
 * Results:
 *      For details, see MSDN
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
ns_open(const char *path, int oflag, int mode)
{
    return _open(path, oflag, mode);
}

int
ns_close(int fildes)
{
    return _close(fildes);
}

ssize_t
ns_write(int fildes, const void *buf, size_t nbyte)
{
    return _write(fildes, buf, (unsigned int)nbyte);
}

ssize_t
ns_read(int fildes, void *buf, size_t nbyte)
{
    return _read(fildes, buf, (unsigned int)nbyte);
}

off_t
ns_lseek(int fildes, off_t offset, int whence)
{
    return (off_t)_lseek(fildes, (long)offset, whence);
}

/*
 *----------------------------------------------------------------------
 *
 * ns_recv, ns_send  --
 *
 *      Elementary operations on sockets. The interfaces are the same
 *      as in a Unix environment.
 *
 *      These functions are implemented in C due to slightly different
 *      interfaces under windows.
 *
 * Results:
 *      For details, see MSDN
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
ssize_t
ns_recv(NS_SOCKET socket, void *buffer, size_t length, int flags)
{
    return recv(socket, buffer, (int)length, flags);
}

ssize_t
ns_send(NS_SOCKET socket, const void *buffer, size_t length, int flags)
{
    return send(socket, buffer, (int)length, flags);
}


// MSVC specific implementation
static void fseterr(FILE *fp)
{
    struct file { // Undocumented implementation detail
        unsigned char *_ptr;
        unsigned char *_base;
        int _cnt;
        int _flag;
        int _file;
        int _charbuf;
        int _bufsiz;
    };
    #define _IOERR 0x10

    ((struct file *)fp)->_flag |= _IOERR;
}

/*
 *----------------------------------------------------------------------
 *
 * ns_getline --
 *
 *      Basic implementation of the POSIX function getline for windows.
 *
 * Results:
 *      For details, see getline man pages.
 *
 * Side effects:
 *      Potentially allocating / reallocating memory.
 *
 *----------------------------------------------------------------------
 */
ssize_t
ns_getline(char **lineptr, size_t *n, FILE *stream)
{
    ssize_t nread = 0;
    int c = EOF;

    /*
     * Check input parameters
     */
    if (lineptr == NULL || n == NULL || stream == NULL || (*lineptr == NULL && *n != 0)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Return -1, when we are at EOF or in an error state of the stream.
     */
    if (feof(stream) || ferror(stream)) {
        return -1;
    }

    /*
     * If there is no buffer provided, allocate one via malloc()
     */
    if (*lineptr == NULL) {
        *n = 256;
        *lineptr = malloc(*n);
        if (*lineptr == NULL) {
            fseterr(stream);
            errno = ENOMEM;
            return -1;
        }
    }
    /*
     * Read char by char until we reach a newline. We could do
     * performance-wise better than this, but this is straightforward for EOF
     * handling, and eay to understand.
     */
    while (c != '\n') {
        c = fgetc(stream);
        if (c == EOF) {
            break;
        }

        /*
         * In case, the buffer was filled up, double it via realloc().
         */
        if (nread >= (ssize_t)(*n - 1)) {
            size_t newn = *n * 2;
            char  *newptr = realloc(*lineptr, newn);

            if (newptr == NULL) {
                /*
                 * When realloc() failed, give up.
                 */
                fseterr(stream);
                errno = ENOMEM;
                return -1;
            }
            *lineptr = newptr;
            *n = newn;
        }
        (*lineptr)[nread++] = (char)c;
    }
    /*
     * When we reach EOF or we could not read anything, return -1.
     */
    if (c == EOF && nread == 0) {
        return -1;
    }
    /*
     * Terminate the returned string with a NUL character.
     */
    (*lineptr)[nread] = 0;

    return nread;
}

#else
/*
 * Avoid empty translation unit
 */
   typedef void empty;
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
