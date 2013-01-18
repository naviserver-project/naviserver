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

NS_EXPORT int Ns_ModuleVersion = 1;


typedef struct Config {
    int        deferaccept;  /* Enable the TCP_DEFER_ACCEPT optimization. */
    int        nodelay;      /* Enable the TCP_NODEALY optimization. */
} Config;

/*
 * Local functions defined in this file.
 */

static Ns_DriverListenProc Listen;
static Ns_DriverAcceptProc Accept;
static Ns_DriverRecvProc Recv;
static Ns_DriverSendProc Send;
static Ns_DriverSendFileProc SendFile;
static Ns_DriverKeepProc Keep;
static Ns_DriverCloseProc Close;

static void SetNodelay(Ns_Driver *driver, NS_SOCKET sock);


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

NS_EXPORT int
Ns_ModuleInit(char *server, char *module)
{
    Ns_DriverInitData  init;
    Config            *cfg;
    CONST char        *path;

    path = Ns_ConfigGetPath(server, module, NULL);
    cfg = ns_malloc(sizeof(Config));
    cfg->deferaccept = Ns_ConfigBool(path, "deferaccept", NS_FALSE);
    cfg->nodelay = Ns_ConfigBool(path, "nodelay", NS_FALSE);

    init.version = NS_DRIVER_VERSION_2;
    init.name = "nssock";
    init.listenProc = Listen;
    init.acceptProc = Accept;
    init.recvProc = Recv;
    init.sendProc = Send;
    init.sendFileProc = SendFile;
    init.keepProc = Keep;
    init.requestProc = NULL;
    init.closeProc = Close;
    init.opts = NS_DRIVER_ASYNC;
    init.arg = cfg;
    init.path = (char*)path;

    return Ns_DriverInit(server, module, &init);
}


/*
 *----------------------------------------------------------------------
 *
 * Listen --
 *
 *      Open a listening TCP socket in non-blocking mode.
 *
 * Results:
 *      The open socket or INVALID_SOCKET on error.
 *
 * Side effects:
 *      Enable TCP_DEFER_ACCEPT if available.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
Listen(Ns_Driver *driver, CONST char *address, int port, int backlog)
{
    NS_SOCKET sock;

    sock = Ns_SockListenEx((char*)address, port, backlog);
    if (sock != INVALID_SOCKET) {
	Config *cfg = driver->arg;

        (void) Ns_SockSetNonBlocking(sock);
	if (cfg->deferaccept) {
	    Ns_SockSetDeferAccept(sock, driver->recvwait);
	}
    }
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Accept --
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
Accept(Ns_Sock *sock, NS_SOCKET listensock,
       struct sockaddr *sockaddrPtr, int *socklenPtr)
{
    Config *cfg    = sock->driver->arg;
    int     status = NS_DRIVER_ACCEPT_ERROR;

    sock->sock = Ns_SockAccept(listensock, sockaddrPtr, socklenPtr);
    if (sock->sock != INVALID_SOCKET) {

#ifdef __APPLE__
      /* 
       * Darwin's poll returns per default writable in situations,
       * where nothing can be written.  Setting the socket option for
       * the send low watermark to 1 fixes this problem.
       */
        int value = 1;
	setsockopt(sock->sock, SOL_SOCKET,SO_SNDLOWAT, &value, sizeof(value));
#endif
        Ns_SockSetNonBlocking(sock->sock);
	SetNodelay(sock->driver, sock->sock);
        status = cfg->deferaccept
            ? NS_DRIVER_ACCEPT_DATA : NS_DRIVER_ACCEPT;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Recv --
 *
 *      Receive data into given buffers.
 *
 * Results:
 *      Total number of bytes received or -1 on error or timeout.
 *
 * Side effects:
 *      May block once for driver recvwait timeout seconds if no data
 *      available on first attempt.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Recv(Ns_Sock *sock, struct iovec *bufs, int nbufs,
     Ns_Time *timeoutPtr, int flags)
{
    ssize_t n;
    
    n = Ns_SockRecvBufs(sock->sock, bufs, nbufs, timeoutPtr, flags);
    if (n == 0) {
	/* this means usually eof, return value of 0 means in the driver SOCK_MORE */
	n = -1;
    }
    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * Send --
 *
 *      Send data from given buffers.
 *
 * Results:
 *      Total number of bytes sent or -1 on error or timeout.
 *
 * Side effects:
 *      May block once for driver sendwait timeout seconds if first
 *      attempt would block.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
Send(Ns_Sock *sockPtr, struct iovec *bufs, int nbufs,
     Ns_Time *timeoutPtr, int flags)
{
    ssize_t   n;
    int       decork;
    NS_SOCKET sock = sockPtr->sock;

    decork = Ns_SockCork(sockPtr, 1);

#ifdef _WIN32
    DWORD n1;
    if (WSASend(sock, (LPWSABUF)bufs, nbufs, &n1, flags,
                NULL, NULL) != 0) {
        n1 = -1;
    }
    n = n1;
#else
    {
	struct msghdr msg;
      
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = bufs;
	msg.msg_iovlen = nbufs;
	
	n = sendmsg(sock, &msg, flags);
	
	if (n < 0) {
	    Ns_Log(Debug, "SockSend: %s",
		   ns_sockstrerror(ns_sockerrno));
	}
    }
#endif
    if (decork) {
      Ns_SockCork(sockPtr, 0);
    }
    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * SendFile --
 *
 *      Send given file buffers directly to socket.
 *
 * Results:
 *      Total number of bytes sent or -1 on error or timeout.
 *
 * Side effects:
 *      May block once for driver sendwait timeout seconds if first
 *      attempt would block.
 *      May block 1 or more times due to disk IO.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SendFile(Ns_Sock *sock, Ns_FileVec *bufs, int nbufs,
         Ns_Time *timeoutPtr, int flags)
{
    return Ns_SockSendFileBufs(sock, bufs, nbufs, timeoutPtr, flags);
}


/*
 *----------------------------------------------------------------------
 *
 * Keep --
 *
 *      We are always to try keepalive if the upper layers are.
 *
 * Results:
 *      1, always.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
Keep(Ns_Sock *sock)
{
    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * Close --
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
Close(Ns_Sock *sock)
{
    if (sock->sock > -1) {
        ns_sockclose(sock->sock);
        sock->sock = -1;
    }
}


static void
SetNodelay(Ns_Driver *driver, NS_SOCKET sock)
{
    Config *cfg = driver->arg;

    if (cfg->nodelay) {
#ifdef TCP_NODELAY
	int value = 1;

        if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                       &value, sizeof(value)) == -1) {
            Ns_Log(Error, "nssock: setsockopt(TCP_NODELAY): %s",
                   ns_sockstrerror(ns_sockerrno));
        }
#endif
    }
}
