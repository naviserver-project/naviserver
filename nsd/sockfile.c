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
 * sockfile.c --
 *
 *      Use the native OS sendfile-like implementation to send a file
 *      to an Ns_Sock if possible. Otherwise just use read/write etc.
 *
 *      Functions in this file never block on non-writable socket.
 *      It is the caller responsibility to retry/repeat operation.
 *      This should be done every time the calls return less bytes
 *      written than requested (including zero).
 */

#include "nsd.h"

#ifdef HAVE_SYS_SENDFILE_H
#include <sys/sendfile.h>
#endif

#ifdef _WIN32
#include <io.h>
static ssize_t pread(int fd, char *buf, size_t count, off_t offset);
#endif

/*
 * Local functions defined in this file
 */

static ssize_t ns_sendfile(Ns_Sock *sock, int fd, off_t offset, size_t length)
    NS_GNUC_NONNULL(1);

static ssize_t SendFile(Ns_Sock *sock, int fd, off_t offset, size_t length, unsigned int flags)
    NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetFileVec --
 *
 *      Fill in the fields of an Ns_FileVec structure, handling both
 *      file-based and data-based offsets.
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
Ns_SetFileVec(Ns_FileVec *bufs, int i,  int fd, const void *data,
              off_t offset, size_t length)
{
    bufs[i].fd = fd;
    bufs[i].length = length;
    bufs[i].offset = offset;
    bufs[i].buffer = (char *)data;

    return length;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ResetFileVec --
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
Ns_ResetFileVec(Ns_FileVec *bufs, int nbufs, size_t sent)
{
    int i;

    for (i = 0; i < nbufs && sent > 0u; i++) {
        size_t length = bufs[i].length;

        if (length > 0u) {
            if (sent >= length) {
                sent -= length;
                bufs[i].buffer = NULL;
                bufs[i].offset = 0;
                bufs[i].length = 0;
            } else {
                bufs[i].offset += (off_t)sent;
                bufs[i].length -= sent;
                break;
            }
        }
    }

    return i;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_SockSendFileBufs --
 *
 *      Send a vector of buffers/files on a nonblocking socket.
 *
 * Results:
 *      Number of bytes sent, -1 on error.
 *      Not all data may be sent.
 *
 * Side effects:
 *      May block reading data from disk.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_SockSendFileBufs(Ns_Sock *sock, const Ns_FileVec *bufs, int nbufs, unsigned int flags)
{
    ssize_t       towrite = 0, nwrote = 0;
    struct iovec  sbufs[UIO_MAXIOV], *sbufPtr;
    int           nsbufs = 0, i;

    sbufPtr = sbufs;

    for (i = 0; i < nbufs; i++) {
        size_t   length = bufs[i].length;
        int      fd     = bufs[i].fd;
        off_t    offset = bufs[i].offset;

        if (length == 0) {
            continue;
        }

        towrite += (ssize_t)length;

        if (fd == NS_INVALID_FD) {

            /*
             * Coalesce runs of memory buffers into fixed-sized iovec.
             */
            (void) Ns_SetVec(sbufPtr, nsbufs++, (char*)bufs[i].buffer + offset, length);
        }

        if (   (fd == NS_INVALID_FD && (nsbufs == UIO_MAXIOV || i == nbufs-1))
            || (fd != NS_INVALID_FD && (nsbufs > 0))) {

            /*
             * Flush pending memory buffers.
             */

            ssize_t sent = NsDriverSend((Sock *)sock, sbufPtr, nsbufs, 0u);
            if (sent == -1) {
                nwrote = -1;
                break;
            }
            if (sent > 0) {
                nwrote += sent;
            }
            if (sent < towrite) {
                break;
            }
            nsbufs = 0; /* All pending buffers flushed */
        }

        /*
         * Send a single file range.
         */

        if (fd != NS_INVALID_FD) {
            ssize_t sent = SendFile(sock, fd, offset, length, flags);
            if (sent == -1) {
                nwrote = -1;
                break;
            }
            if (sent > 0) {
                nwrote += sent;
            }
            if (sent < towrite) {
                break;
            }
            towrite = 0;
        }
    }

    return nwrote;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_SockCork --
 *
 *      Turn TCP_CORK state on or off, if supported by the OS. The
 *      function tracks the cork state in the socket structure to be
 *      able to handle nesting calls.
 *
 * Results:
 *      NS_TRUE or NS_FALSE.
 *
 * Side effects:
 *      Switch TCP send state, potentially update sockPtr->flags
 *
 *----------------------------------------------------------------------
 */
bool
Ns_SockCork(const Ns_Sock *sock, bool cork)
{
    bool success = NS_FALSE;
#if defined(TCP_CORK) || defined(UDP_CORK)
    Sock *sockPtr = (Sock *)sock;
    int corkInt = (int)cork;

    assert(sock != NULL);

    /* fprintf(stderr, "### Ns_SockCork sock %d %d\n", sockPtr->sock, cork); */

    if (cork && (sockPtr->flags & NS_CONN_SOCK_CORKED) != 0u) {
        /*
         * Don't cork an already corked connection.
         */
    } else if (!cork && (sockPtr->flags & NS_CONN_SOCK_CORKED) == 0u) {
        /*
         * Don't uncork an already uncorked connection.
         */
        Ns_Log(Error, "socket: trying to uncork an uncorked socket %d",
               sockPtr->sock);
    } else {
        /*
         * The cork state changes, try to alter the socket options, unless the
         * socket is already closed (don't complain in such cases to the
         * system log file).
         */
# if defined(TCP_CORK)
        if ((sockPtr->drvPtr->opts & NS_DRIVER_UDP) == 0) {
            if ((sockPtr->sock != NS_INVALID_SOCKET)
                && (setsockopt(sockPtr->sock, IPPROTO_TCP, TCP_CORK, &corkInt, sizeof(corkInt)) == -1)
                ) {
                Ns_Log(Error, "socket(%d): setsockopt(TCP_CORK) %d: %s",
                       sockPtr->sock, corkInt, ns_sockstrerror(ns_sockerrno));
            } else {
                success = NS_TRUE;
            }
        }
# endif
# if defined(UDP_CORK)
        if ((sockPtr->drvPtr->opts & NS_DRIVER_UDP) != 0) {
            if ((sockPtr->sock != NS_INVALID_SOCKET)
                && setsockopt(sockPtr->sock, IPPROTO_UDP, UDP_CORK, &corkInt, sizeof(corkInt)) == -1) {

                Ns_Log(Error, "socket(%d): setsockopt(UDP_CORK) %d: %s",
                       sockPtr->sock, corkInt, ns_sockstrerror(ns_sockerrno));
            } else {
                success = NS_TRUE;
            }
        }
# endif
        if (success) {
            /*
             * On success, update the corked flag.
             */
            if (cork) {
                sockPtr->flags |= NS_CONN_SOCK_CORKED;
            } else {
                sockPtr->flags &= ~NS_CONN_SOCK_CORKED;
            }
        }
    }
#else
    (void)cork;
    (void)sock;
#endif
    return success;
}



/*
 *----------------------------------------------------------------------
 *
 * SendFile --
 *
 *      Custom wrapper for sendfile() that handles the case
 *      where it is not defined for the platform.
 *
 * Results:
 *      Number of bytes sent, -1 on error.
 *      Not all data may be sent.
 *
 * Side effects:
 *      May block reading data from disk.
 *
 *----------------------------------------------------------------------
 */

/* Decide once at compile-time */
#if defined(HAVE_LINUX_SENDFILE) || defined(HAVE_BSD_SENDFILE)
# define NS_HAVE_NATIVE_SENDFILE 1
#else
# define NS_HAVE_NATIVE_SENDFILE 0
#endif

static ssize_t
SendFile(Ns_Sock *sock, int fd, off_t offset, size_t length, unsigned int flags)
{
    ssize_t sent;

    NS_NONNULL_ASSERT(sock != NULL);

    assert(fd != NS_INVALID_FD);
    assert(offset >= 0);

    /*
     * Only, when the current driver supports sendfile(), try to use the
     * native implementation. When we are using e.g. HTTPS, using sendfile
     * does not work, since it would write plain data to the encrypted
     * channel. The sendfile emulation ns_sendfile() uses always the right
     * driver I/O.
     */

#if NS_HAVE_NATIVE_SENDFILE

    if ((flags & NS_DRIVER_CAN_USE_SENDFILE) == 0u) {
        sent = ns_sendfile(sock, fd, offset, length);

    } else {
# if defined(HAVE_LINUX_SENDFILE)
        sent = sendfile(sock->sock, fd, &offset, length);
        if (sent == -1) {
            if (errno == EINTR || NS_ERRNO_WOULDBLOCK(errno)) {
                sent = 0;
            } else if (errno == EINVAL || errno == ENOSYS) {
                sent = ns_sendfile(sock, fd, offset, length);
            }
        }
# elif defined(HAVE_BSD_SENDFILE)
        int   rc, opt_flags = 0;
        off_t sbytes = 0;

        rc = sendfile(fd, sock->sock, offset, length, NULL, &sbytes, opt_flags);
        if (rc == 0 || errno == EINTR || NS_ERRNO_WOULDBLOCK(errno)) {
            sent = sbytes;
        } else if (errno == EOPNOTSUPP) {
            sent = ns_sendfile(sock, fd, offset, length);
        } else {
            sent = 0;
        }
# endif
    }

#else  /* ! NS_HAVE_NATIVE_SENDFILE */
    /*
     * No native sendfile on this platform → always use fallback. The provided
     * "flags" are ignored.
     */
    (void)flags;
    sent = ns_sendfile(sock, fd, offset, length);

#endif /* NS_HAVE_NATIVE_SENDFILE */

  return sent;
}



/*
 *----------------------------------------------------------------------
 *
 * ns_sendfile --
 *
 *      Emulates the operation of kernel-based sendfile().
 *
 * Results:
 *      Number of bytes sent, -1 on error
 *      Not all data may be sent.
 *
 * Side effects:
 *      May block reading data from disk.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
ns_sendfile(Ns_Sock *sock, int fd, off_t offset, size_t length)
{
    char               buf[16384];
    struct iovec       iov;
    ssize_t            nwrote = 0, toread = (ssize_t)length;
    bool               decork;
    Sock              *sockPtr;
    Ns_DriverSendProc *sendProc;

    NS_NONNULL_ASSERT(sock != NULL);

    sockPtr = (Sock *)sock;

    NS_NONNULL_ASSERT(sockPtr->drvPtr != NULL);

    sendProc = sockPtr->drvPtr->sendProc;

    if (sendProc == NULL) {
        Ns_Log(Warning, "no sendProc registered for driver %s",
               sockPtr->drvPtr->threadName);
        return -1;
    }

    decork = Ns_SockCork(sock, NS_TRUE);

    while (toread > 0) {
        ssize_t nread, sent;

        nread = pread(fd, buf, MIN((size_t)toread, sizeof(buf)), offset);

        if (nread <= 0) {
            nwrote = -1;
            break;
        }

        (void) Ns_SetVec(&iov, 0, buf, (size_t)nread);
        sent = (*sendProc)(sock, &iov, 1, 0u);

        if (sent == -1) {
            nwrote = -1;
            break;
        }
        if (sent > 0) {
            nwrote += sent;
        }
        if (sent != nread) {
            break;
        }
        toread -= nread;
        offset += (off_t)nread;
    }

    if (decork) {
        (void) Ns_SockCork(sock, NS_FALSE);
    }

    return nwrote;
}



/*
 *----------------------------------------------------------------------
 *
 * pread --
 *
 *      The pread() syscall emulation for Windows.
 *
 * Results:
 *      Number of bytes read, -1 on error
 *
 * Side effects:
 *      Advancing the file pointer.
 *
 *----------------------------------------------------------------------
 */
#ifdef _WIN32
static ssize_t
pread(int fd, char *buf, size_t count, off_t offset)
{
    HANDLE   fh = (HANDLE)_get_osfhandle(fd);
    ssize_t  result;

    if (fh == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        result = -1;
    } else {
        DWORD      ret, c = (DWORD)count;
        OVERLAPPED overlapped = { 0u };

        overlapped.Offset = (DWORD)offset;
        overlapped.OffsetHigh = (DWORD)(((uint64_t)offset) >> 32);

        if (ReadFile(fh, buf, c, &ret, &overlapped) == FALSE) {
            result = -1;
        } else {
            result = (ssize_t)ret;
        }
    }

    return result;
}
#endif /* _WIN32 */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
