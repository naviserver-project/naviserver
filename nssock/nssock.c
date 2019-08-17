/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
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
 * nssock.c --
 *
 *      Call internal Ns_DriverInit.
 *
 */

#include "ns.h"

NS_EXPORT const int Ns_ModuleVersion = 1;


typedef struct Config {
    int        deferaccept;  /* Enable the TCP_DEFER_ACCEPT optimization. */
    int        nodelay;      /* Enable the TCP_NODEALY optimization. */
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

NS_EXPORT Ns_ModuleInitProc Ns_ModuleInit;

static void SetNodelay(Ns_Driver *driver, NS_SOCKET sock)
    NS_GNUC_NONNULL(1);


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
    Config            *cfg;
    const char        *path;

    NS_NONNULL_ASSERT(module != NULL);

    memset(&init, 0, sizeof(init));
    path = Ns_ConfigGetPath(server, module, (char *)0L);
    cfg = ns_malloc(sizeof(Config));
    cfg->deferaccept = Ns_ConfigBool(path, "deferaccept", NS_FALSE);
    cfg->nodelay = Ns_ConfigBool(path, "nodelay", NS_TRUE);

    init.version = NS_DRIVER_VERSION_4;
    init.name         = "nssock";
    init.listenProc   = SockListen;
    init.acceptProc   = SockAccept;
    init.recvProc     = SockRecv;
    init.sendProc     = SockSend;
    init.sendFileProc = SendFile;
    init.keepProc     = Keep;
    init.requestProc  = NULL;
    init.closeProc    = SockClose;
    init.opts         = NS_DRIVER_ASYNC;
    init.arg          = cfg;
    init.path         = (char*)path;
    init.protocol     = "http";
    init.defaultPort  = 80;

    return Ns_DriverInit(server, module, &init);
}


/*
 *----------------------------------------------------------------------
 *
 * SockListen --
 *
 *      Open a listening TCP socket in non-blocking mode.
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

    sock = Ns_SockListenEx(address, port, backlog, reuseport);
    if (sock != NS_INVALID_SOCKET) {
        Config *cfg = driver->arg;

        (void) Ns_SockSetNonBlocking(sock);
        if (cfg->deferaccept != 0) {
            Ns_SockSetDeferAccept(sock, driver->recvwait);
        }
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * SockAccept --
 *
 *      Accept a new TCP socket in non-blocking mode.
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
    Config *cfg    = sock->driver->arg;
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
        setsockopt(sock->sock, SOL_SOCKET,SO_SNDLOWAT, &value, sizeof(value));
#endif
        (void)Ns_SockSetNonBlocking(sock->sock);
        SetNodelay(sock->driver, sock->sock);
        status = (cfg->deferaccept != 0) ? NS_DRIVER_ACCEPT_DATA : NS_DRIVER_ACCEPT;
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
SockSend(Ns_Sock *sock, const struct iovec *bufs, int nbufs,
         const Ns_Time *UNUSED(timeoutPtr), unsigned int flags)
{
    ssize_t   sent;
    bool      decork;

    decork = Ns_SockCork(sock, NS_TRUE);

    sent = Ns_SockSendBufs2(sock->sock, bufs, nbufs, flags);

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
SendFile(Ns_Sock *sock, Ns_FileVec *bufs, int nbufs,
         Ns_Time *UNUSED(timeoutPtr), unsigned int flags)
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


static void
SetNodelay(Ns_Driver *driver, NS_SOCKET sock)
{
#ifdef TCP_NODELAY
    Config *cfg;

    NS_NONNULL_ASSERT(driver != NULL);

    cfg = driver->arg;
    if (cfg->nodelay != 0) {
        int value = 1;

        if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                       (const void *)&value, sizeof(value)) == -1) {
            Ns_Log(Error, "nssock: setsockopt(TCP_NODELAY): %s",
                   ns_sockstrerror(ns_sockerrno));
        } else {
            Ns_Log(Debug, "nodelay: socket option TCP_NODELAY activated");
        }
    }
#endif
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
