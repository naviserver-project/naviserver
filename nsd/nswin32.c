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

#ifdef _WIN32

/*
 * win32.c --
 *
 *  Win32 specific routines.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");

static Ns_ThreadProc ServiceTicker;
static void StopTicker(void);
static void StartTicker(DWORD pending);
static VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
static VOID WINAPI ServiceHandler(DWORD code);
static BOOL WINAPI ConsoleHandler(DWORD code);
static void ReportStatus(DWORD state, DWORD code, DWORD hint);
static void ExitService(void);
static char *GetServiceName(Ns_DString *dsPtr, char *server);

/*
 * Static variables used in this file
 */

static Ns_Mutex lock;
static Ns_Cond cond;
static Ns_Thread tickThread;
static SERVICE_STATUS_HANDLE hStatus = 0;
static SERVICE_STATUS curStatus;
static Ns_Tls tls;
static int service = 0;
static int tick;
static int sigpending = 0;
static int servicefailed = 0;

#define SysErrMsg() (NsWin32ErrMsg(GetLastError()))

void NsdInit(void);


/*
 *----------------------------------------------------------------------
 *
 * NsBlockSignal --
 *
 *      Mask one specific signale
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
NsBlockSignal(int signal)
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
NsUnblockSignal(int signal)
{
    return;
}

int
Ns_SetGroup(char *group)
{
    return -1;
}

int
Ns_SetUser(char *user)
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
DllMain(HANDLE hModule, DWORD why, LPVOID lpReserved)
{
    WSADATA wsd;

    if (why == DLL_PROCESS_ATTACH) {
        Ns_TlsAlloc(&tls, ns_free);
        if (WSAStartup(MAKEWORD(1, 1), &wsd) != 0) {
            return FALSE;
        }
        DisableThreadLibraryCalls(hModule);
        Nsd_LibInit();
    } else if (why == DLL_PROCESS_DETACH) {
        WSACleanup();
    }

    return TRUE;
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
NsWin32ErrMsg(int err)
{
    char *msg;

    msg = Ns_TlsGet(&tls);
    if (msg == NULL) {
        msg = ns_malloc(100);
        Ns_TlsSet(&tls, msg);
    }
    snprintf(msg, 100, "win32 error code: %d", err);

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
 *      None.
 *
 * Side effects:
 *      Service control manager will create a new thread running
 *      ServiceMain().
 *
 *----------------------------------------------------------------------
 */

