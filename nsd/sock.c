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

/*
 * TCP_FASTOPEN was introduced in Linux 3.7.0. At the time of this
 * writing, TCP_FASTOPEN is just defined in linux/tcp.h, which we
 * can't include here (testing with FC18)
 */
#ifdef HAVE_TCP_FASTOPEN
# ifndef TCP_FASTOPEN
#  define TCP_FASTOPEN           23      /* Enable FastOpen on listeners */
# endif
#endif

#if defined(__APPLE__) && defined(__MACH__)
# include <AvailabilityMacros.h>
#endif


/*
 * Local functions defined in this file
 */

static NS_SOCKET SockConnect(const char *host, unsigned short port,
                             const char *lhost, unsigned short lport,
                             bool async) NS_GNUC_NONNULL(1);

static NS_SOCKET SockSetup(NS_SOCKET sock);

static ssize_t SockRecv(NS_SOCKET sock, struct iovec *bufs, int nbufs,
                        unsigned int flags);

static ssize_t SockSend(NS_SOCKET sock, struct iovec *bufs, int nbufs,
                        unsigned int flags);

static NS_SOCKET BindToSameFamily(struct sockaddr *saPtr,
                                  struct sockaddr *lsaPtr,
                                  const char *lhost, unsigned short lport)
                                  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static NS_INLINE bool Retry(int errorCode) NS_GNUC_CONST;

static Ns_SockProc CloseLater;



/*
 *----------------------------------------------------------------------
 *
 * Retry --
 *
 *      Boolean function to check whether the provided error code entails a
 *      retry. This is defined as an inline function rathen than a macro to
 *      avoid potentially multiple calls to GetLastError(), used for
 *      "ns_sockerrno" under windows.
 *
 * Results:
 *      Boolean value
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static NS_INLINE bool Retry(int errorCode)
{
    return (errorCode == NS_EAGAIN || errorCode == NS_EINTR || errorCode == NS_EWOULDBLOCK);
}


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
Ns_SetVec(struct iovec *bufs, int i, const void *data, size_t len)
{
    bufs[i].iov_base = (void *) data;
    bufs[i].iov_len = len;

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
Ns_ResetVec(struct iovec *bufs, int nbufs, size_t sent)
{
    int     i;

    for (i = 0; i < nbufs && sent > 0u; i++) {
        const char *data = bufs[i].iov_base;
        size_t      len  = bufs[i].iov_len;

        if (len > 0u) {
            if (sent >= len) {
                sent -= len;
                (void) Ns_SetVec(bufs, i, NULL, 0u);
            } else {
                (void) Ns_SetVec(bufs, i, data + sent, len - sent);
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
Ns_SumVec(const struct iovec *bufs, int nbufs)
{
    register int i;
    size_t       sum = 0u;

    NS_NONNULL_ASSERT(bufs != NULL);

    for (i = 0; i < nbufs; i++) {
        if (bufs[i].iov_len > 0u) {
            sum += bufs[i].iov_len;
        }
    }
    return sum;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSetReceiveState --
 *
 *      Set the sockState of the last receive operation in the Sock structure.
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
Ns_SockSetReceiveState(Ns_Sock *sock, Ns_SockState sockState)
{
    NS_NONNULL_ASSERT(sock != NULL);

    ((Sock *)sock)->recvSockState = sockState;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockInErrorState --
 *
 *      Check the error State of a ns_sock structure.
 *
 *      Background: SSL_shutdown() must not be called if a previous fatal error
 *      has occurred on a connection i.e. if SSL_get_error() has returned
 *      SSL_ERROR_SYSCALL or SSL_ERROR_SSL.
 *
 *      Note: For the time being, we have just the read error state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockInErrorState(Ns_Sock *sock)
{
    NS_NONNULL_ASSERT(sock != NULL);

    return (((Sock *)sock)->recvSockState == NS_SOCK_EXCEPTION);
}




/*
 *----------------------------------------------------------------------
 *
 * Ns_SockRecvBufs --
 *
 *      Read data from a non-blocking socket into a vector of buffers.
 *      When the timeoutPtr is given, wait max the given time until
 *      the data is readable.
 *
 * Results:
 *      Number of bytes read or -1 on error (including timeout).  The return
 *      value will be 0 when the peer has performed an orderly shutdown. The
 *      resulting sockstate has one of the following codes:
 *
 *      NS_SOCK_READ, NS_SOCK_DONE, NS_SOCK_AGAIN, NS_SOCK_EXCEPTION,
 *      NS_SOCK_TIMEOUT
 *
 * Side effects:
 *      May wait for given timeout if first attempt would block.
 *
 *----------------------------------------------------------------------
 */
