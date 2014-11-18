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

static Ns_ThreadProc ServiceTicker;
static void StopTicker(void);
static void StartTicker(DWORD pending);
static VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
static VOID WINAPI ServiceHandler(DWORD code);
static BOOL WINAPI ConsoleHandler(DWORD code);
static void ReportStatus(DWORD state, DWORD code, DWORD hint);
static char *GetServiceName(Ns_DString *dsPtr, char *service);

/*
 * Static variables used in this file
 */

static Ns_Mutex lock;
static Ns_Cond cond;
static Ns_Thread tickThread;
static SERVICE_STATUS_HANDLE hStatus = 0;
static SERVICE_STATUS curStatus;
static Ns_Tls tls;
static int serviceRunning = 0;
static int tick = 0;
static unsigned int sigpending = 0U;
static int serviceFailed = 0;

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
Ns_SetGroup(char *UNUSED(group))
{
    return -1;
}

int
Ns_SetUser(char *UNUSED(user))
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
    WSADATA wsd;

    if (why == (DWORD)DLL_PROCESS_ATTACH) {
        Ns_TlsAlloc(&tls, ns_free);
        if (WSAStartup((WORD)MAKEWORD(1, 1), &wsd) != 0) {
            return FALSE;
        }
        DisableThreadLibraryCalls(hModule);
        Nsd_LibInit();
    } else if (why == (DWORD)DLL_PROCESS_DETACH) {
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
NsWin32ErrMsg(DWORD err)
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

    freopen(DEVNULL, "rt", stdin);
    freopen(DEVNULL, "wt", stdout);
    freopen(DEVNULL, "wt", stderr);

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

    serviceRunning = 1;

    table[0].lpServiceName = PACKAGE_NAME;
    table[0].lpServiceProc = ServiceMain;
    table[1].lpServiceName = NULL;
    table[1].lpServiceProc = NULL;

    ok = StartServiceCtrlDispatcher(table);

    if (ok == 0) {
        Ns_Log(Error, "nswin32: StartServiceCtrlDispatcher(): '%s'",
               SysErrMsg());
    }

    return ((ok != 0) ? NS_OK : NS_ERROR);
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
NsRemoveService(char *service)
{
    SC_HANDLE hmgr;
    SERVICE_STATUS status;
    Ns_DString name;
    BOOL ok;

    Ns_DStringInit(&name);
    GetServiceName(&name, service);
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
    Ns_DStringFree(&name);

    return ((ok != 0) ? NS_OK : NS_ERROR);
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
NsInstallService(char *service)
{
    SC_HANDLE hmgr, hsrv;
    bool ok = FALSE;
    char nsd[PATH_MAX], config[PATH_MAX];
    Ns_DString name, cmd;

    if (_fullpath(config, nsconf.config, sizeof(config)) == NULL) {
        Ns_Log(Error, "nswin32: invalid config path '%s'", nsconf.config);
    } else if (GetModuleFileName(NULL, nsd, sizeof(nsd)) == 0u) {
        Ns_Log(Error, "nswin32: failed to find nsd.exe: '%s'", SysErrMsg());
    } else {
        Ns_DStringInit(&name);
        Ns_DStringInit(&cmd);
        Ns_DStringVarAppend(&cmd, "\"", nsd, "\"",
                            " -S -s ", service, " -t \"", config, "\"", NULL);
        GetServiceName(&name, service);
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
        Ns_DStringFree(&name);
        Ns_DStringFree(&cmd);
    }

    return ((ok != 0) ? NS_OK : NS_ERROR);
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
    unsigned int sig;

    /*
     * If running as a service, stop the ticker thread and report
     * startup complete. Otherwise, register a handler which will
     * initiate an orderly shutdown on Ctrl-C.
     */

    if (serviceRunning == 0) {
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    } else {
        StopTicker();
        ReportStatus((DWORD)SERVICE_RUNNING, NO_ERROR, 0u);
    }
    Ns_MutexSetName2(&lock, "ns", "signal");
    do {
        Ns_MutexLock(&lock);
        while (sigpending == 0U) {
            Ns_CondWait(&cond, &lock);
        }
        sig = sigpending;
        sigpending = 0U;
        if ((sig & (1u << NS_SIGINT)) != 0U) {

           /*
            * Signalize the Service Control Manager
            * to restart the service.
            */

            serviceFailed = 1;
        }
        Ns_MutexUnlock(&lock);
        if ((sig & (1u << NS_SIGHUP)) != 0U) {
	    NsRunSignalProcs();
        }
    } while ((sig & (1u << NS_SIGHUP)) != 0U);

    /*
     * If running as a service, startup the ticker thread again
     * to keep updating status until shutdown is complete.
     */

    if (serviceRunning != 0) {
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
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsMemMap(const char *path, size_t size, int mode, FileMap *mapPtr)
{
    HANDLE hndl, mobj;
    LPCVOID addr;
    char name[256];

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
                             mode == NS_MMAP_WRITE ? (DWORD)PAGE_READWRITE : (DWORD)PAGE_READONLY,
                             0u,
                             0u,
                             name);

    if (mobj == NULL || mobj == INVALID_HANDLE_VALUE) {
        Ns_Log(Error, "CreateFileMapping(%s): %ld", path, GetLastError());
        CloseHandle(hndl);
        return NS_ERROR;
    }

    addr = MapViewOfFile(mobj,
                         mode == NS_MMAP_WRITE ? (DWORD)FILE_MAP_WRITE : (DWORD)FILE_MAP_READ,
                         0u,
                         0u,
                         size);

    if (addr == NULL) {
        Ns_Log(Warning, "MapViewOfFile(%s): %ld", path, GetLastError());
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
NsMemUmap(const FileMap *mapPtr)
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
ns_socknbclose(NS_SOCKET sock)
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

NS_SOCKET
ns_sockdup(NS_SOCKET sock)
{
    HANDLE hp, src, dup;

    src = (HANDLE) sock;
    hp = GetCurrentProcess();
    if (DuplicateHandle(hp, src, hp, &dup, 0u, FALSE, (DWORD)DUPLICATE_SAME_ACCESS) == 0) {
        return NS_INVALID_SOCKET;
    }

    return (NS_SOCKET) dup;
}


/*
 *----------------------------------------------------------------------
 * ns_sock_set_blocking --
 *
 *      Set a channel blocking or non-blocking
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
ns_sock_set_blocking(NS_SOCKET fd, int blocking) 
{
    long state = (long)(blocking == 0);

    return ioctlsocket(fd, (long)FIONBIO, &state);
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
 *      Create a temporary file based on the provided template and
 *      return its fd.  This is a cheap replacement for mkstemp()
 *      under unix-like systems.
 *
 * Results:
 *      fd if ok, -1 on error.
 *
 * Side effects:
 *      Opens a temporary file.
 *
 *----------------------------------------------------------------------
 */
#include <share.h>

int
ns_mkstemp(char *template) 
{
    int err, fd = -1;

    err = _mktemp_s(template, strlen(template));

    if (err == 0) {
	err = _sopen_s(&fd, template, 
		       O_RDWR | O_CREAT |_O_TEMPORARY | O_EXCL, 
		       _SH_DENYRW,
		       _S_IREAD | _S_IWRITE);
    }

    if (err != 0) {
	return -1;
    }

    return fd;
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
    struct sockaddr_in ia[2];
    int size;

    size = (int)sizeof(struct sockaddr_in);
    sock = Ns_SockListen("127.0.0.1", 0);
    if (sock == NS_INVALID_SOCKET ||
        getsockname(sock, (struct sockaddr *) &ia[0], &size) != 0) {
        return -1;
    }
    size = (int)sizeof(struct sockaddr_in);
    socks[1] = Ns_SockConnect("127.0.0.1", (int) ntohs(ia[0].sin_port));
    if (socks[1] == NS_INVALID_SOCKET ||
        getsockname(socks[1], (struct sockaddr *) &ia[1], &size) != 0) {
        ns_sockclose(sock);
        return -1;
    }
    size = (int)sizeof(struct sockaddr_in);
    socks[0] = accept(sock, (struct sockaddr *) &ia[0], &size);
    ns_sockclose(sock);
    if (socks[0] == NS_INVALID_SOCKET) {
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
 *      Socket descriptor or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListenEx(const char *address, int port, int backlog)
{
    NS_SOCKET sock;
    struct sockaddr_in sa;

    if (Ns_GetSockAddr(&sa, address, port) != NS_OK) {
        return NS_INVALID_SOCKET;
    }
    sock = Ns_SockBind(&sa);
    if (sock != NS_INVALID_SOCKET && listen(sock, backlog) != 0) {
        ns_sockclose(sock);
        sock = NS_INVALID_SOCKET;
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
GetServiceName(Ns_DString *dsPtr, char *service)
{
    Ns_DStringVarAppend(dsPtr, PACKAGE_NAME, "-", service, NULL);
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
        ReportStatus(pending, NO_ERROR, 2000u);
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
    curStatus.dwServiceType = (DWORD)SERVICE_WIN32_OWN_PROCESS;
    curStatus.dwServiceSpecificExitCode = 0u;
    StartTicker((DWORD)SERVICE_START_PENDING);
    Ns_Main((int)argc, argv, NULL);
    StopTicker();
    ReportStatus((DWORD)SERVICE_STOP_PENDING, NO_ERROR, 100u);
    if (serviceFailed == 0) {
        Ns_Log(Notice, "nswin32: noitifying SCM about exit");
        ReportStatus((DWORD)SERVICE_STOPPED, 0u, 0u);
    }
    Ns_Log(Notice, "nswin32: service exiting");
    if(serviceFailed) {
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
    if (hStatus != 0 && SetServiceStatus(hStatus, &curStatus) != TRUE) {
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
ns_poll(struct pollfd *fds, NS_POLL_NFDS_TYPE nfds, int timo)
{
    struct timeval timeout, *toPtr;
    fd_set ifds, ofds, efds;
    unsigned long int i;
    NS_SOCKET n = NS_INVALID_SOCKET;
    int rc;

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
        toPtr = NULL;
    } else {
        toPtr = &timeout;
        timeout.tv_sec = timo / 1000;
        timeout.tv_usec = (timo - timeout.tv_sec * 1000) * 1000;
    }
    rc = select((int)++n, &ifds, &ofds, &efds, toPtr);
    if (rc <= 0) {
        return rc;
    }
    for (i = 0u; i < nfds; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd == NS_INVALID_SOCKET) {
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

/*
 *----------------------------------------------------------------------
 *
 * ns_open, ns_close, ns_write, ns_read  --
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

int 
ns_dup(int fildes) 
{
    return _dup(fildes);
}

int     
ns_dup2(int fildes, int fildes2)
{
    return _dup2(fildes, fildes2);
}
#else
/* avoid empty translation unit */
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
