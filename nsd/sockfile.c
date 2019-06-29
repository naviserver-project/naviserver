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
ssize_t pread(int fd, char *buf, size_t count, off_t offset);
#endif

/*
 * Local functions defined in this file
 */

static ssize_t _SendFile(Ns_Sock *sock, int fd, off_t offset, size_t length);
static ssize_t SendFile(Ns_Sock *sock, int fd, off_t offset, size_t length);


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

    if (fd != NS_INVALID_FD) {
        bufs[i].offset = offset;
    } else {
        bufs[i].offset = ((off_t)(intptr_t) data) + offset;
    }
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
    int          i;

    for (i = 0; i < nbufs && sent > 0u; i++) {
        int    fd     = bufs[i].fd;
        size_t length = bufs[i].length;
        off_t  offset = bufs[i].offset;

        if (length > 0u) {
            if (sent >= length) {
                sent -= length;
                (void) Ns_SetFileVec(bufs, i, fd, NULL, 0, 0u);
            } else {
                (void) Ns_SetFileVec(bufs, i, fd, NULL,
                                     offset + (off_t)sent, length - sent);
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
 *      Send a vector of buffers/files on a non-blocking socket.
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
Ns_SockSendFileBufs(Ns_Sock *sock, const Ns_FileVec *bufs, int nbufs)
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

            (void) Ns_SetVec(sbufPtr, nsbufs++, INT2PTR(offset), length);
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
            ssize_t sent = SendFile(sock, fd, offset, length);
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
 *      NS_TRUE or NS_FALSE
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
         * error.log).
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
 *      where the source file system does not support it,
 *      or when it is not defined for the platform.
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

static ssize_t
SendFile(Ns_Sock *sock, int fd, off_t offset, size_t length)
{
    ssize_t sent;
    Sock *sockPtr = (Sock *)sock;

    NS_NONNULL_ASSERT(sock != NULL);

    assert(fd != NS_INVALID_FD);
    assert(offset >= 0);

    /*
     * Only, when the current driver supports sendfile(), try to use the
     * native implementation. When we are using e.g. HTTPS, using sendfile
     * does not work, since it would write plain data to the encrypted
     * channel. The sendfile emulation _SendFile() uses always the right
     * driver I/O.
     */
    if (sockPtr->drvPtr->sendFileProc == NULL) {
        sent = _SendFile(sock, fd, offset, length);
    } else {
#if defined(HAVE_LINUX_SENDFILE)
        sent = sendfile(sock->sock, fd, &offset, length);
        if (sent == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                sent = 0;
            } else if (errno == EINVAL || errno == ENOSYS) {
                sent = _SendFile(sock, fd, offset, length);
            }
        }
#elif defined(HAVE_BSD_SENDFILE)
        {
            int rc, flags = 0;
            off_t sbytes = 0;

            rc = sendfile(fd, sock->sock, offset, length, NULL, &sbytes, flags);
            if (rc == 0 || errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                sent = sbytes;
            } else if (errno == EOPNOTSUPP) {
                sent = _SendFile(sock, fd, offset, length);
            }
        }
#else
        sent = _SendFile(sock, fd, offset, length);
#endif
    }
    return sent;
}



/*
 *----------------------------------------------------------------------
 *
 * _SendFile
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
_SendFile(Ns_Sock *sock, int fd, off_t offset, size_t length)
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
        sent = (*sendProc)(sock, &iov, 1, NULL, 0);

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
ssize_t pread(int fd, char *buf, size_t count, off_t offset)
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
