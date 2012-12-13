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
 * sock.c --
 *
 *      Wrappers and convenience functions for TCP/IP stuff.
 */

#include "nsd.h"

#ifndef INADDR_NONE
#define INADDR_NONE -1
#endif


/*
 * Local functions defined in this file
 */

static NS_SOCKET SockConnect(char *host, int port, char *lhost, int lport,
			     int async);
static NS_SOCKET SockSetup(NS_SOCKET sock);
static int SockRecv(NS_SOCKET sock, struct iovec *bufs, int nbufs, int flags);
static int SockSend(NS_SOCKET sock, struct iovec *bufs, int nbufs, int flags);



/*
 *----------------------------------------------------------------------
 *
 * Ns_SetVec --
 *
 *      Set the fields of the given iovec.
 *
 * Results:
 *      The given length.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_SetVec(struct iovec *iov, int i, CONST void *data, size_t len)
{
    iov[i].iov_base = (void *) data;
    iov[i].iov_len = len;

    return len;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ResetVec --
 *
 *      Zero the bufs which have had their data sent and adjust
 *      the remainder.
 *
 * Results:
 *      Index of first buf to send.
 *
 * Side effects:
 *      Updates offset and length members.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ResetVec(struct iovec *iov, int nbufs, size_t sent)
{
    int     i;
    char   *data;
    size_t  len;

    for (i = 0; i < nbufs && sent > 0; i++) {

        data = iov[i].iov_base;
        len  = iov[i].iov_len;

        if (len > 0) {
            if (sent >= len) {
                sent -= len;
                Ns_SetVec(iov, i, NULL, 0);
            } else {
                Ns_SetVec(iov, i, data + sent, len - sent);
                break;
            }
        }
    }
    return i;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SumVec --
 *
 *      Count the bytes in all buffers.
 *
 * Results:
 *      Total length of all buffers.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_SumVec(struct iovec *bufs, int nbufs)
{
    int     i;
    size_t  sum = 0;

    for (i = 0; i < nbufs; i++) {
        if (bufs[i].iov_len > 0) {
            sum += bufs[i].iov_len;
        }
    }
    return sum;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockRecvBufs --
 *
 *      Read data from a non-blocking socket into a vector of buffers.
 *
 * Results:
 *      Number of bytes read or -1 on error.
 *
 * Side effects:
 *      May wait for given timeout if first attempt would block.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockRecvBufs(NS_SOCKET sock, struct iovec *bufs, int nbufs,
                Ns_Time *timeoutPtr, int flags)
{
    int n;

    n = SockRecv(sock, bufs, nbufs, flags);
    if (n < 0
        && ns_sockerrno == EWOULDBLOCK
        && Ns_SockTimedWait(sock, NS_SOCK_READ, timeoutPtr) == NS_OK) {
        n = SockRecv(sock, bufs, nbufs, flags);
    }

    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSendBufs --
 *
 *      Send a vector of buffers on a non-blocking socket.
 *
 * Results:
 *      Number of bytes sent or -1 on error.
 *
 * Side effects:
 *      May wait for given timeout if first attempt would block.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockSendBufs(NS_SOCKET sock, struct iovec *bufs, int nbufs,
                Ns_Time *timeoutPtr, int flags)
{
    int           sbufLen, sbufIdx = 0, nsbufs = 0, bufIdx = 0;
    int           nwrote = 0, sent = -1;
    void         *data;
    size_t        len, towrite = 0;
    struct iovec  sbufs[UIO_MAXIOV], *sbufPtr;

    sbufPtr = sbufs;
    sbufLen = UIO_MAXIOV;

    while (bufIdx < nbufs || towrite > 0) {

        /*
         * Send up to UIO_MAXIOV buffers of data at a time and strip out
         * empty buffers.
         */

        while (bufIdx < nbufs && sbufIdx < sbufLen) {

            data = bufs[bufIdx].iov_base;
            len  = bufs[bufIdx].iov_len;

            if (len > 0 && data != NULL) {
                towrite += Ns_SetVec(sbufPtr, sbufIdx++, data, len);
                nsbufs++;
            }
            bufIdx++;
        }

        /*
         * Timeout once if first attempt would block.
         */

        sent = SockSend(sock, sbufPtr, nsbufs, flags);
        if (sent < 0
            && ns_sockerrno == EWOULDBLOCK
            && Ns_SockTimedWait(sock, NS_SOCK_WRITE, timeoutPtr) == NS_OK) {
            sent = SockSend(sock, sbufPtr, nsbufs, flags);
        }
        if (sent < 0) {
            break;
        }

        towrite -= sent;
        nwrote  += sent;

        if (towrite > 0) {

            sbufIdx = Ns_ResetVec(sbufPtr, nsbufs, sent);
            nsbufs -= sbufIdx;

            /*
             * If there are more whole buffers to send, move the remaining unsent
             * buffers to the beginning of the iovec array so that we always send
             * the maximum number of buffers the OS can handle.
             */

            if (bufIdx < nbufs - 1) {
                memmove(sbufPtr, sbufPtr + sbufIdx, (size_t) sizeof(struct iovec) * nsbufs);
            } else {
                sbufPtr = sbufPtr + sbufIdx;
                sbufLen = nsbufs - sbufIdx;
            }
        } else {
            nsbufs = 0;
        }
        sbufIdx = 0;
    }

    return nwrote ? nwrote : sent;
}


