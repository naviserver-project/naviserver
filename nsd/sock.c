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

static Ns_ReturnCode WaitForConnect(NS_SOCKET sock);

static NS_SOCKET SockSetup(NS_SOCKET sock);

static ssize_t SockRecv(NS_SOCKET sock, struct iovec *bufs, int nbufs,
                        unsigned int flags);

static ssize_t SockSend(NS_SOCKET sock, const struct iovec *bufs, int nbufs,
                        unsigned int flags);

static NS_SOCKET BindToSameFamily(struct sockaddr *saPtr,
                                  struct sockaddr *lsaPtr,
                                  const char *lhost, unsigned short lport)
                                  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static NS_INLINE bool Retry(int errorCode) NS_GNUC_CONST;

static Ns_SockProc CloseLater;

static const char *ErrorCodeString(int errorCode) NS_GNUC_PURE;


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
 *      Boolean value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static NS_INLINE bool
Retry(int errorCode)
{
    return (errorCode == NS_EAGAIN
            || errorCode == NS_EINTR
#if defined(__APPLE__)
            /*
             * Due to a possible kernel bug at least in OS X 10.10 "Yosemite",
             * EPROTOTYPE can be returned while trying to write to a socket
             * that is shutting down. If we retry the write, we should get
             * the expected EPIPE instead.
             */
            || errorCode == EPROTOTYPE
#endif
            || errorCode == NS_EWOULDBLOCK);
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
 *      Reduce the size in the iovec by the number of bytes sent (last
 *      argument). Zero the bufs which have had their data sent and
 *      adjust the remainder.
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
    int i;

    for (i = 0; (i < nbufs) && (sent > 0u); i++) {
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
Ns_SockSetReceiveState(Ns_Sock *sock, Ns_SockState sockState, unsigned long recvErrno)
{
    NS_NONNULL_ASSERT(sock != NULL);

    ((Sock *)sock)->recvSockState = sockState;
    ((Sock *)sock)->recvErrno = recvErrno;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockGetPort --
 *
 *      Get the port of the provided Sock structure from the IPv4 or
 *      IPv6 sock addr.
 *
 * Results:
 *      Port or 0 on errors.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
unsigned short
Ns_SockGetPort(const Ns_Sock *sock)
{
    unsigned short result;
    struct NS_SOCKADDR_STORAGE sa;
    socklen_t len = (socklen_t)sizeof(sa);
    int       retVal;

    NS_NONNULL_ASSERT(sock != NULL);

    retVal = getsockname(sock->sock, (struct sockaddr *) &sa, &len);
    if (retVal == -1) {
        result = 0u;
    } else {
        result = Ns_SockaddrGetPort((struct sockaddr *)&sa);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockGetLocalAddr --
 *
 *      Get the IP Address in form of a string from the provided NS_SOCKET.
 *
 * Results:
 *      String or NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_SockGetAddr(const Ns_Sock *sock)
{
    const char *result;
    struct NS_SOCKADDR_STORAGE sa;
    socklen_t len = (socklen_t)sizeof(sa);
    int       retVal;

    NS_NONNULL_ASSERT(sock != NULL);

    retVal = getsockname(sock->sock, (struct sockaddr *) &sa, &len);
    if (retVal == -1) {
        result = 0u;
    } else {
        result = ns_inet_ntoa((struct sockaddr *)&sa);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockInErrorState --
 *
 *      Check the error State of an ns_sock structure.
 *
 *      Background: SSL_shutdown() must not be called if a previous
 *      fatal error has occurred on a connection i.e. if SSL_get_error()
 *      has returned SSL_ERROR_SYSCALL or SSL_ERROR_SSL.
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
Ns_SockInErrorState(const Ns_Sock *sock)
{
    NS_NONNULL_ASSERT(sock != NULL);

    return (((const Sock *)sock)->recvSockState == NS_SOCK_EXCEPTION);
}




/*
 *----------------------------------------------------------------------
 *
 * Ns_SockRecvBufs --
 *
 *      Read data from a nonblocking socket into a vector of buffers.
 *      When the timeoutPtr is given, wait max the given time until
 *      the data is readable.
 *
 * Results:
 *      Number of bytes read or -1 on error (including timeout).  The
 *      return value will be 0 when the peer has performed an orderly
 *      shutdown. The resulting sockstate is set in sockPtr and has one
 *      of the following codes:
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
    ssize_t        nrBytes;
    Ns_SockState   sockState = NS_SOCK_READ;
    unsigned long  recvErrno = 0u;
    Sock          *sockPtr = (Sock *)sock;

    NS_NONNULL_ASSERT(sock != NULL);

    nrBytes = Ns_SockRecvBufs2(sock->sock, bufs, nbufs, flags, &sockState, &recvErrno);
    if (sockState == NS_SOCK_AGAIN && timeoutPtr != NULL) {
        /*
         * When a timeoutPtr is provided, perform timeout handling.
         */
        Ns_ReturnCode status;

        status = Ns_SockTimedWait(sock->sock,
                                  (unsigned int)NS_SOCK_READ,
                                  timeoutPtr);
        if (status == NS_OK) {
            nrBytes = SockRecv(sock->sock, bufs, nbufs, flags);
        } else if (status == NS_TIMEOUT) {
            sockState = NS_SOCK_TIMEOUT;
        } else {
            sockState = NS_SOCK_EXCEPTION;
            recvErrno = (unsigned long)ns_sockerrno;
        }
    }
    sockPtr->recvSockState = sockState;
    if (sockState == NS_SOCK_EXCEPTION) {
        sockPtr->recvErrno = recvErrno;
    }

    return nrBytes;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockRecvBufs2 --
 *
 *      Read data from a nonblocking socket into a vector of buffers.
 *      Ns_SockRecvBufs2() is similar to Ns_SockRecvBufs() with the
 *      following differences:
 *
 *        a) the first argument is an NS_SOCKET
 *        b) it performs no timeout handliong
 *        c) it returns the sockstate in its last argument
 *
 * Results:
 *      Number of bytes read or -1 on error.  The return value will be 0
 *      when the peer has performed an orderly shutdown. The resulting
 *      sockstate has one of the following codes:
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
                 unsigned int flags, Ns_SockState *sockStatePtr,
                 unsigned long *errnoPtr)
{
    ssize_t      n;
    Ns_SockState sockState = NS_SOCK_READ;

    NS_NONNULL_ASSERT(bufs != NULL);
    NS_NONNULL_ASSERT(errnoPtr != NULL);

    n = SockRecv(sock, bufs, nbufs, flags);

    if (unlikely(n == -1)) {
        int sockerrno = ns_sockerrno;

        if (Retry(sockerrno)) {
            /*
             * Resource is temporarily unavailable.
             */
            sockState = NS_SOCK_AGAIN;
        } else {
            /*
             * Some other error.
             */
            Ns_Log(Debug, "Ns_SockRecvBufs2 errno %d on sock %d: %s",
                   sockerrno, sock, strerror(sockerrno));
            sockState = NS_SOCK_EXCEPTION;
        }
        *errnoPtr = (unsigned long)sockerrno;
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
 *      Send a vector of buffers on a nonblocking socket.
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

        if (unlikely(Ns_LogSeverityEnabled(Debug)
                     && sent != -1
                     && sent != (ssize_t)toWrite)
            ) {
            Ns_Log(Debug, "Ns_SockSendBufs partial write: want to send %" PRIdz
                   " bytes, sent %" PRIdz " timeoutPtr %p",
                   toWrite, sent, (void*)timeoutPtr);
        }
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
 *      Send a vector of buffers on a nonblocking socket.
 *      It is similar to Ns_SockSendBufs() except that it
 *        a) receives an NS_SOCK as first argument
 *        b) it does not care about partial writes,
 *           it simply returns the number of bytes sent.
 *        c) it never blocks
 *        d) it does not try corking
 *
 * Results:
 *      Number of bytes sent (which might be also 0 on NS_EAGAIN cases)
 *      or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SockSendBufs2(NS_SOCKET sock, const struct iovec *bufs, int nbufs,
                 unsigned int flags)
{
    ssize_t sent;

    NS_NONNULL_ASSERT(bufs != NULL);

    sent = SockSend(sock, bufs, nbufs, flags);

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
 * Ns_SockRecv --
 *
 *      Timed recv operation from a nonblocking socket.
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
 *      Timed send operation to a nonblocking socket.
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

    nwrote = ns_send(sock, buffer, length, MSG_NOSIGNAL|MSG_DONTWAIT);

    if (unlikely(nwrote == -1)) {
        if (Retry(ns_sockerrno)) {
            if (Ns_SockTimedWait(sock, (unsigned int)NS_SOCK_WRITE,
                                 timeoutPtr) == NS_OK) {
                nwrote = ns_send(sock, buffer, length, MSG_NOSIGNAL|MSG_DONTWAIT);
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
    int           n = 0, pollTimeout;
    struct pollfd pfd;
    Ns_ReturnCode result = NS_OK;
    short         requestedEvents, count = 0;

    /*
     * If there is no timeout specified, set pollTimeout to "-1", meaning an
     * infinite poll timeout. Otherwise compute the milliseconds from the
     * specified timeout values. Note that the timeout might be "0",
     * meaning that the poll() call will return immediately.
     */
    if (timeoutPtr != NULL) {
        pollTimeout = (int)Ns_TimeToMilliseconds(timeoutPtr);
    } else {
        pollTimeout = -1;
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
        n = ns_poll(&pfd, (NS_POLL_NFDS_TYPE)1, pollTimeout);
        if (n == -1 && Retry(ns_sockerrno)) {
            count ++;
            continue;
        }
        break;
    }

    if (count > 1) {
        Ns_Log(Debug, "Ns_SockTimedWait on sock %d: tried %d times, returns n %d",
               sock, count, n);
    }

    if (likely(n > 0)) {
        if ((pfd.revents & requestedEvents) == 0) {
            Ns_Log(Debug, "Ns_SockTimedWait on sock %d: event mismatch, expected"
                   " %.4x received %.4x", sock, requestedEvents, pfd.revents);
            result = NS_ERROR;
        } else {

            Ns_Log(Debug, "Ns_SockTimedWait on sock %d: poll returned %d,"
                   " requested 0x%.2x"
                   " expected 0x%.4x received 0x%.4x", sock, n, what, requestedEvents, pfd.revents);

            if (((what & (unsigned int)NS_SOCK_READ) != 0u && ((pfd.events & POLLIN) != 0u))
                || ((what & (unsigned int)NS_SOCK_WRITE) != 0u && ((pfd.events & POLLOUT) != 0u))
                ) {
                /*
                 * We got a "readable" for a "read" request or a "writable"
                 * for a "write" request, so everything is fine.
                 */
                Ns_Log(Debug, "Ns_SockTimedWait on sock %d got what we wanted", sock);
            } else {
                int       err, sockerrno = 0;
                socklen_t len = sizeof(sockerrno);

                /*
                 * We did not get, what we wanted, so it must be some error.
                 */
                err = getsockopt(sock, SOL_SOCKET, SO_ERROR, (void*)&sockerrno, &len);
                if (err == -1) {
                    Ns_Log(Debug, "Ns_SockTimedWait getsockopt on sock %d failed "
                           "errno %d <%s>",
                           sock, errno, strerror(errno));
                    result = NS_ERROR;
                } else if (sockerrno != 0) {
                    Ns_Log(Debug, "Ns_SockTimedWait getsockopt on sock %d retrieved error "
                           "errno %d <%s>",
                           sock, sockerrno, strerror(sockerrno));

                    result = NS_ERROR;
                }
            }
        }
    } else if (n == 0) {
        result = NS_TIMEOUT;
    } else {
        Ns_Log(Debug, "Ns_SockTimedWait on sock %d errno %d <%s>",
               sock, errno, strerror(errno));
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
    int sockerrno;

    sock = accept(sock, saPtr, lenPtr);
    sockerrno = ns_sockerrno;

    Ns_Log(Debug, "Ns_SockAccept returns sock %d, err %s", sock,
           (sockerrno == 0) ? "NONE" : ns_sockstrerror(sockerrno));

    if (likely(sock != NS_INVALID_SOCKET)) {
        sock = SockSetup(sock);
    } else if (sockerrno != 0 && sockerrno != NS_EAGAIN) {
        Ns_Log(Warning, "accept() fails, reason: %s", ns_sockstrerror(sockerrno));
    }

    return sock;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_BindSock --
 *
 *      Deprecated version of Ns_SockBind().
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
             * IPv4 connectivity through AF_INET6 can be disabled by
             * default, for example by /proc/sys/net/ipv6/bindv6only to
             * 1 on Linux. We explicitly enable IPv4 so we don't need to
             * bind separate sockets for v4 and v6.
             */
            n = 0;
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const void *) &n, (socklen_t)sizeof(n));
#endif
        }
        Ns_LogSockaddr(Debug, "trying to bind on", (const struct sockaddr *) saPtr);

        if (bind(sock, (const struct sockaddr *)saPtr,
                 Ns_SockaddrGetSockLen((const struct sockaddr *)saPtr)) != 0) {

            Ns_Log(Notice, "bind operation on sock %d lead to error: %s",
                   sock, ns_sockstrerror(ns_sockerrno));
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
 * Ns_SockConnect, Ns_SockConnect2 --
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
 * Ns_SockAsyncConnect, Ns_SockAsyncConnect2 --
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
 * Ns_SockTimedConnect, Ns_SockTimedConnect2 --
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
        /*Ns_Log(Notice, "SockConnect for %s:%hu returned socket %d, wait for write (errno %d <%s>)",
          host, port, sock, errno, ns_sockstrerror(errno));*/

        status = Ns_SockTimedWait(sock, (unsigned int)NS_SOCK_WRITE, timeoutPtr);
        /*Ns_Log(Notice, "Ns_SockTimedWait for %s:%hu on %d returned error %d <%s> -> status %d",
          host, port, sock, errno, ns_sockstrerror(errno), status);*/

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
 *      Leave a consistent error message in the interpreter result in case a
 *      connect attempt failed. For timeout cases, set the Tcl error code to
 *      "NS_TIMEOUT".
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
Ns_SockConnectError(Tcl_Interp *interp, const char *host, unsigned short portNr,
                    Ns_ReturnCode status)
{
    NS_NONNULL_ASSERT(host != NULL);

    if (status == NS_TIMEOUT) {

        /*
         * Watch: Ns_TclPrintfResult() destroys errorCode variable
         */
        Ns_TclPrintfResult(interp, "timeout while connecting to %s port %hu",
                           host, portNr);
        Ns_Log(Ns_LogTimeoutDebug, "connect to %s port %hu runs into timeout",
               host, portNr);
        Tcl_SetErrorCode(interp, "NS_TIMEOUT", (char *)0L);
    } else {
        const char *err;
        char buf[16];

        /*
         * Tcl_PosixError() maintains errorCode variable
         */
        err = (Tcl_GetErrno() != 0) ? Tcl_PosixError(interp) : "reason unknown";
        sprintf(buf, "%hu", portNr);
        Tcl_AppendResult(interp, "can't connect to ", host, " port ", buf,
                         ": ", err, (char *)0L);
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
 *      NS_OK or NS_ERROR.
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
 *      NS_OK or NS_ERROR.
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
 * Ns_SockSetNodelay --
 *
 *      Set socket option TCP_NODELAY when defined.
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
Ns_SockSetNodelay(NS_SOCKET sock)
{
#ifdef TCP_NODELAY
    int value = 1;

    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                   (const void *)&value, sizeof(value)) == -1) {
        Ns_Log(Error, "nssock(%d): setsockopt(TCP_NODELAY): %s",
               sock, ns_sockstrerror(ns_sockerrno));
    } else {
        Ns_Log(Debug, "nssock(%d): option TCP_NODELAY activated", sock);
    }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSetDeferAccept --
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
        Ns_Log(Notice, "nssock(%d): option TCP_FASTOPEN activated", sock);
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
 * Ns_SockSetKeepalive --
 *
 *      Tell the OS to activate keepalive on TCP sockets.
 *      This makes it easy to detect stale connections.
 *
 *      For details, see
 *      https://www.tldp.org/HOWTO/html_single/TCP-Keepalive-HOWTO/
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Changing socket behavior.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SockSetKeepalive(NS_SOCKET sock, int optval)
{
#ifdef SO_KEEPALIVE
    socklen_t optlen = sizeof(optval);

    if (Ns_LogSeverityEnabled(Debug)) {
        int val0 = 0;

        /*
         * Check on enabled debug the pre-existent status of the keepalive
         * socket option in order to compare it with the newly set value.
         */
        if (getsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &val0, &optlen) < 0) {
            Ns_Log(Error, "sock(%d) can't set keepalive %d: %s",
                   sock, optval, ns_sockstrerror(ns_sockerrno));

        } else if (val0 != optval) {
            if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen) < 0) {
                Ns_Log(Error, "sock(%d) can't set keepalive %d: %s",
                       sock, optval, ns_sockstrerror(ns_sockerrno));
            }
        }
        Ns_Log(Notice, "sock(%d): socket option SO_KEEPALIVE set from %d to %d",
               sock, val0, optval);
    } else {
        if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen) < 0) {
            Ns_Log(Error, "sock(%d) can't set keepalive %d: %s",
                   sock, optval, ns_sockstrerror(ns_sockerrno));
        }
    }
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
 *      See socketpair(2).
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
    return (rc == 0);
}

