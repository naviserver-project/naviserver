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
 * nssock.c --
 *
 *      Call internal Ns_DriverInit.
 *
 */

#include "ns.h"

NS_EXTERN const int Ns_ModuleVersion;
NS_EXPORT const int Ns_ModuleVersion = 1;


typedef struct Config {
    int        deferaccept;  /* Enable the TCP_DEFER_ACCEPT optimization. */
    int        nodelay;      /* Enable the TCP_NODELAY optimization. */
} Config;

/*
 * Local functions defined in this file.
 */

static Ns_DriverListenProc SockListen;
static Ns_DriverAcceptProc SockAccept;
static Ns_DriverRecvProc SockRecv;
static Ns_DriverSendProc SockSend;
static Ns_DriverCloseProc SockClose;
static Ns_DriverSendFileProc SendFile;
static Ns_DriverKeepProc Keep;
static Ns_DriverConnInfoProc ConnInfo;

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;


/*
 *----------------------------------------------------------------------
 *
 * Ns_ModuleInit --
 *
 *      Sock module init routine.
 *
 * Results:
 *      See Ns_DriverInit.
 *
 * Side effects:
 *      See Ns_DriverInit.
 *
 *----------------------------------------------------------------------
 */

NS_EXPORT Ns_ReturnCode
Ns_ModuleInit(const char *server, const char *module)
{
    Ns_DriverInitData  init;
    Config            *drvCfgPtr;
    const char        *section;

    NS_NONNULL_ASSERT(module != NULL);

    memset(&init, 0, sizeof(init));
    section = Ns_ConfigSectionPath(NULL, server, module, NS_SENTINEL);
    drvCfgPtr = ns_malloc(sizeof(Config));
    drvCfgPtr->deferaccept = Ns_ConfigBool(section, "deferaccept", NS_FALSE);
    drvCfgPtr->nodelay = Ns_ConfigBool(section, "nodelay", NS_TRUE);

    init.version = NS_DRIVER_VERSION_5;
    init.name         = "nssock";
    init.listenProc   = SockListen;
    init.acceptProc   = SockAccept;
    init.recvProc     = SockRecv;
    init.sendProc     = SockSend;
    init.sendFileProc = SendFile;
    init.keepProc     = Keep;
    init.connInfoProc = ConnInfo;
    init.requestProc  = NULL;
    init.closeProc    = SockClose;
    init.opts         = NS_DRIVER_ASYNC;
    init.arg          = drvCfgPtr;
    init.path         = (char*)section;
    init.protocol     = "http";
    init.defaultPort  = 80;

    return Ns_DriverInit(server, module, &init);
}