/*
 *----------------------------------------------------------------------
 *
 * NsSockRecv --
 *
 *      Timed recv() from a non-blocking socket.
 *
 * Results:
 *      Number of bytes read
 *
 * Side effects:
 *      May wait for given timeout.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockRecv(NS_SOCKET sock, void *buf, size_t toread, Ns_Time *timePtr)
{
    int nread;

    nread = recv(sock, buf, toread, 0);

    if (nread == -1
        && ns_sockerrno == EWOULDBLOCK
        && Ns_SockTimedWait(sock, NS_SOCK_READ, timePtr) == NS_OK) {
        nread = recv(sock, buf, toread, 0);
    }

    return nread;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSend --
 *
 *      Timed send() to a non-blocking socket.
 *      NOTE: This may not write all of the data you send it!
 *
 * Results:
 *      Number of bytes written, -1 for error
 *
 * Side effects:
 *      May wait given timeout.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockSend(NS_SOCKET sock, void *buf, size_t towrite, Ns_Time *timeoutPtr)
{
    int nwrote;

    nwrote = send(sock, buf, towrite, 0);

    if (nwrote == -1
        && ns_sockerrno == EWOULDBLOCK
        && Ns_SockTimedWait(sock, NS_SOCK_WRITE, timeoutPtr) == NS_OK) {
        nwrote = send(sock, buf, towrite, 0);
    }

    return nwrote;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockTimedWait --
 *
 *      Wait for I/O.
 *
 * Results:
 *      NS_OK, NS_TIMEOUT, or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockTimedWait(NS_SOCKET sock, int what, Ns_Time *timeoutPtr)
{
    int           n, msec = -1;
    struct pollfd pfd;

    if (timeoutPtr != NULL) {
        msec = timeoutPtr->sec * 1000 + timeoutPtr->usec / 1000;
    }
    pfd.fd = sock;

    switch (what) {
    case NS_SOCK_READ:
        pfd.events = POLLIN;
        break;
    case NS_SOCK_WRITE:
        pfd.events = POLLOUT;
        break;
    case NS_SOCK_EXCEPTION:
        pfd.events = POLLPRI;
        break;
    default:
        return NS_ERROR;
        break;
    }
    pfd.revents = 0;
    do {
        n = ns_poll(&pfd, 1, msec);
    } while (n < 0 && errno == EINTR);
    if (n > 0) {
        return NS_OK;
    }

    return NS_TIMEOUT;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockWait --
 *
 *      Wait for I/O. Compatibility function for older modules.
 *
 * Results:
 *      NS_OK, NS_TIMEOUT, or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockWait(NS_SOCKET sock, int what, int timeout)
{
    Ns_Time tm = { timeout, 0 };
    return Ns_SockTimedWait(sock, what, &tm);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListen --
 *
 *      Listen for connections with default backlog.
 *
 * Results:
 *      A socket or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListen(char *address, int port)
{
    return Ns_SockListenEx(address, port, nsconf.backlog);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockAccept --
 *
 *      Accept a TCP socket, setting close on exec.
 *
 * Results:
 *      A socket or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockAccept(NS_SOCKET lsock, struct sockaddr *saPtr, int *lenPtr)
{
    NS_SOCKET sock;

    sock = accept(lsock, saPtr, (socklen_t *) lenPtr);

    if (sock != INVALID_SOCKET) {
        sock = SockSetup(sock);
    } else if (errno != EAGAIN) {
        Ns_Log(Notice, "accept() fails, reason: %s", strerror(errno));
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBind --
 *
 *      Create a TCP socket and bind it to the passed-in address.
 *
 * Results:
 *      A socket or -1 on error.
 *
 * Side effects:
 *      Will set SO_REUSEADDR on the socket.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_BindSock(struct sockaddr_in *saPtr)
{
    return Ns_SockBind(saPtr);
}

NS_SOCKET
Ns_SockBind(struct sockaddr_in *saPtr)
{
    NS_SOCKET sock;
    int       n;

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock != INVALID_SOCKET) {
        sock = SockSetup(sock);
    }
    if (sock != INVALID_SOCKET) {
        n = 1;
        if (saPtr->sin_port != 0) {
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                       (char *) &n, sizeof(n));
        }
        if (bind(sock, (struct sockaddr *) saPtr,
                 sizeof(struct sockaddr_in)) != 0) {
            ns_sockclose(sock);
            sock = INVALID_SOCKET;
        }
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockConnect --
 *
 *      Open a TCP connection to a host/port.
 *
 * Results:
 *      A socket, or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockConnect(char *host, int port)
{
    return SockConnect(host, port, NULL, 0, 0);
}

NS_SOCKET
Ns_SockConnect2(char *host, int port, char *lhost, int lport)
{
    return SockConnect(host, port, lhost, lport, 0);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockAsyncConnect --
 *
 *      Like Ns_SockConnect, but uses a nonblocking socket.
 *
 * Results:
 *      A socket, or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockAsyncConnect(char *host, int port)
{
    return SockConnect(host, port, NULL, 0, 1);
}

NS_SOCKET
Ns_SockAsyncConnect2(char *host, int port, char *lhost, int lport)
{
    return SockConnect(host, port, lhost, lport, 1);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockTimedConnect --
 *
 *      Like Ns_SockConnect, but with an optional timeout in seconds.
 *
 * Results:
 *      A socket, or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockTimedConnect(char *host, int port, Ns_Time *timePtr)
{
    return Ns_SockTimedConnect2(host, port, NULL, 0, timePtr);
}

NS_SOCKET
Ns_SockTimedConnect2(char *host, int port, char *lhost, int lport,
                     Ns_Time *timePtr)
{
    NS_SOCKET sock;
    int       err;
    socklen_t len;

    /*
     * Connect to the host asynchronously and wait for
     * it to connect.
     */

    sock = SockConnect(host, port, lhost, lport, 1);

    if (sock != INVALID_SOCKET) {
        len = sizeof(err);
        err = Ns_SockTimedWait(sock, NS_SOCK_WRITE, timePtr);
        switch (err) {
        case NS_OK:
            len = sizeof(err);
            if (!getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len)) {
                return sock;
            }
            break;
        case NS_TIMEOUT:
            errno = ETIMEDOUT;
            break;
        default:
            break;
        }
        ns_sockclose(sock);
        sock = INVALID_SOCKET;
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSetNonBlocking --
 *
 *      Set a socket nonblocking.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockSetNonBlocking(NS_SOCKET sock)
{
    int nb = 1;

    if (ns_sockioctl(sock, FIONBIO, &nb) == -1) {
        return NS_ERROR;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSetBlocking --
 *
 *      Set a socket blocking.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockSetBlocking(NS_SOCKET sock)
{
    int nb = 0;

    if (ns_sockioctl(sock, FIONBIO, &nb) == -1) {
        return NS_ERROR;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * SetDeferAccept --
 *
 *      Tell the OS not to give us a new socket until data is available.
 *      This saves overhead in the poll() loop and the latency of a RT.
 *
 *      Otherwise, we will get socket as soon as the TCP connection
 *      is established.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Disabled by default as Linux seems broken (does not respect
 *      the timeout, linux-2.6.26).
 *
 *----------------------------------------------------------------------
 */

void
Ns_SockSetDeferAccept(NS_SOCKET sock, int secs)
{
#ifdef TCP_FASTOPEN_UNTESTED
    int qlen = 5;
  
    if (setsockopt(sock, IPPROTO_TCP, TCP_FASTOPEN,
		   &sec, sizeof(qlen)) == -1) {
	Ns_Log(Error, "sock: setsockopt(TCP_FASTOPEN): %s",
	       ns_sockstrerror(ns_sockerrno));
    }
#else
# ifdef TCP_DEFER_ACCEPT
    if (setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT,
		   &secs, sizeof(secs)) == -1) {
	Ns_Log(Error, "sock: setsockopt(TCP_DEFER_ACCEPT): %s",
	       ns_sockstrerror(ns_sockerrno));
    }
# else
#  ifdef SO_ACCEPTFILTER
    struct accept_filter_arg afa;
    int n;
    
    memset(&afa, 0, sizeof(afa));
    strcpy(afa.af_name, "httpready");
    n = setsockopt(sock, SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa));
    if (n < 0) {
	Ns_Log(Error, "sock: setsockopt(SO_ACCEPTFILTER): %s",
	       ns_sockstrerror(ns_sockerrno));
    }
#  endif
# endif
#endif
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_GetSockAddr --
 *
 *      Take a host/port and fill in a sockaddr_in structure
 *      appropriately. Host may be an IP address or a DNS name.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      May perform DNS query.
 *
 *----------------------------------------------------------------------
 */

int
Ns_GetSockAddr(struct sockaddr_in *saPtr, char *host, int port)
{
    struct in_addr ia;
    Ns_DString     ds;

    if (host == NULL) {
        ia.s_addr = htonl(INADDR_ANY);
    } else {
        ia.s_addr = inet_addr(host);
        if (ia.s_addr == INADDR_NONE) {
            Ns_DStringInit(&ds);
            if (Ns_GetAddrByHost(&ds, host) == NS_TRUE) {
                ia.s_addr = inet_addr(ds.string);
            }
            Ns_DStringFree(&ds);
            if (ia.s_addr == INADDR_NONE) {
                return NS_ERROR;
            }
        }
    }

    memset(saPtr, 0, sizeof(struct sockaddr_in));
    saPtr->sin_family = AF_INET;
    saPtr->sin_addr = ia;
    saPtr->sin_port = htons((unsigned short) port);

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockPipe --
 *
 *      Create a pair of unix-domain sockets.
 *
 * Results:
 *      See socketpair(2)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockPipe(NS_SOCKET socks[2])
{
    if (ns_sockpair(socks) != 0) {
        return NS_ERROR;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockCloseLater --
 *
 *      Register a callback to close a socket when writable.  This
 *      is necessary for timed-out async connecting sockets on NT.
 *
 * Results:
 *      NS_OK or NS_ERROR from Ns_SockCallback.
 *
 * Side effects:
 *      Socket will be closed sometime in the future.
 *
 *----------------------------------------------------------------------
 */

static int
CloseLater(NS_SOCKET sock, void *arg, int why)
{
    ns_sockclose(sock);
    return NS_FALSE;
}

int
Ns_SockCloseLater(NS_SOCKET sock)
{
    return Ns_SockCallback(sock, CloseLater, NULL, NS_SOCK_WRITE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockErrno --
 *
 *      Errno/GetLastError utility routines.
 *
 * Results:
 *      See code.
 *
 * Side effects:
 *      May set last error.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ClearSockErrno(void)
{
#ifdef _WIN32
    SetLastError(0);
#else
    errno = 0;
#endif
}

int
Ns_GetSockErrno(void)
{
#ifdef _WIN32
    return (int) WSAGetLastError();
#else
    return errno;
#endif
}

void
Ns_SetSockErrno(int err)
{
#ifdef _WIN32
    SetLastError((DWORD) err);
#else
    errno = err;
#endif
}

char *
Ns_SockStrError(int err)
{
#ifdef _WIN32
    return NsWin32ErrMsg(err);
#else
    return strerror(err);
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * NsPoll --
 *
 *      Poll file descriptors using an absolute timeout and restarting
 *      after any interrupts which may be received.
 *
 * Results:
 *      See poll(2) man page.
 *
 * Side effects:
 *      See poll(2) man page.
 *
 *----------------------------------------------------------------------
 */

int
NsPoll(struct pollfd *pfds, int nfds, Ns_Time *timeoutPtr)
{
    Ns_Time now, diff;
    int     i, n, ms;

    /*
     * Clear revents.
     */

    for (i = 0; i < nfds; ++i) {
        pfds[i].revents = 0;
    }

    /*
     * Determine relative time from absolute time and continue polling
     * if any interrupts are received.
     */

    do {
        if (timeoutPtr == NULL) {
            ms = -1;
        } else {
            Ns_GetTime(&now);
            if (Ns_DiffTime(timeoutPtr, &now, &diff) <= 0) {
                ms = 0;
            } else {
                ms = diff.sec * 1000 + diff.usec / 1000;
            }
        }
        n = ns_poll(pfds, (size_t) nfds, ms);
    } while (n < 0 && ns_sockerrno == EINTR);

    /*
     * Poll errors are not tolerated in as they must indicate
     * a code error which if ignored could lead to data loss and/or
     * endless polling loops and error messages.
     */

    if (n < 0) {
        Ns_Fatal("ns_poll() failed: %s", ns_sockstrerror(ns_sockerrno));
    }

    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * SockConnect --
 *
 *      Open a TCP connection to a host/port.
 *
 * Results:
 *      A socket or -1 on error.
 *
 * Side effects:
 *      If async is true, the returned socket will be nonblocking.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
SockConnect(char *host, int port, char *lhost, int lport, int async)
{
    NS_SOCKET          sock;
    struct sockaddr_in lsa;
    struct sockaddr_in sa;

    if (Ns_GetSockAddr(&sa, host, port) != NS_OK ||
        Ns_GetSockAddr(&lsa, lhost, lport) != NS_OK) {
        return INVALID_SOCKET;
    }
    sock = Ns_SockBind(&lsa);
    if (sock != INVALID_SOCKET) {
        if (async) {
            Ns_SockSetNonBlocking(sock);
        }
        if (connect(sock, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
            int err = ns_sockerrno;
            if (!async || (err != EINPROGRESS && err != EWOULDBLOCK)) {
                ns_sockclose(sock);
                sock = INVALID_SOCKET;
            }
        }
        if (async && sock != INVALID_SOCKET) {
            Ns_SockSetBlocking(sock);
        }
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * SockSetup --
 *
 *      Setup new sockets for close-on-exec and possibly duped high.
 *
 * Results:
 *      Current or duped socket.
 *
 * Side effects:
 *      Original socket is closed if duped.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
SockSetup(NS_SOCKET sock)
{
#ifdef USE_DUPHIGH
    int nsock;

    nsock = fcntl(sock, F_DUPFD, 256);
    if (nsock != -1) {
    close(sock);
    sock = nsock;
    }
#endif
#ifndef _WIN32
    (void) fcntl(sock, F_SETFD, 1);
#endif
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * SockRecv --
 *
 *      Read data from a non-blocking socket into a vector of buffers.
 *
 * Results:
 *      Number of bytes read or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SockRecv(NS_SOCKET sock, struct iovec *bufs, int nbufs, int flags)
{
    int n;

#ifdef _WIN32
    if (WSARecv(sock, (LPWSABUF)bufs, nbufs, &n, &flags,
                NULL, NULL) != 0) {
        n = -1;
    }

    return n;
#else
    struct msghdr msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = bufs;
    msg.msg_iovlen = nbufs;

    n = recvmsg(sock, &msg, flags);

    if (n < 0) {
        Ns_Log(Debug, "SockRecv: %s",
               ns_sockstrerror(ns_sockerrno));
    }
    return n;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * SockSend --
 *
 *      Send a vector of buffers on a non-blocking socket. Not all
 *      data may be sent.
 *
 * Results:
 *      Number of bytes sent or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SockSend(NS_SOCKET sock, struct iovec *bufs, int nbufs, int flags)
{
#ifdef _WIN32
    DWORD n;
    if (WSASend(sock, (LPWSABUF)bufs, nbufs, &n, flags,
                NULL, NULL) != 0) {
        n = -1;
    }

    return n;
#else
    int n;
    struct msghdr msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = bufs;
    msg.msg_iovlen = nbufs;

    n = sendmsg(sock, &msg, flags);

    if (n < 0) {
        Ns_Log(Debug, "SockSend: %s",
               ns_sockstrerror(ns_sockerrno));
    }
    return n;
#endif
}