int
NsConnectService(void)
{
    SERVICE_TABLE_ENTRY table[2];
    char  *nul = "nul:", buf[PATH_MAX];
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

    freopen(nul, "rt", stdin);
    freopen(nul, "wt", stdout);
    freopen(nul, "wt", stderr);

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

    service = 1;

    table[0].lpServiceName = NSD_NAME;
    table[0].lpServiceProc = ServiceMain;
    table[1].lpServiceName = NULL;
    table[1].lpServiceProc = NULL;

    ok = StartServiceCtrlDispatcher(table);

    if (!ok) {
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
 *      None.
 *
 * Side effects:
 *      Service should stop and then disappear from the list in the
 *      services control panel.
 *
 *----------------------------------------------------------------------
 */

int
NsRemoveService(char *server)
{
    SC_HANDLE hmgr, hsrv;
    SERVICE_STATUS status;
    Ns_DString name;
    BOOL ok;

    Ns_DStringInit(&name);
    GetServiceName(&name, server);
    ok = FALSE;
    hmgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (hmgr != NULL) {
        hsrv = OpenService(hmgr, name.string, SERVICE_ALL_ACCESS);
        if (hsrv != NULL) {
            ControlService(hsrv, SERVICE_CONTROL_STOP, &status);
            ok = DeleteService(hsrv);
            CloseServiceHandle(hsrv);
        }
        CloseServiceHandle(hmgr);
    }
    if (ok) {
        Ns_Log(Notice, "nswin32: removed service: %s", name.string);
    } else {
        Ns_Log(Error, "nswin32: failed to remove %s service: %s",
               name.string, SysErrMsg());
    }
    Ns_DStringFree(&name);

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
 *      None.
 *
 * Side effects:
 *      Service should appear in the list in the services control panel.
 *
 *----------------------------------------------------------------------
 */

int
NsInstallService(char *server)
{
    SC_HANDLE hmgr, hsrv;
    BOOL ok;
    char nsd[PATH_MAX], config[PATH_MAX];
    Ns_DString name, cmd;

    ok = FALSE;
    if (_fullpath(config, nsconf.config, sizeof(config)) == NULL) {
        Ns_Log(Error, "nswin32: invalid config path '%s'", nsconf.config);
    } else if (!GetModuleFileName(NULL, nsd, sizeof(nsd))) {
        Ns_Log(Error, "nswin32: failed to find nsd.exe: '%s'", SysErrMsg());
    } else {
        Ns_DStringInit(&name);
        Ns_DStringInit(&cmd);
        Ns_DStringVarAppend(&cmd, "\"", nsd, "\"",
                            " -S -s ", server, " -t \"", config, "\"", NULL);
        GetServiceName(&name, server);
        Ns_Log(Notice, "nswin32: installing %s service: %s",
               name.string, cmd.string);
        hmgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (hmgr != NULL) {
            hsrv = CreateService(hmgr, name.string, name.string,
                                 SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 cmd.string, NULL, NULL, "TcpIp\0", NULL, NULL);
            if (hsrv != NULL) {
                CloseServiceHandle(hsrv);
                ok = TRUE;
            }
            CloseServiceHandle(hmgr);
        }
        if (!ok) {
            Ns_Log(Error, "nswin32: failed to install service '%s': '%s'",
                   name.string, SysErrMsg());
        }
        Ns_DStringFree(&name);
        Ns_DStringFree(&cmd);
    }

    return (ok ? NS_OK : NS_ERROR);
}


/*
 *----------------------------------------------------------------------
 * NsRestoreSignals --
 *
 *      Noop to avoid ifdefs and make symetrical to Unix part
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
    int sig;

    /*
     * If running as a service, stop the ticker thread and report
     * startup complete. Otherwise, register a handler which will
     * initiate an orderly shutdown on Ctrl-C.
     */

    if (!service) {
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    } else {
        StopTicker();
        ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    }
    Ns_MutexSetName2(&lock, "ns", "signal");
    do {
        Ns_MutexLock(&lock);
        while (sigpending == 0) {
            Ns_CondWait(&cond, &lock);
        }
        sig = sigpending;
        sigpending = 0;
        if ((sig & NS_SIGINT)) {

           /*
            * Signalize the Service Control Manager
            * to restart the service.
            */

            servicefailed = 1;
        }
        Ns_MutexUnlock(&lock);
        if ((sig & NS_SIGHUP)) {
            NsRunSignalProcs();
        }
    } while (sig == NS_SIGHUP);

    /*
     * If running as a service, startup the ticker thread again
     * to keep updating status until shutdown is complete.
     */

    if (service) {
        StartTicker(SERVICE_STOP_PENDING);
    }

    return sig;
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
        sigpending |= sig;
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
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsMemMap(CONST char *path, int size, int mode, FileMap *mapPtr)
{
    HANDLE hndl, mobj;
    LPCVOID addr;
    char name[256];

    switch (mode) {
    case NS_MMAP_WRITE:
        hndl = CreateFile(path,
                          GENERIC_READ|GENERIC_WRITE,
                          FILE_SHARE_READ|FILE_SHARE_WRITE,
                          NULL,
                          OPEN_EXISTING,
                          FILE_FLAG_WRITE_THROUGH,
                          NULL);
        break;
    case NS_MMAP_READ:
        hndl = CreateFile(path,
                          GENERIC_READ,
                          FILE_SHARE_READ,
                          NULL,
                          OPEN_EXISTING,
                          0,
                          NULL);
        break;
    default:
        return NS_ERROR;
    }

    if (hndl == NULL || hndl == INVALID_HANDLE_VALUE) {
        Ns_Log(Error, "CreateFile(%s): %d", path, GetLastError());
        return NS_ERROR;
    }

    snprintf(name, sizeof(name), "MapObj-%s", Ns_ThreadGetName());

    mobj = CreateFileMapping(hndl,
                             NULL,
                             mode == NS_MMAP_WRITE ?
                             PAGE_READWRITE : PAGE_READONLY,
                             0,
                             0,
                             name);

    if (mobj == NULL || mobj == INVALID_HANDLE_VALUE) {
        Ns_Log(Error, "CreateFileMapping(%s): %d", path, GetLastError());
        CloseHandle(hndl);
        return NS_ERROR;
    }

    addr = MapViewOfFile(mobj,
                         mode == NS_MMAP_WRITE ?
                         FILE_MAP_WRITE : FILE_MAP_READ,
                         0,
                         0,
                         size);

    if (addr == NULL) {
        Ns_Log(Warning, "MapViewOfFile(%s): %d", path, GetLastError());
        CloseHandle(mobj);
        CloseHandle(hndl);
        return NS_ERROR;
    }

    mapPtr->mapobj = (void *) mobj;
    mapPtr->handle = (int) hndl;
    mapPtr->addr   = (void *) addr;
    mapPtr->size   = size;

    return NS_OK;
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
NsMemUmap(FileMap *mapPtr)
{
    UnmapViewOfFile((LPCVOID)mapPtr->addr);
    CloseHandle((HANDLE)mapPtr->mapobj);
    CloseHandle((HANDLE)mapPtr->handle);
}


/*
 *----------------------------------------------------------------------
 *
 * ns_socknbclose --
 *
 *      Perform a non-blocking socket close via the socket callback
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
ns_socknbclose(SOCKET sock)
{
    if (Ns_SockCloseLater(sock) != NS_OK) {
        return SOCKET_ERROR;
    }

    return 0;
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

SOCKET
ns_sockdup(SOCKET sock)
{
    HANDLE hp, src, dup;

    src = (HANDLE) sock;
    hp = GetCurrentProcess();
    if (!DuplicateHandle(hp, src, hp, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return INVALID_SOCKET;
    }

    return (SOCKET) dup;
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
    return _pipe(fds, 4096, _O_NOINHERIT|_O_BINARY);
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
ns_sockpair(SOCKET socks[2])
{
    SOCKET sock;
    struct sockaddr_in ia[2];
    int size;

    size = sizeof(struct sockaddr_in);
    sock = Ns_SockListen("127.0.0.1", 0);
    if (sock == INVALID_SOCKET ||
        getsockname(sock, (struct sockaddr *) &ia[0], &size) != 0) {
        return -1;
    }
    size = sizeof(struct sockaddr_in);
    socks[1] = Ns_SockConnect("127.0.0.1", (int) ntohs(ia[0].sin_port));
    if (socks[1] == INVALID_SOCKET ||
        getsockname(socks[1], (struct sockaddr *) &ia[1], &size) != 0) {
        ns_sockclose(sock);
        return -1;
    }
    size = sizeof(struct sockaddr_in);
    socks[0] = accept(sock, (struct sockaddr *) &ia[0], &size);
    ns_sockclose(sock);
    if (socks[0] == INVALID_SOCKET) {
        ns_sockclose(socks[1]);
        return -1;
    }
    if (ia[0].sin_addr.s_addr != ia[1].sin_addr.s_addr ||
        ia[0].sin_port != ia[1].sin_port) {
        ns_sockclose(socks[0]);
        ns_sockclose(socks[1]);
        return -1;
    }

    return 0;
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
 *      Socket descriptor or INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

SOCKET
Ns_SockListenEx(char *address, int port, int backlog)
{
    SOCKET sock;
    struct sockaddr_in sa;

    if (Ns_GetSockAddr(&sa, address, port) != NS_OK) {
        return -1;
    }
    sock = Ns_SockBind(&sa);
    if (sock != INVALID_SOCKET && listen(sock, backlog) != 0) {
        ns_sockclose(sock);
        sock = INVALID_SOCKET;
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
ConsoleHandler(DWORD ignored)
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
 *      Construct the service name for the corresponding server.
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
GetServiceName(Ns_DString *dsPtr, char *server)
{
    Ns_DStringVarAppend(dsPtr, NSD_NAME, "-", server, NULL);
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
 *      Thread is created or signalled to stop and joined.
 *
 *----------------------------------------------------------------------
 */

static void
StartTicker(DWORD pending)
{
    Ns_MutexLock(&lock);
    tick = 1;
    Ns_MutexUnlock(&lock);
    Ns_ThreadCreate(ServiceTicker, (void *) pending, 0, &tickThread);
}

static void
StopTicker(void)
{
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
    DWORD pending = (DWORD) arg;

    Ns_ThreadSetName("-ticker-");

    Ns_MutexLock(&lock);
    do {
        ReportStatus(pending, NO_ERROR, 2000);
        Ns_GetTime(&timeout);
        Ns_IncrTime(&timeout, 1, 0);
        Ns_CondTimedWait(&cond, &lock, &timeout);
    } while (tick);
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
    if (hStatus == 0) {
        Ns_Fatal("nswin32: RegisterServiceCtrlHandler() failed: '%s'",
                 SysErrMsg());
    }
    curStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    curStatus.dwServiceSpecificExitCode = 0;
    StartTicker(SERVICE_START_PENDING);
    Ns_Main(argc, argv, NULL);
    StopTicker();
    ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 100);
    if (!servicefailed) {
        Ns_Log(Notice, "nswin32: noitifying SCM about exit");
        ReportStatus(SERVICE_STOPPED, 0, 0);
    }
    Ns_Log(Notice, "nswin32: service exiting");
    if(servicefailed) {
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
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        NsSendSignal(NS_SIGTERM);
    } else {
        ReportStatus(code, NO_ERROR, 0);
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
    static DWORD check = 1;

    if (state == SERVICE_START_PENDING) {
        curStatus.dwControlsAccepted = 0;
    } else {
        curStatus.dwControlsAccepted =
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }
    curStatus.dwCurrentState = state;
    curStatus.dwWin32ExitCode = code;
    if (code == ERROR_SERVICE_SPECIFIC_ERROR) {
        curStatus.dwServiceSpecificExitCode = code;
    }
    curStatus.dwWaitHint = hint;
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        curStatus.dwCheckPoint = 0;
    } else {
        curStatus.dwCheckPoint = check++;
    }
    if (hStatus != 0 && SetServiceStatus(hStatus, &curStatus) != TRUE) {
        Ns_Fatal("nswin32: SetServiceStatus(%d) failed: '%s'", state,
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
ns_poll(struct pollfd *fds, unsigned long int nfds, int timo)
{
    struct timeval timeout, *toptr;
    fd_set ifds, ofds, efds;
    int i, rc, n = -1;

    FD_ZERO(&ifds);
    FD_ZERO(&ofds);
    FD_ZERO(&efds);

    for (i = 0; i < nfds; ++i) {
        if (fds[i].fd == -1) {
            continue;
        }
        if (fds[i].fd > n) {
            n = fds[i].fd;
        }
        if ((fds[i].events & POLLIN)) {
            FD_SET(fds[i].fd, &ifds);
        }
        if ((fds[i].events & POLLOUT)) {
            FD_SET(fds[i].fd, &ofds);
        }
        if ((fds[i].events & POLLPRI)) {
            FD_SET(fds[i].fd, &efds);
        }
    }
    if (timo < 0) {
        toptr = NULL;
    } else {
        toptr = &timeout;
        timeout.tv_sec = timo / 1000;
        timeout.tv_usec = (timo - timeout.tv_sec * 1000) * 1000;
    }
    rc = select(++n, &ifds, &ofds, &efds, toptr);
    if (rc <= 0) {
        return rc;
    }
    for (i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd == -1) {
            continue;
        }
        if (FD_ISSET(fds[i].fd, &ifds)) {
            fds[i].revents |= POLLIN;
        }
        if (FD_ISSET(fds[i].fd, &ofds)) {
            fds[i].revents |= POLLOUT;
        }
        if (FD_ISSET(fds[i].fd, &efds)) {
            fds[i].revents |= POLLPRI;
        }
    }

    return rc;
}

#endif