ssize_t
Ns_SockRecvBufs(Ns_Sock *sock, struct iovec *bufs, int nbufs,
                const Ns_Time *timeoutPtr, unsigned int flags)
{
    ssize_t      n;
    Ns_SockState sockState = NS_SOCK_READ;
    Sock        *sockPtr = (Sock *)sock;

    NS_NONNULL_ASSERT(sock != NULL);

    n = Ns_SockRecvBufs2(sock->sock, bufs, nbufs, flags, &sockState);
    if (sockState == NS_SOCK_AGAIN) {
        /*
         * If a timeoutPtr was provided, perform timeout handling.
         */
        if (timeoutPtr != NULL) {
            Ns_ReturnCode status;

            status = Ns_SockTimedWait(sock->sock,
                                      (unsigned int)NS_SOCK_READ,
                                      timeoutPtr);
            if (status == NS_OK) {
                n = SockRecv(sock->sock, bufs, nbufs, flags);
            } else if (status == NS_TIMEOUT) {
                sockState = NS_SOCK_TIMEOUT;
            } else {
                sockState = NS_SOCK_EXCEPTION;
            }
        }
    }
    sockPtr->recvSockState = sockState;

    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockRecvBufs2 --
 *
 *      Read data from a non-blocking socket into a vector of buffers.
 *      Ns_SockRecvBufs2() is similar to Ns_SockRecvBufs() with the following
 *      differences:
 *        a) the first argument is a NS_SOCKET
 *        b) it performs no timeout handliong
 *        c) it returns the sockstate in its last argument
 *
 * Results:
 *      Number of bytes read or -1 on error.  The return
 *      value will be 0 when the peer has performed an orderly shutdown. The
 *      resulting sockstate has one of the following codes:
 *
 *      NS_SOCK_READ, NS_SOCK_DONE, NS_SOCK_AGAIN, NS_SOCK_EXCEPTION
 *
 * Side effects:
 *      May wait for given timeout if first attempt would block.
 *
 *----------------------------------------------------------------------
 */
ssize_t
Ns_SockRecvBufs2(NS_SOCKET sock, struct iovec *bufs, int nbufs,
                 unsigned int flags, Ns_SockState *sockStatePtr)
{
    ssize_t      n;
    Ns_SockState sockState = NS_SOCK_READ;

    NS_NONNULL_ASSERT(bufs != NULL);

    n = SockRecv(sock, bufs, nbufs, flags);

    if (unlikely(n == -1)) {
        if (Retry(ns_sockerrno)) {
            /*
             * Resource is temporarily unavailable.
             */
            sockState = NS_SOCK_AGAIN;
        } else {
            /*
             * Some other error.
             */
            sockState = NS_SOCK_EXCEPTION;
        }
    } else if (unlikely(n == 0)) {
        /*
         * Peer has performed an orderly shutdown.
         */
        sockState = NS_SOCK_DONE;
    }

    *sockStatePtr = sockState;

    return n;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSendBufs --
 *
 *      Send a vector of buffers on a non-blocking socket.
 *      Promises to send all of the data.
 *
 * Results:
 *      Number of bytes sent or -1 on error.
 *
 * Side effects:
 *      May block, waiting for a writable socket.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SockSendBufs(Ns_Sock *sock, const struct iovec *bufs, int nbufs,
                const Ns_Time *timeoutPtr, unsigned int flags)
{
    int           sbufLen, sbufIdx = 0, nsbufs = 0, bufIdx = 0;
    size_t        toWrite = 0u;
    ssize_t       nWrote = 0;
    struct iovec  sbufs[UIO_MAXIOV], *sbufPtr;

    NS_NONNULL_ASSERT(sock != NULL);
    NS_NONNULL_ASSERT(bufs != NULL);

    assert(nbufs >= 1);

    sbufPtr = sbufs;
    sbufLen = UIO_MAXIOV;

    while (bufIdx < nbufs || toWrite > 0u) {

        ssize_t sent;

        /*
         * Send up to UIO_MAXIOV buffers at a time
         * while stripping out empty buffers.
         */

        while (bufIdx < nbufs && sbufIdx < sbufLen) {
            const void *data;
            size_t len;

            data = bufs[bufIdx].iov_base;
            len  = bufs[bufIdx].iov_len;

            if (len > 0u && data != NULL) {
                toWrite += Ns_SetVec(sbufPtr, sbufIdx++, data, len);
                nsbufs++;
            }

            bufIdx++;
        }

        sent = NsDriverSend((Sock *)sock, sbufPtr, nsbufs, flags);

        if (sent == 0
            && Ns_SockTimedWait(sock->sock, (unsigned int)NS_SOCK_WRITE,
                                timeoutPtr) == NS_OK) {
            sent = NsDriverSend((Sock *)sock, sbufPtr, nsbufs, flags);
        }
        if (sent == -1) {
            nWrote = -1;
            break;
        }

        toWrite -= (size_t)sent;
        nWrote  += sent;

        if (toWrite == 0u) {
            nsbufs = 0;
        } else {

            sbufIdx = Ns_ResetVec(sbufPtr, nsbufs, (size_t)sent);
            nsbufs -= sbufIdx;

            /*
             * If there are more whole buffers to send, move the remaining
             * unsent buffers to the beginning of the iovec array so that
             * we always send the maximum number of buffers the OS can handle.
             */

            if (bufIdx < (nbufs - 1)) {
                assert(nsbufs > 0);
                memmove(sbufPtr, sbufPtr + sbufIdx, sizeof(struct iovec) * (size_t)nsbufs);
            } else {
                sbufPtr = sbufPtr + sbufIdx;
                sbufLen = nsbufs - sbufIdx;
            }
        }
        sbufIdx = 0;
    }

    return nWrote;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSendBufs2 --
 *
 *      Send a vector of buffers on a non-blocking socket.
 *      It is similar to Ns_SockSendBufs() except that it
 *        a) receives a NS_SOCK as first argument
 *        b) it does not care about partial writes,
 *           it simply returns the number of bytes sent.
 *        c) it never blocks
 *        d) it does not try corking
 *
 * Results:
 *      Number of bytes sent (which might be also 0 on EAGAIN cases)
 *      or -1 on error.
 *
 * Side effects:
 *      none
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SockSendBufs2(NS_SOCKET sock, const struct iovec *bufs, int nbufs,
                 unsigned int flags)
{
    ssize_t sent = 0;

    NS_NONNULL_ASSERT(bufs != NULL);

    sent = SockSend(sock, (struct iovec *)bufs, nbufs, flags);

    if (unlikely(sent == -1)) {
        if (Retry(ns_sockerrno)) {
            /*
             * Resource is temporarily unavailable.
             */
            sent = 0;
        }
    }

    return sent;
}