Ns_ReturnCode
Ns_SockCloseLater(NS_SOCKET sock)
{
    return Ns_SockCallback(sock, CloseLater, NULL, (unsigned int)NS_SOCK_WRITE);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ClearSockErrno, Ns_GetSockErrno, Ns_SetSockErrno, Ns_SockStrError --
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

const char *
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
                ms = (int)Ns_TimeToMilliseconds(&diff);
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
 *      We have to make sure that the local address (where the local
 *      bind happens) is of the same address family, which is especially
 *      important for (lhost == NULL), where the caller has no chance to
 *      influence the behavior, and we assume per default AF_INET6.
 *
 * Results:
 *      A socket or NS_INVALID_SOCKET on error.
 *
 * Side effects:
 *
 *      Potentially updating socket address info pointed by lsaPtr.
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

#ifdef SO_NOSIGPIPE
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
#endif
        }
    }

    return sock;
}

/*
 *----------------------------------------------------------------------
 *
 * WaitForConnect --
 *
 *      Handle EINPROGRESS and friends in async connect attempts.  We check
 *      after the connect, whether the socket is writable.  This works
 *      sometimes, but in some cases, poll() returns that the socket is
 *      writable, but it is still not connected. So, we double check, if we
 *      can get the peer address for this socket via getpeername(), which is
 *      only possible when connection succeeded.  Otherwise we retry a couple
 *      of times.
 *
 * Results:
 *      NS_OK on success or NS_ERROR on failure.
 *
 * Side effects:
 *
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
WaitForConnect(NS_SOCKET sock)
{
    struct pollfd sockfd;
    Ns_ReturnCode result;
    int count = 20;

    for (;;) {
        int nfds;

        sockfd.events = POLLOUT;
        sockfd.revents = 0;
        sockfd.fd = sock;

        /*
         * Wait for max 100ms for the socket to become
         * writable, otherwise use the next offered IP
         * address. TODO: The waiting timespan should be
         * probably configurable.
         */
        nfds = ns_poll(&sockfd, 1, 100);
        Ns_Log(Debug, "WaitForConnect: poll returned 0x%.4x, nsfds %d", sockfd.revents, nfds);

        if ((sockfd.revents & POLLOUT) != 0) {
            struct NS_SOCKADDR_STORAGE sa;
            socklen_t socklen = (socklen_t)sizeof(sa);

            /*
             * The socket is writable, but does it really mean, we are connected?
             */
            Ns_Log(Debug, "WaitForConnect: sock %d is writable", sock);

            /*
             * We know this for sure, when getpeername() on the socket succeeds.
             */
            if (getpeername(sock, (struct sockaddr *)&sa, &socklen) != 0) {
                if (ns_sockerrno == ENOTCONN) {
                    /*
                     * The socket is not yet connected.
                     */
                    if (count-- > 0) {
                        struct pollfd timeoutfd;
                        /*
                         * Wait 1ms and retry
                         */
                        (void) ns_poll(&timeoutfd, 0, 1);
                        Ns_Log(Debug, "WaitForConnect: sock %d ENOTCONN, left retries %d",
                               sock, count);
                        continue;
                    } else {
                        Ns_Log(Debug, "WaitForConnect: sock %d ENOTCONN, giving up", sock);
                    }
                } else {
                    Ns_Log(Debug, "WaitForConnect on sock %d: getpeername() returned error %s",
                           sock, ns_sockstrerror(ns_sockerrno));
                }
                result = NS_ERROR;
            } else {
                /*
                 * Everything is fine: socket is writable, and getpeername()
                 * returned success.
                 */
                result = NS_OK;
            }
        } else {
            /*
             * The socket is NOT writable. The behaviour on macOS differs from
             * Linux: while we see a writable socket during in-progress
             * states, we do not get this under macOS. In case, we have no
             * error state, we retry.
             */
            if (sockfd.revents == 0 && Ns_SockErrorCode(NULL, sock) == 0) {
                Ns_Log(Debug, "WaitForConnect: sock %d retry", sock);
                continue;
            }
            result = NS_ERROR;
        }
        break;
    }
    Ns_Log(Debug, "WaitForConnect: sock %d connect success %d", sock, result);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SockConnect --
 *
 *      Open a TCP connection to a host/port sync or async.  "host" and
 *      "port" refer to the remote, "lhost" and "lport" to the local
 *      communication endpoint.
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
            const char    *address;

            address = ns_strtok(addresses, " ");
            /*
             * In the next iteration, process the next address.
             */
            addresses = NULL;
            if (address == NULL) {
                Ns_Log(Debug, "SockConnect to %s: addresses exhausted", host);
                break;
            }

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
                         * calls. However, in the async case, it is hard to
                         * recover and retry in these cases. Therefore, if we
                         * have multiple IP addresses, and async handling, we
                         * wait for the writable state, and pereform finally a
                         * getsockopt() here. We might loose some concurrency,
                         * but the handling is this way much easier.
                         */
                        if (multipleIPs) {

                            if (err == NS_EWOULDBLOCK) {
                                Ns_Log(Debug, "async connect to %s on sock %d returned NS_EWOULDBLOCK",
                                       address, sock);
                            } else {
                                Ns_Log(Debug, "async connect to %s on sock %d returned EINPROGRESS",
                                       address, sock);
                            }
                            if (WaitForConnect(sock) == NS_OK) {
                                /*
                                 * The socket is connected, we can use this IP address.
                                 */
                                /*Ns_Log(Notice, "async connect multipleIPs INPROGRESS "
                                       "sock %d socket writable (address %s)",
                                       sock, address);*/
                                break;

                            } else {
                                /*
                                 * The socket could not be connected, try a next IP address.
                                 */
                                /* Ns_Log(Notice, "async connect multipleIPs INPROGRESS "
                                       "sock %d connect failed (address %s), try next",
                                       sock, address); */
                                ns_sockclose(sock);
                                sock = NS_INVALID_SOCKET;
                                continue;
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

    Ns_Log(Debug, "SockConnect to %s: returns sock %d", host, sock);

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
 *      Read data from a nonblocking socket into a vector of buffers.
 *
 * Results:
 *      Number of bytes read or -1 on error.  The return value will be 0
 *      when the peer has performed an orderly shutdown (as defined by
 *      POSIX).
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
 *      Send data to a nonblocking socket from a vector of buffers.
 *
 * Results:
 *      Number of bytes written or -1 on error, partial write or write to
 *      non-writable socket
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SockSend(NS_SOCKET sock, const struct iovec *bufs, int nbufs, unsigned int flags)
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
    msg.msg_iov = (struct iovec *)bufs;
    msg.msg_iovlen = (NS_MSG_IOVLEN_T)nbufs;
    numBytes = sendmsg(sock, &msg, (int)flags|MSG_NOSIGNAL|MSG_DONTWAIT);
#endif

    if (numBytes == -1) {
        Ns_Log(Debug, "SockSend: %d, %s", sock, ns_sockstrerror(ns_sockerrno));
    }

    return numBytes;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SockErrorCode --
 *
 *      Get the error from a socket and return it. In case, an interp is
 *      given set as well the Tcl error code.
 *
 * Results:
 *      Numeric error code or 0.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Ns_SockErrorCode(Tcl_Interp *interp, NS_SOCKET sock) {
    int       err, sockerrno = 0;
    socklen_t len = sizeof(sockerrno);

    err = getsockopt(sock, SOL_SOCKET, SO_ERROR, (void*)&sockerrno, &len);
    if (interp != NULL && (err == -1 || sockerrno != 0)) {
        (void)Ns_PosixSetErrorCode(interp, sockerrno);
    }
    return sockerrno;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_PosixSetErrorCode --
 *
 *      Set the Tcl error code in the interpreter based on the provided
 *      numeric value (POSIX errno) and return the error message.
 *
 * Results:
 *      Error message in form of a string.
 *
 * Side effects:
 *      Sets Tcl error code.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_PosixSetErrorCode(Tcl_Interp *interp, int errorNum) {
    const char *errorMsg;

    NS_NONNULL_ASSERT(interp != NULL);

    errorMsg = Tcl_ErrnoMsg(errorNum);
    Tcl_SetErrorCode(interp, "POSIX",
                     ErrorCodeString(errorNum),
                     Tcl_ErrnoMsg(errorNum),
                     (char *)0L);
    return errorMsg;
}

/*
 *----------------------------------------------------------------------
 *
 * NsSockSetRecvErrorCode --
 *
 *      Set the Tcl error code in the interpreter based on the
 *      information provided by Sock* (actually by recvErrno); the
 *      function works for OpenSSL and plain POSIX style error codes.
 *
 * Results:
 *      Error message in form of a string.
 *
 * Side effects:
 *      Sets Tcl error code.
 *
 *----------------------------------------------------------------------
 */
const char *
NsSockSetRecvErrorCode(const Sock *sockPtr, Tcl_Interp *interp) {

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(sockPtr != NULL);

#ifdef HAVE_OPENSSL_EVP_H

    if (STREQ(sockPtr->drvPtr->protocol, "https")) {
        return Ns_SSLSetErrorCode(interp, sockPtr->recvErrno);
    }
#endif
    return Ns_PosixSetErrorCode(interp, (int)sockPtr->recvErrno);
}

/*
 *----------------------------------------------------------------------
 *
 * ErrorCodeString --
 *
 *      Map errorCode integer to a language independent string.  This
 *      function is practically a copy of the Tcl implementation, except
 *      that the error code is passed-in instead of relying to a global
 *      variable (which is not a good idea in multithreaded programs).
 *
 * Results:
 *      Error code string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char *
ErrorCodeString(int errorCode)
{
    switch (errorCode) {
#if defined(E2BIG) && (!defined(EOVERFLOW) || (E2BIG != EOVERFLOW))
    case E2BIG: return "E2BIG";
#endif
#ifdef EACCES
    case EACCES: return "EACCES";
#endif
#ifdef EADDRINUSE
    case EADDRINUSE: return "EADDRINUSE";
#endif
#ifdef EADDRNOTAVAIL
    case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
#endif
#ifdef EADV
    case EADV: return "EADV";
#endif
#ifdef EAFNOSUPPORT
    case EAFNOSUPPORT: return "EAFNOSUPPORT";
#endif
#ifdef EAGAIN
    case EAGAIN: return "EAGAIN";
#endif
#ifdef EALIGN
    case EALIGN: return "EALIGN";
#endif
#if defined(EALREADY) && (!defined(EBUSY) || (EALREADY != EBUSY))
    case EALREADY: return "EALREADY";
#endif
#ifdef EBADE
    case EBADE: return "EBADE";
#endif
#ifdef EBADF
    case EBADF: return "EBADF";
#endif
#ifdef EBADFD
    case EBADFD: return "EBADFD";
#endif
#ifdef EBADMSG
    case EBADMSG: return "EBADMSG";
#endif
#ifdef ECANCELED
    case ECANCELED: return "ECANCELED";
#endif
#ifdef EBADR
    case EBADR: return "EBADR";
#endif
#ifdef EBADRPC
    case EBADRPC: return "EBADRPC";
#endif
#ifdef EBADRQC
    case EBADRQC: return "EBADRQC";
#endif
#ifdef EBADSLT
    case EBADSLT: return "EBADSLT";
#endif
#ifdef EBFONT
    case EBFONT: return "EBFONT";
#endif
#ifdef EBUSY
    case EBUSY: return "EBUSY";
#endif
#ifdef ECHILD
    case ECHILD: return "ECHILD";
#endif
#ifdef ECHRNG
    case ECHRNG: return "ECHRNG";
#endif
#ifdef ECOMM
    case ECOMM: return "ECOMM";
#endif
#ifdef ECONNABORTED
    case ECONNABORTED: return "ECONNABORTED";
#endif
#ifdef ECONNREFUSED
    case ECONNREFUSED: return "ECONNREFUSED";
#endif
#ifdef ECONNRESET
    case ECONNRESET: return "ECONNRESET";
#endif
#if defined(EDEADLK) && (!defined(EWOULDBLOCK) || (EDEADLK != EWOULDBLOCK))
    case EDEADLK: return "EDEADLK";
#endif
#if defined(EDEADLOCK) && (!defined(EDEADLK) || (EDEADLOCK != EDEADLK))
    case EDEADLOCK: return "EDEADLOCK";
#endif
#ifdef EDESTADDRREQ
    case EDESTADDRREQ: return "EDESTADDRREQ";
#endif
#ifdef EDIRTY
    case EDIRTY: return "EDIRTY";
#endif
#ifdef EDOM
    case EDOM: return "EDOM";
#endif
#ifdef EDOTDOT
    case EDOTDOT: return "EDOTDOT";
#endif
#ifdef EDQUOT
    case EDQUOT: return "EDQUOT";
#endif
#ifdef EDUPPKG
    case EDUPPKG: return "EDUPPKG";
#endif
#ifdef EEXIST
    case EEXIST: return "EEXIST";
#endif
#ifdef EFAULT
    case EFAULT: return "EFAULT";
#endif
#ifdef EFBIG
    case EFBIG: return "EFBIG";
#endif
#ifdef EHOSTDOWN
    case EHOSTDOWN: return "EHOSTDOWN";
#endif
#ifdef EHOSTUNREACH
    case EHOSTUNREACH: return "EHOSTUNREACH";
#endif
#if defined(EIDRM) && (!defined(EINPROGRESS) || (EIDRM != EINPROGRESS))
    case EIDRM: return "EIDRM";
#endif
#ifdef EINIT
    case EINIT: return "EINIT";
#endif
#ifdef EINPROGRESS
    case EINPROGRESS: return "EINPROGRESS";
#endif
#ifdef EINTR
    case EINTR: return "EINTR";
#endif
#ifdef EINVAL
    case EINVAL: return "EINVAL";
#endif
#ifdef EIO
    case EIO: return "EIO";
#endif
#ifdef EISCONN
    case EISCONN: return "EISCONN";
#endif
#ifdef EISDIR
    case EISDIR: return "EISDIR";
#endif
#ifdef EISNAME
    case EISNAM: return "EISNAM";
#endif
#ifdef ELBIN
    case ELBIN: return "ELBIN";
#endif
#ifdef EL2HLT
    case EL2HLT: return "EL2HLT";
#endif
#ifdef EL2NSYNC
    case EL2NSYNC: return "EL2NSYNC";
#endif
#ifdef EL3HLT
    case EL3HLT: return "EL3HLT";
#endif
#ifdef EL3RST
    case EL3RST: return "EL3RST";
#endif
#ifdef ELIBACC
    case ELIBACC: return "ELIBACC";
#endif
#ifdef ELIBBAD
    case ELIBBAD: return "ELIBBAD";
#endif
#ifdef ELIBEXEC
    case ELIBEXEC: return "ELIBEXEC";
#endif
#if defined(ELIBMAX) && (!defined(ECANCELED) || (ELIBMAX != ECANCELED))
    case ELIBMAX: return "ELIBMAX";
#endif
#ifdef ELIBSCN
    case ELIBSCN: return "ELIBSCN";
#endif
#ifdef ELNRNG
    case ELNRNG: return "ELNRNG";
#endif
#if defined(ELOOP) && (!defined(ENOENT) || (ELOOP != ENOENT))
    case ELOOP: return "ELOOP";
#endif
#ifdef EMFILE
    case EMFILE: return "EMFILE";
#endif
#ifdef EMLINK
    case EMLINK: return "EMLINK";
#endif
#ifdef EMSGSIZE
    case EMSGSIZE: return "EMSGSIZE";
#endif
#ifdef EMULTIHOP
    case EMULTIHOP: return "EMULTIHOP";
#endif
#ifdef ENAMETOOLONG
    case ENAMETOOLONG: return "ENAMETOOLONG";
#endif
#ifdef ENAVAIL
    case ENAVAIL: return "ENAVAIL";
#endif
#ifdef ENET
    case ENET: return "ENET";
#endif
#ifdef ENETDOWN
    case ENETDOWN: return "ENETDOWN";
#endif
#ifdef ENETRESET
    case ENETRESET: return "ENETRESET";
#endif
#ifdef ENETUNREACH
    case ENETUNREACH: return "ENETUNREACH";
#endif
#ifdef ENFILE
    case ENFILE: return "ENFILE";
#endif
#ifdef ENOANO
    case ENOANO: return "ENOANO";
#endif
#if defined(ENOBUFS) && (!defined(ENOSR) || (ENOBUFS != ENOSR))
    case ENOBUFS: return "ENOBUFS";
#endif
#ifdef ENOCSI
    case ENOCSI: return "ENOCSI";
#endif
#if defined(ENODATA) && (!defined(ECONNREFUSED) || (ENODATA != ECONNREFUSED))
    case ENODATA: return "ENODATA";
#endif
#ifdef ENODEV
    case ENODEV: return "ENODEV";
#endif
#ifdef ENOENT
    case ENOENT: return "ENOENT";
#endif
#ifdef ENOEXEC
    case ENOEXEC: return "ENOEXEC";
#endif
#ifdef ENOLCK
    case ENOLCK: return "ENOLCK";
#endif
#ifdef ENOLINK
    case ENOLINK: return "ENOLINK";
#endif
#ifdef ENOMEM
    case ENOMEM: return "ENOMEM";
#endif
#ifdef ENOMSG
    case ENOMSG: return "ENOMSG";
#endif
#ifdef ENONET
    case ENONET: return "ENONET";
#endif
#ifdef ENOPKG
    case ENOPKG: return "ENOPKG";
#endif
#ifdef ENOPROTOOPT
    case ENOPROTOOPT: return "ENOPROTOOPT";
#endif
#ifdef ENOSPC
    case ENOSPC: return "ENOSPC";
#endif
#if defined(ENOSR) && (!defined(ENAMETOOLONG) || (ENAMETOOLONG != ENOSR))
    case ENOSR: return "ENOSR";
#endif
#if defined(ENOSTR) && (!defined(ENOTTY) || (ENOTTY != ENOSTR))
    case ENOSTR: return "ENOSTR";
#endif
#ifdef ENOSYM
    case ENOSYM: return "ENOSYM";
#endif
#ifdef ENOSYS
    case ENOSYS: return "ENOSYS";
#endif
#ifdef ENOTBLK
    case ENOTBLK: return "ENOTBLK";
#endif
#ifdef ENOTCONN
    case ENOTCONN: return "ENOTCONN";
#endif
#ifdef ENOTRECOVERABLE
    case ENOTRECOVERABLE: return "ENOTRECOVERABLE";
#endif
#ifdef ENOTDIR
    case ENOTDIR: return "ENOTDIR";
#endif
#if defined(ENOTEMPTY) && (!defined(EEXIST) || (ENOTEMPTY != EEXIST))
    case ENOTEMPTY: return "ENOTEMPTY";
#endif
#ifdef ENOTNAM
    case ENOTNAM: return "ENOTNAM";
#endif
#ifdef ENOTSOCK
    case ENOTSOCK: return "ENOTSOCK";
#endif
#ifdef ENOTSUP
    case ENOTSUP: return "ENOTSUP";
#endif
#ifdef ENOTTY
    case ENOTTY: return "ENOTTY";
#endif
#ifdef ENOTUNIQ
    case ENOTUNIQ: return "ENOTUNIQ";
#endif
#ifdef ENXIO
    case ENXIO: return "ENXIO";
#endif
#if defined(EOPNOTSUPP) &&  (!defined(ENOTSUP) || (ENOTSUP != EOPNOTSUPP))
    case EOPNOTSUPP: return "EOPNOTSUPP";
#endif
#ifdef EOTHER
    case EOTHER: return "EOTHER";
#endif
#if defined(EOVERFLOW) && (!defined(EFBIG) || (EOVERFLOW != EFBIG)) && (!defined(EINVAL) || (EOVERFLOW != EINVAL))
    case EOVERFLOW: return "EOVERFLOW";
#endif
#ifdef EOWNERDEAD
    case EOWNERDEAD: return "EOWNERDEAD";
#endif
#ifdef EPERM
    case EPERM: return "EPERM";
#endif
#if defined(EPFNOSUPPORT) && (!defined(ENOLCK) || (ENOLCK != EPFNOSUPPORT))
    case EPFNOSUPPORT: return "EPFNOSUPPORT";
#endif
#ifdef EPIPE
    case EPIPE: return "EPIPE";
#endif
#ifdef EPROCLIM
    case EPROCLIM: return "EPROCLIM";
#endif
#ifdef EPROCUNAVAIL
    case EPROCUNAVAIL: return "EPROCUNAVAIL";
#endif
#ifdef EPROGMISMATCH
    case EPROGMISMATCH: return "EPROGMISMATCH";
#endif
#ifdef EPROGUNAVAIL
    case EPROGUNAVAIL: return "EPROGUNAVAIL";
#endif
#ifdef EPROTO
    case EPROTO: return "EPROTO";
#endif
#ifdef EPROTONOSUPPORT
    case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
#endif
#ifdef EPROTOTYPE
    case EPROTOTYPE: return "EPROTOTYPE";
#endif
#ifdef ERANGE
    case ERANGE: return "ERANGE";
#endif
#if defined(EREFUSED) && (!defined(ECONNREFUSED) || (EREFUSED != ECONNREFUSED))
    case EREFUSED: return "EREFUSED";
#endif
#ifdef EREMCHG
    case EREMCHG: return "EREMCHG";
#endif
#ifdef EREMDEV
    case EREMDEV: return "EREMDEV";
#endif
#ifdef EREMOTE
    case EREMOTE: return "EREMOTE";
#endif
#ifdef EREMOTEIO
    case EREMOTEIO: return "EREMOTEIO";
#endif
#ifdef EREMOTERELEASE
    case EREMOTERELEASE: return "EREMOTERELEASE";
#endif
#ifdef EROFS
    case EROFS: return "EROFS";
#endif
#ifdef ERPCMISMATCH
    case ERPCMISMATCH: return "ERPCMISMATCH";
#endif
#ifdef ERREMOTE
    case ERREMOTE: return "ERREMOTE";
#endif
#ifdef ESHUTDOWN
    case ESHUTDOWN: return "ESHUTDOWN";
#endif
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT";
#endif
#ifdef ESPIPE
    case ESPIPE: return "ESPIPE";
#endif
#ifdef ESRCH
    case ESRCH: return "ESRCH";
#endif
#ifdef ESRMNT
    case ESRMNT: return "ESRMNT";
#endif
#ifdef ESTALE
    case ESTALE: return "ESTALE";
#endif
#ifdef ESUCCESS
    case ESUCCESS: return "ESUCCESS";
#endif
#if defined(ETIME) && (!defined(ELOOP) || (ETIME != ELOOP))
    case ETIME: return "ETIME";
#endif
#if defined(ETIMEDOUT) && (!defined(ENOSTR) || (ETIMEDOUT != ENOSTR))
    case ETIMEDOUT: return "ETIMEDOUT";
#endif
#ifdef ETOOMANYREFS
    case ETOOMANYREFS: return "ETOOMANYREFS";
#endif
#ifdef ETXTBSY
    case ETXTBSY: return "ETXTBSY";
#endif
#ifdef EUCLEAN
    case EUCLEAN: return "EUCLEAN";
#endif
#ifdef EUNATCH
    case EUNATCH: return "EUNATCH";
#endif
#ifdef EUSERS
    case EUSERS: return "EUSERS";
#endif
#ifdef EVERSION
    case EVERSION: return "EVERSION";
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
    case EWOULDBLOCK: return "EWOULDBLOCK";
#endif
#ifdef EXDEV
    case EXDEV: return "EXDEV";
#endif
#ifdef EXFULL
    case EXFULL: return "EXFULL";
#endif
    }
    return "unknown error";
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