/*
 *----------------------------------------------------------------------
 *
 * SockListen --
 *
 *      Open a listening TCP socket in nonblocking mode.
 *
 * Results:
 *      The open socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      Enable TCP_DEFER_ACCEPT if available.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
SockListen(Ns_Driver *driver, const char *address, unsigned short port, int backlog, bool reuseport)
{
    NS_SOCKET sock;
    bool      unixDomainSocket;

    unixDomainSocket = (*address == '/');
    if (unixDomainSocket) {
        sock = Ns_SockListenUnix(address, backlog, 0 /*mode*/);
    } else {
        sock = Ns_SockListenEx(address, port, backlog, reuseport);
    }

    if (sock != NS_INVALID_SOCKET) {
        const Config *drvCfgPtr = driver->arg;

        (void) Ns_SockSetNonBlocking(sock);
        if (drvCfgPtr->deferaccept != 0 && !unixDomainSocket) {
            Ns_SockSetDeferAccept(sock, (long)driver->recvwait.sec);
        }

        if (unixDomainSocket) {
            Ns_Log(Notice, "listening on unix:%s (sock %d)", address, (int)sock);
        } else {
            Ns_Log(Notice, "listening on [%s]:%d (sock %d)", address, port, (int)sock);
        }
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * SockAccept --
 *
 *      Accept a new TCP socket in nonblocking mode.
 *
 * Results:
 *      NS_DRIVER_ACCEPT       - socket accepted
 *      NS_DRIVER_ACCEPT_DATA  - socket accepted, data present
 *      NS_DRIVER_ACCEPT_ERROR - socket not accepted
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static NS_DRIVER_ACCEPT_STATUS
SockAccept(Ns_Sock *sock, NS_SOCKET listensock,
           struct sockaddr *sockaddrPtr, socklen_t *socklenPtr)
{
    const Config *drvCfgPtr = sock->driver->arg;
    NS_DRIVER_ACCEPT_STATUS status = NS_DRIVER_ACCEPT_ERROR;

    sock->sock = Ns_SockAccept(listensock, sockaddrPtr, socklenPtr);
    if (sock->sock != NS_INVALID_SOCKET) {

#ifdef __APPLE__
        /*
         * Darwin's poll returns per default writable in situations,
         * where nothing can be written.  Setting the socket option for
         * the send low watermark to 1 fixes this problem.
         */
        int value = 1;
        setsockopt(sock->sock, SOL_SOCKET, SO_SNDLOWAT, &value, sizeof(value));
#endif
        (void)Ns_SockSetNonBlocking(sock->sock);
        if (drvCfgPtr->nodelay != 0) {
            Ns_SockSetNodelay(sock->sock);
        }

        status = (drvCfgPtr->deferaccept != 0) ? NS_DRIVER_ACCEPT_DATA : NS_DRIVER_ACCEPT;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * SockRecv --
 *
 *      Receive data into given buffers.
 *
 * Results:
 *      Total number of bytes received or -1 on error, EOF or timeout.
 *      The member "recvSockState" of Ns_Sock with have the following
 *      potential SockState values:
 *          success:  NS_SOCK_READ
 *          eof:      NS_SOCK_DONE
 *          again:    NS_SOCK_AGAIN
 *          error:    NS_SOCK_EXCEPTION
 *          timeout:  NS_SOCK_TIMEOUT
 *          not used: NS_SOCK_NONE
 *
 * Side effects:
 *      May block once for driver recvwait timeout seconds if no data
 *      available on first attempt.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SockRecv(Ns_Sock *sock, struct iovec *bufs, int nbufs,
         Ns_Time *timeoutPtr, unsigned int flags)
{
    return Ns_SockRecvBufs(sock, bufs, nbufs, timeoutPtr, flags);
}


/*
 *----------------------------------------------------------------------
 *
 * SockSend --
 *
 *      Send data from given buffers.
 *
 * Results:
 *      Number of bytes sent, -1 on error.
 *      May return 0 (zero) if socket is not writable.
 *
 * Side effects:
 *      Timeout value and handling are ignored.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SockSend(Ns_Sock *sock, const struct iovec *bufs, int nbufs, unsigned int flags)
{
    ssize_t       sent;
    bool          decork;
    unsigned long errorCode;

    decork = Ns_SockCork(sock, NS_TRUE);

    sent = Ns_SockSendBufsEx(sock->sock, bufs, nbufs, flags, &errorCode);
    Ns_SockSetSendErrno(sock, errorCode);

    if (decork) {
        Ns_SockCork(sock, NS_FALSE);
    }

    return sent;
}


/*
 *----------------------------------------------------------------------
 *
 * SendFile --
 *
 *      Send given file buffers directly to socket.
 *
 * Results:
 *      Total number of bytes sent, -1 on error.
 *      May return 0 (zero) if socket is not writable.
 *
 *
 * Side effects:
 *      May block on disk IO.
 *      Timeout value and handling are ignored.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SendFile(Ns_Sock *sock, Ns_FileVec *bufs, int nbufs, unsigned int flags)
{
    return Ns_SockSendFileBufs(sock, bufs, nbufs, NS_DRIVER_CAN_USE_SENDFILE|flags);
}


/*
 *----------------------------------------------------------------------
 *
 * Keep --
 *
 *      We are always to try keepalive if the upper layers are.
 *
 * Results:
 *      NS_TRUE, always.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
Keep(Ns_Sock *UNUSED(sock))
{
    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * SockClose --
 *
 *      Close the connection socket.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Ignore any errors.
 *
 *----------------------------------------------------------------------
 */

static void
SockClose(Ns_Sock *sock)
{
    NS_NONNULL_ASSERT(sock != NULL);

    if (sock->sock != NS_INVALID_SOCKET) {
        ns_sockclose(sock->sock);
        sock->sock = NS_INVALID_SOCKET;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * ConnInfo --
 *
 *      Return Tcl_Obj hinting connection details
 *
 * Results:
 *      Tcl_Obj *
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj*
ConnInfo(Ns_Sock *sock)
{
    Tcl_Obj *resultObj;
    char     ipString[NS_IPADDR_SIZE];

    NS_NONNULL_ASSERT(sock != NULL);

    resultObj = Tcl_NewDictObj();
    (void)ns_inet_ntop(Ns_SockGetConfiguredSockAddr(sock), ipString, NS_IPADDR_SIZE);
    Tcl_DictObjPut(NULL, resultObj,
                   Tcl_NewStringObj("currentaddr", 11),
                   Tcl_NewStringObj(ipString, -1));

    (void)Ns_SockaddrAddToDictIpProperties((struct sockaddr *)&(sock->sa), resultObj);

    return resultObj;
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