/*
 *----------------------------------------------------------------------
 *
 * NsSockRecv --
 *
 *      Timed recv operation from a non-blocking socket.
 *
 * Results:
 *      Number of bytes read, -1 on error.
 *
 * Side effects:
 *      May wait for given timeout.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SockRecv(NS_SOCKET sock, void *buffer, size_t length,
            const Ns_Time *timeoutPtr)
{
    ssize_t nread;

    NS_NONNULL_ASSERT(buffer != NULL);

    nread = ns_recv(sock, buffer, length, 0);

    if (unlikely(nread == -1)) {
        if (Retry(ns_sockerrno)) {
            if (Ns_SockTimedWait(sock, (unsigned int)NS_SOCK_READ,
                                 timeoutPtr) == NS_OK) {
                nread = ns_recv(sock, buffer, length, 0);
            }
        }
    }

    return nread;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSend --
 *
 *      Timed send operation to a non-blocking socket.
 *
 * Results:
 *      Number of bytes written, -1 for error
 *
 * Side effects:
 *      May wait for given timeout.
 *      May not write all of the data.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SockSend(NS_SOCKET sock, const void *buffer, size_t length,
            const Ns_Time *timeoutPtr)
{
    ssize_t nwrote;

    NS_NONNULL_ASSERT(buffer != NULL);

    nwrote = ns_send(sock, buffer, length, 0);

    if (unlikely(nwrote == -1)) {
        if (Retry(ns_sockerrno)) {
            if (Ns_SockTimedWait(sock, (unsigned int)NS_SOCK_WRITE,
                                 timeoutPtr) == NS_OK) {
                nwrote = ns_send(sock, buffer, length, 0);
            }
        }
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
 *      NS_OK, NS_ERROR, or NS_TIMEOUT.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_SockTimedWait(NS_SOCKET sock, unsigned int what, const Ns_Time *timeoutPtr)
{
    int           n, msec = -1;
    struct pollfd pfd;
    Ns_ReturnCode result;
    short         requestedEvents, count = 0;

    if (timeoutPtr != NULL) {
        msec = (int)(timeoutPtr->sec * 1000 + timeoutPtr->usec / 1000);
    }
    pfd.fd = sock;
    pfd.events = 0;

    if ((what & (unsigned int)NS_SOCK_READ) != 0u) {
        pfd.events |= (short)POLLIN;
    }
    if ((what & (unsigned int)NS_SOCK_WRITE) != 0u) {
        pfd.events |= (short)POLLOUT;
    }
    if ((what & (unsigned int)NS_SOCK_EXCEPTION) != 0u) {
        pfd.events |= (short)POLLPRI;
    }
    requestedEvents = pfd.events;

    for (;;) {
        pfd.revents = 0;
        n = ns_poll(&pfd, (NS_POLL_NFDS_TYPE)1, msec);
        if (n == -1 && Retry(ns_sockerrno)) {
            count ++;
            continue;
        }
        break;
    };

    if (count > 1) {
        Ns_Log(Warning, "Ns_SockTimedWait on sock %d tried %d times, returns n %d",
               sock, count, n);
    }

    if (likely(n > 0)) {
        if ((pfd.revents & requestedEvents) == 0) {
            socklen_t len = sizeof(errno);

            getsockopt(sock, SOL_SOCKET, SO_ERROR, &errno, &len);
            /*Ns_Log(Warning, "Ns_SockTimedWait on sock %d returns events %.4x (errno %d <%s>)",
              sock, pfd.revents, errno, strerror(errno));*/

            result = NS_ERROR;
        } else {
            result = NS_OK;
        }
    } else if (n == 0) {
        result = NS_TIMEOUT;
    } else {
        /*Ns_Log(Warning, "Ns_SockTimedWait on sock %d returns error", sock);*/
        result = NS_ERROR;
    }

    return result;
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

Ns_ReturnCode
Ns_SockWait(NS_SOCKET sock, unsigned int what, int timeout)
{
    Ns_Time t;

    t.sec  = timeout;
    t.usec = 0;

    return Ns_SockTimedWait(sock, what, &t);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListen --
 *
 *      Listen for connections with default backlog.
 *
 * Results:
 *      A socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListen(const char *address, unsigned short port)
{
    return Ns_SockListenEx(address, port, nsconf.backlog, NS_FALSE);  // TODO: currently no parameter defined
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockAccept --
 *
 *      Accept a TCP socket, setting close on exec.
 *
 * Results:
 *      A socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockAccept(NS_SOCKET sock, struct sockaddr *saPtr, socklen_t *lenPtr)
{
    sock = accept(sock, saPtr, lenPtr);

    Ns_Log(Debug, "Ns_SockAccept returns sock %d, err %s", sock,
           (ns_sockerrno == 0) ? "NONE" : ns_sockstrerror(ns_sockerrno));

    if (likely(sock != NS_INVALID_SOCKET)) {
        sock = SockSetup(sock);
    } else if (ns_sockerrno != 0 && ns_sockerrno != NS_EAGAIN) {
        Ns_Log(Warning, "accept() fails, reason: %s",
               ns_sockstrerror(ns_sockerrno));
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
 *      A socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      Will set SO_REUSEADDR always on the socket, SO_REUSEPORT
 *      optionally.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_BindSock(const struct sockaddr *saPtr)
{
    return Ns_SockBind(saPtr, NS_FALSE);
}

NS_SOCKET
Ns_SockBind(const struct sockaddr *saPtr, bool reusePort)
{
    NS_SOCKET sock;

    NS_NONNULL_ASSERT(saPtr != NULL);
    Ns_LogSockaddr(Debug, "Ns_SockBind called with", (const struct sockaddr *) saPtr);

    sock = (NS_SOCKET)socket((int)saPtr->sa_family, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (sock != NS_INVALID_SOCKET) {

#if defined(SO_REUSEPORT)
        if (reusePort) {
            int optval = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &optval, (socklen_t)sizeof(optval));
        }
#endif
        sock = SockSetup(sock);
    }

    if (sock != NS_INVALID_SOCKET) {
        unsigned short port = Ns_SockaddrGetPort((const struct sockaddr *)saPtr);

        if (port != 0u) {
            int n = 1;

            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void *) &n, (socklen_t)sizeof(n));
#ifdef HAVE_IPV6
            /*
             * IPv4 connectivity through AF_INET6 can be disabled by default, for
             * example by /proc/sys/net/ipv6/bindv6only to 1 on Linux. We
             * explicitly enable IPv4 so we don't need to bind separate sockets
             * for v4 and v6.
             */
            n = 0;
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const void *) &n, (socklen_t)sizeof(n));
#endif
        }
        Ns_LogSockaddr(Debug, "trying to bind on", (const struct sockaddr *) saPtr);

        if (bind(sock, (const struct sockaddr *)saPtr,
                 Ns_SockaddrGetSockLen((const struct sockaddr *)saPtr)) != 0) {

            Ns_Log(Notice, "bind operation on sock %d lead to error: %s", sock, ns_sockstrerror(ns_sockerrno));
            Ns_LogSockaddr(Warning, "bind on", (const struct sockaddr *) saPtr);
            ns_sockclose(sock);
            sock = NS_INVALID_SOCKET;
        }

        if (port == 0u) {
            /*
             * Refetch the socket structure containing the potentially fresh port
             */
            socklen_t socklen = Ns_SockaddrGetSockLen((const struct sockaddr *)saPtr);

            (void) getsockname(sock, (struct sockaddr *)saPtr, &socklen);
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
 *      A socket, or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockConnect(const char *host, unsigned short port)
{
    NS_NONNULL_ASSERT(host != NULL);

    return SockConnect(host, port, NULL, 0u, NS_FALSE);
}

NS_SOCKET
Ns_SockConnect2(const char *host, unsigned short port, const char *lhost, unsigned short lport)
{
    NS_NONNULL_ASSERT(host != NULL);

    return SockConnect(host, port, lhost, lport, NS_FALSE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockAsyncConnect --
 *
 *      Like Ns_SockConnect, but uses a nonblocking socket.
 *
 * Results:
 *      A socket, or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockAsyncConnect(const char *host, unsigned short port)
{
    NS_NONNULL_ASSERT(host != NULL);

    return SockConnect(host, port, NULL, 0u, NS_TRUE);
}

NS_SOCKET
Ns_SockAsyncConnect2(const char *host, unsigned short port, const char *lhost, unsigned short lport)
{
    NS_NONNULL_ASSERT(host != NULL);

    return SockConnect(host, port, lhost, lport, NS_TRUE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockTimedConnect --
 *
 *      Like Ns_SockConnect, but with an optional timeout in seconds.
 *
 * Results:
 *      A socket, or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockTimedConnect(const char *host, unsigned short port, const Ns_Time *timeoutPtr)
{
    NS_NONNULL_ASSERT(host != NULL);
    NS_NONNULL_ASSERT(timeoutPtr != NULL);

    return Ns_SockTimedConnect2(host, port, NULL, 0, timeoutPtr, NULL);
}

NS_SOCKET
Ns_SockTimedConnect2(const char *host, unsigned short port, const char *lhost,
                     unsigned short lport,
                     const Ns_Time *timeoutPtr, Ns_ReturnCode *statusPtr)
{
    NS_SOCKET     sock;
    socklen_t     len;
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(host != NULL);
    NS_NONNULL_ASSERT(timeoutPtr != NULL);

    /*
     * Connect to the host asynchronously and wait for
     * it to connect.
     */

    sock = SockConnect(host, port, lhost, lport, NS_TRUE);
    if (unlikely(sock == NS_INVALID_SOCKET)) {
        /*Ns_Log(Warning, "SockConnect returned invalid socket");*/
        status = NS_ERROR;

    } else {
        /*Ns_Log(Notice, "SockConnect returned socket %d, wait for write (errno %d <%s>)",
          sock, errno, ns_sockstrerror(errno));*/

        status = Ns_SockTimedWait(sock, (unsigned int)NS_SOCK_WRITE, timeoutPtr);
        /*Ns_Log(Notice, "Ns_SockTimedWait on %d returned status %d error %d <%s>",
          sock, status, errno, ns_sockstrerror(errno));*/
        switch (status) {
        case NS_OK:
            {
                int err;
                /*
                 * Try to reset error status
                 */
                len = (socklen_t)sizeof(err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&err, &len) == -1) {
                    Ns_Log(Warning, "getsockopt %d returned error %d <%s>",
                           sock, errno, ns_sockstrerror(errno));
                    status = NS_ERROR;
                }
                break;
            }
        case NS_TIMEOUT:
            errno = ETIMEDOUT;
            break;

        case NS_ERROR:         NS_FALL_THROUGH; /* fall through */
        case NS_FILTER_BREAK:  NS_FALL_THROUGH; /* fall through */
        case NS_FILTER_RETURN: NS_FALL_THROUGH; /* fall through */
        case NS_FORBIDDEN:     NS_FALL_THROUGH; /* fall through */
        case NS_UNAUTHORIZED:
            break;
        }
        /*
         * Return in all error cases an invalid socket.
         */
        if (status != NS_OK) {
            ns_sockclose(sock);
            sock = NS_INVALID_SOCKET;
        }
    }

    /*
     * When a statusPtr is provided, return the status code. The client can
     * determine, if e.g. a timeout occurred.
     */
    if (statusPtr != NULL) {
        *statusPtr = status;
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockConnectError --
 *
 *      Leave a consistent error message in the interpreter result in case
 *      a connect attempt failed. For timeout cases, set the Tcl error code to
 *      "NS_TIMEOUT".
 *
 * Results:
 *      NONE
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SockConnectError(Tcl_Interp *interp, const char *host, unsigned short portNr,
                    Ns_ReturnCode status)
{
    NS_NONNULL_ASSERT(host != NULL);

    if (status == NS_TIMEOUT) {
        Ns_TclPrintfResult(interp, "timeout while connecting to %s port %hu", host, portNr);
        Tcl_SetErrorCode(interp, "NS_TIMEOUT", (char *)0L);
    } else {
        /*
         * Tcl_PosixError() sets as well the Tcl error code.
         */
        Ns_TclPrintfResult(interp, "can't connect to %s port %hu: %s",
                           host, portNr,
                           (Tcl_GetErrno() != 0) ?  Tcl_PosixError(interp) : "reason unknown");
    }
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

Ns_ReturnCode
Ns_SockSetNonBlocking(NS_SOCKET sock)
{
    Ns_ReturnCode status;

    if (ns_sock_set_blocking(sock, NS_FALSE) == -1) {
        status = NS_ERROR;
    } else {
        status = NS_OK;
    }
    return status;
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

Ns_ReturnCode
Ns_SockSetBlocking(NS_SOCKET sock)
{
    Ns_ReturnCode status;

    if (ns_sock_set_blocking(sock, NS_TRUE) == -1) {
        status = NS_ERROR;
    } else {
        status = NS_OK;
    }
    return status;
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
Ns_SockSetDeferAccept(NS_SOCKET sock, long secs)
{
#ifdef TCP_FASTOPEN
# if defined(__APPLE__) && defined(__MACH__)
    int qlen = 1;
# else
    int qlen = 5;
# endif

    if (setsockopt(sock, IPPROTO_TCP, TCP_FASTOPEN,
                   (const void *)&qlen, (socklen_t)sizeof(qlen)) == -1) {
        Ns_Log(Error, "deferaccept setsockopt(TCP_FASTOPEN): %s",
               ns_sockstrerror(ns_sockerrno));
    } else {
        Ns_Log(Notice, "deferaccept: socket option TCP_FASTOPEN activated");
    }
    (void)secs;
#else
# ifdef TCP_DEFER_ACCEPT
    if (setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT,
                   (const void *)&secs, (socklen_t)sizeof(secs)) == -1) {
        Ns_Log(Error, "deferaccept setsockopt(TCP_DEFER_ACCEPT): %s",
               ns_sockstrerror(ns_sockerrno));
    } else {
        Ns_Log(Notice, "deferaccept: socket option DEFER_ACCEPT activated (timeout %ld)", secs);
    }
# else
#  ifdef SO_ACCEPTFILTER
    struct accept_filter_arg afa;
    int n;

    memset(&afa, 0, sizeof(afa));
    strncpy(afa.af_name, "httpready", sizeof(afa.af_name));
    n = setsockopt(sock, SOL_SOCKET, SO_ACCEPTFILTER, &afa, (socklen_t)sizeof(afa));
    if (n == -1) {
        Ns_Log(Error, "deferaccept setsockopt(SO_ACCEPTFILTER): %s",
               ns_sockstrerror(ns_sockerrno));
    } else {
        Ns_Log(Notice, "deferaccept: socket option SO_ACCEPTFILTER activated");

    }
    (void)secs;
#  endif
# endif
#endif
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

Ns_ReturnCode
Ns_SockPipe(NS_SOCKET socks[2])
{
    Ns_ReturnCode status;

    NS_NONNULL_ASSERT(socks != NULL);

    if (ns_sockpair(socks) != 0) {
        status = NS_ERROR;
    } else {
        status = NS_OK;
    }

    return status;
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

static bool
CloseLater(NS_SOCKET sock, void *UNUSED(arg), unsigned int UNUSED(why))
{
    int rc = ns_sockclose(sock);
    return (rc == 0 ? NS_TRUE : NS_FALSE);
}

Ns_ReturnCode
Ns_SockCloseLater(NS_SOCKET sock)
{
    return Ns_SockCallback(sock, CloseLater, NULL, (unsigned int)NS_SOCK_WRITE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ClearSockErrno, Ns_GetSockErrno, Ns_SetSockErrno, Ns_SockStrError  --
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
    SetLastError(0u);
#else
    errno = 0;
#endif
}

ns_sockerrno_t
Ns_GetSockErrno(void)
{
    return ns_sockerrno;
}

void
Ns_SetSockErrno(ns_sockerrno_t err)
{
#ifdef _WIN32
    SetLastError(err);
#else
    errno = err;
#endif
}

char *
Ns_SockStrError(ns_sockerrno_t err)
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
NsPoll(struct pollfd *pfds, NS_POLL_NFDS_TYPE nfds, const Ns_Time *timeoutPtr)
{
    Ns_Time now, diff;
    int     n, ms;
    NS_POLL_NFDS_TYPE i;

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
                ms = (int)(diff.sec * 1000 + diff.usec / 1000);
            }
        }
        n = ns_poll(pfds, nfds, ms);
    } while (n < 0 && ns_sockerrno == NS_EINTR);

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
 * BindToSameFamily --
 *
 *       We have to make sure that the local address (where the local bind
 *       happens) is of the same address family, which is especially important
 *       for (lhost == NULL), where the caller has no chance to influence the
 *       behavior, and we assume per default AF_INET6.
 *
 * Results:
 *      A socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *
 *      Pententially updating sockat address info pointed by lsaPtr.
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
BindToSameFamily(struct sockaddr *saPtr, struct sockaddr *lsaPtr,
                 const char *lhost, unsigned short lport)
{
    NS_SOCKET     sock;
    Ns_ReturnCode result;

    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(lsaPtr != NULL);

    /*
     * The resolving of the host was successful.
     */
    result = Ns_GetSockAddr(lsaPtr,
#ifdef HAVE_IPV6
                            ((saPtr->sa_family == AF_INET) && (lhost == NULL)) ? "0.0.0.0" : lhost,
#else
                            lhost,
#endif
                            lport);
    if (result != NS_OK) {
        Ns_Log(Notice, "SockConnect: cannot resolve to local address %s %d", lhost, lport);
        sock = NS_INVALID_SOCKET;
    } else {
        /*
         *
         */
        sock = Ns_SockBind(lsaPtr, NS_FALSE);
        if (sock != NS_INVALID_SOCKET) {

#if defined(__APPLE__) && defined(__MACH__)
# if defined(MAC_OS_X_VERSION_10_13)
            {
                /*
                 * macOS High Sierra raises "Broken pipe: 13" errors
                 * for the http.test regression test. It seems to
                 * ignore the setting
                 *
                 *     ns_sigmask(SIG_BLOCK, &set, NULL);
                 *
                 * in unix.c where "set" contains SIGPIPE. Therefore,
                 * we turn off SIGPPIPE directly on the socket.
                 */
                int set = 1;
                setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));
            }
# endif
#endif
        }
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * SockConnect --
 *
 *      Open a TCP connection to a host/port sync or async.  "host" and "port"
 *      refer to the remote, "lhost" and "lport" to the local communication
 *      endpoint.
 *
 * Results:
 *      A socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *      If async is true, the returned socket will be set temporarily
 *      nonblocking.
 *
 *----------------------------------------------------------------------
 */


static NS_SOCKET
SockConnect(const char *host, unsigned short port, const char *lhost,
            unsigned short lport, bool async)
{
    NS_SOCKET             sock;
    bool                  success;
    Tcl_DString           ds;

    /*Ns_Log(Notice, "SockConnect calls Ns_GetSockAddr %s %hu", host, port);*/

    Tcl_DStringInit(&ds);
    success = Ns_GetAllAddrByHost(&ds, host);

    if (!success) {
        Ns_Log(Warning, "SockConnect could not resolve host %s", host);
        sock = NS_INVALID_SOCKET;
    } else {

        /*
         * We could resolve the name. Potentially, the resolving of the name
         * led to multiple IP addresses. We have to try all of these, until a
         * connection succeeds.
         */
        struct NS_SOCKADDR_STORAGE sa, lsa;
        char            *addresses = ds.string;
        bool             multipleIPs = (strchr(addresses, INTCHAR(' ')) != NULL);
        struct sockaddr *saPtr = (struct sockaddr *)&sa, *lsaPtr = (struct sockaddr *)&lsa;

        if (multipleIPs) {
            Ns_Log(Debug, "SockConnect: target host <%s> has associated multiple IP addresses <%s>",
                   host, addresses);
        }
        sock = NS_INVALID_SOCKET;

        for (;;) {
            Ns_ReturnCode  result;
            const char    *address = ns_strtok(addresses, " ");

            /*
             * In the next iteration, process the next address.
             */
            addresses = NULL;
            if (address == NULL) {
                break;
            }

            Ns_Log(Debug, "SockConnect to %s: try address <%s> async %d", host, address, async);

            result = Ns_GetSockAddr(saPtr, address, port);
            if (result == NS_OK) {

                sock = BindToSameFamily(saPtr, lsaPtr, lhost, lport);
                if (sock == NS_INVALID_SOCKET) {
                    /*
                     * Maybe there is another address of some other family
                     * available for the bind.
                     */
                    continue;
                }

                if (async) {
                    if (Ns_SockSetNonBlocking(sock) != NS_OK) {
                        Ns_Log(Warning, "attempt to set socket nonblocking failed");
                    }
                }

                if (connect(sock, saPtr, Ns_SockaddrGetSockLen(saPtr)) != 0) {
                    ns_sockerrno_t err = ns_sockerrno;

                    if ((err != NS_EINPROGRESS) && (err != NS_EWOULDBLOCK)) {
                        Ns_Log(Notice, "connect on sock %d async %d err %d <%s>",
                               sock, async, err, ns_sockstrerror(err));
                    }

                    if (async && ((err == NS_EINPROGRESS) || (err == NS_EWOULDBLOCK))) {
                        /*
                         * The code below is implemented also in later
                         * calls. However in the async case, it is hard to
                         * recover and retry in these cases. Therefore, if we
                         * have multiple IP addresses, and async handling, we
                         * wait for the writable state, and pereform finally a
                         * getsockopt() here. We might loose some concurrency,
                         * but the handling is this way much easier.
                         */
                        if (multipleIPs) {
                            struct pollfd sockfd;
                            socklen_t len;

                            if (err == NS_EWOULDBLOCK) {
                                Ns_Log(Debug, "async connect to %s on sock %d returned NS_EWOULDBLOCK",
                                       address, sock);
                            } else {
                                Ns_Log(Debug, "async connect to %s on sock %d returned EINPROGRESS",
                                       address, sock);
                            }
                            sockfd.events = POLLOUT;
                            sockfd.revents = 0;
                            sockfd.fd = sock;
                            /*
                             * Wait for max 100ms for the socket to become
                             * writable, otherwise use the next offered IP
                             * address. TODO: The waiting timespan should be
                             * probably configurable.
                             */
                            (void) ns_poll(&sockfd, 1, 100);

                            /*
                             * poll() finished - either with a timeout or
                             * success. Actually we don't care, since the
                             * getsockopt() for returning out the error helps
                             * us to decide, whether to continiue with this IP
                             * address or not.
                             */
                            len = sizeof(errno);
                            getsockopt(sock, SOL_SOCKET, SO_ERROR, &errno, &len);

                            /*Ns_Log(Notice, "async connect on sock %d, poll returned %d revents %.4x, extraerr %d <%s>",
                              sock, n, sockfd.revents, errno, ns_sockstrerror(errno));*/
                            if (errno != 0) {
                                Ns_Log(Notice, "multiple & async on connect to %s fails on sock %d err %d <%s>",
                                       address, sock, errno, ns_sockstrerror(errno));
                                ns_sockclose(sock);
                                sock = NS_INVALID_SOCKET;
                                continue;
                            } else {
                                Ns_Log(Debug, "async connect multipleIPs INPROGRESS sock %d continue", sock);
                                break;
                            }

                        }
                    } else if (err != 0) {
                        Ns_Log(Notice, "close sock %d due to error err %d <%s>",
                               sock, err, ns_sockstrerror(err));
                        ns_sockclose(sock);
                        Ns_LogSockaddr(Warning, "SockConnect fails", saPtr);
                        sock = NS_INVALID_SOCKET;
                        continue;
                    }
                } else {
                    Ns_Log(Debug, "connect on sock %d async %d succeeded", sock, async);
                    break;
                }
            }
            //if (async && (sock != NS_INVALID_SOCKET)) {
            //if (Ns_SockSetBlocking(sock) != NS_OK) {
            //    Ns_Log(Warning, "attempt to set socket blocking failed");
            //}
            //}
        }
    }
    Tcl_DStringFree(&ds);
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
 *      When USE_DUPHIGH is activated the original socket is closed.
 *
 *
 *----------------------------------------------------------------------
 */

static NS_SOCKET
SockSetup(NS_SOCKET sock)
{
#ifdef USE_DUPHIGH
    NS_SOCKET nsock;

    nsock = fcntl(sock, F_DUPFD, 256);
    if (nsock != NS_INVALID_SOCKET) {
      ns_sockclose(sock);
      sock = nsock;
    }
#endif
#if !defined(_WIN32)
    (void) fcntl(sock, F_SETFD, FD_CLOEXEC);
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
 *
 *      Number of bytes read or -1 on error.  The return value will be 0 when
 *      the peer has performed an orderly shutdown (as defined by POSIX).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SockRecv(NS_SOCKET sock, struct iovec *bufs, int nbufs, unsigned int flags)
{
    ssize_t numBytes = 0;

#ifdef _WIN32
    DWORD RecvBytes = 0, Flags = (DWORD)flags;

    if (WSARecv(sock, (LPWSABUF)bufs, (unsigned long)nbufs, &RecvBytes,
                &Flags, NULL, NULL) != 0) {
        numBytes = -1;
    } else {
        numBytes = (ssize_t)RecvBytes;
    }
#else
    struct msghdr msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = bufs;
    msg.msg_iovlen = (NS_MSG_IOVLEN_T)nbufs;
    numBytes = recvmsg(sock, &msg, (int)flags);
#endif

    if (numBytes == -1) {
        Ns_Log(Debug, "SockRecv: %s", ns_sockstrerror(ns_sockerrno));
    }

    return numBytes;
}


/*
 *----------------------------------------------------------------------
 *
 * SockSend --
 *
 *      Send data to a non-blocking socket from a vector of buffers.
 *
 * Results:
 *
 *      Number of bytes written
 *      -1 on error, partial write or write to non-writable socket
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SockSend(NS_SOCKET sock, struct iovec *bufs, int nbufs, unsigned int flags)
{
    ssize_t numBytes = 0;

#ifdef _WIN32
    DWORD nrBytesSent = 0;

    if (WSASend(sock, (LPWSABUF)bufs, (unsigned long)nbufs, &nrBytesSent,
                (DWORD)flags, NULL, NULL) == -1) {
        numBytes = -1;
    } else {
        numBytes = (ssize_t)nrBytesSent;
    }
#else
    struct msghdr msg;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = bufs;
    msg.msg_iovlen = (NS_MSG_IOVLEN_T)nbufs;
    numBytes = sendmsg(sock, &msg, (int)flags);
#endif

    if (numBytes == -1) {
        Ns_Log(Debug, "SockSend: %d, %s", sock, ns_sockstrerror(ns_sockerrno));
    }

    return numBytes;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
