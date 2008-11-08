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
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");



/*
 * Local functions defined in this file
 */

static ssize_t SendFd(Ns_Sock *sock, int fd, off_t offset, size_t length,
                      Ns_Time *timeoutPtr, int flags,
                      Ns_DriverSendProc *sendProc);

static Ns_DriverSendProc SendBufs;



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
Ns_SetFileVec(Ns_FileVec *bufs, int i,  int fd, CONST void *data,
              off_t offset, size_t length)
{
    bufs[i].fd = fd;
    bufs[i].length = length;

    if (fd > -1) {
        bufs[i].offset = offset;
    } else {
        bufs[i].offset = ((intptr_t) data) + offset;
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
    int          i, fd;
    off_t        offset;
    size_t       length;

    for (i = 0; i < nbufs && sent > 0; i++) {

        fd     = bufs[i].fd;
        offset = bufs[i].offset;
        length = bufs[i].length;

        if (length > 0) {
            if (sent >= length) {
                sent -= length;
                Ns_SetFileVec(bufs, i, fd, NULL, 0, 0);
            } else {
                Ns_SetFileVec(bufs, i, fd, NULL, offset + sent, length - sent);
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
 *      Not all data may be sent.
 *
 * Results:
 *      Number of bytes sent, -1 on error or timeout.
 *
 * Side effects:
 *      May block reading data from disk.
 *
 *      May wait for given timeout if first attempt to write to socket
 *      would block.
 *
 *----------------------------------------------------------------------
 */

#ifdef HAVE_SYS_SENDFILE_H  /* Linux */

#include <sys/sendfile.h>

static ssize_t
Sendfile(Ns_Sock *sock, int fd, off_t offset, size_t tosend, Ns_Time *timeoutPtr);

ssize_t
Ns_SockSendFileBufs(Ns_Sock *sock, CONST Ns_FileVec *bufs, int nbufs,
                    Ns_Time *timeoutPtr, int flags)
{
    off_t         offset;
    size_t        length;
    ssize_t       sent, towrite, nwrote;
    struct iovec  sbufs[UIO_MAXIOV];
    int           nsbufs = 0, i, fd;

    towrite = nwrote = 0;
    sent = -1;

    for (i = 0; i < nbufs; i++) {

        offset = bufs[i].offset;
        length = bufs[i].length;
        fd     = bufs[i].fd;

        if (length <= 0) {
            continue;
        }

        towrite += length;

        if (fd < 0) {
            /*
             * Coalesce runs of memory bufs into fixed-sized iovec.
             */
            Ns_SetVec(sbufs, nsbufs++, (void *) (intptr_t) offset, length);
        }

        /* Flush pending memory bufs. */

        if ((fd < 0
             && (nsbufs == UIO_MAXIOV || i == nbufs -1))
            || (fd >= 0
                && nsbufs > 0)) {

            sent = Ns_SockSendBufs(sock->sock, sbufs, nsbufs, timeoutPtr, 0);

            nsbufs = 0;
            if (sent > 0) {
                nwrote += sent;
            }
            if (sent < towrite) {
                break;
            }
        }

        /* Send a single file range. */

        if (fd >= 0) {
            sent = Sendfile(sock, fd, offset, length, timeoutPtr);
            if (sent > 0) {
                nwrote += sent;
            }
            if (sent < towrite) {
                break;
            }
            towrite = 0;
        }
    }

    return nwrote ? nwrote : sent;
}

static ssize_t
Sendfile(Ns_Sock *sock, int fd, off_t offset, size_t tosend, Ns_Time *timeoutPtr)
{
    ssize_t sent;

    sent = sendfile(sock->sock, fd, &offset, tosend);

    if (sent == -1) {
        switch (errno) {

        case EAGAIN:
            if (Ns_SockTimedWait(sock->sock, NS_SOCK_WRITE, timeoutPtr) == NS_OK) {
                sent = sendfile(sock->sock, fd, &offset, tosend);
            }
            break;

        case EINVAL:
        case ENOSYS:
            /* File system does not support sendfile? */
            sent = SendFd(sock, fd, offset, tosend, timeoutPtr, 0,
                          SendBufs);
            break;
        }
    }

    return sent;
}

#else /* Default implementation */

ssize_t
Ns_SockSendFileBufs(Ns_Sock *sock, CONST Ns_FileVec *bufs, int nbufs,
                    Ns_Time *timeoutPtr, int flags)
{
    return NsSockSendFileBufsIndirect(sock, bufs, nbufs, timeoutPtr, flags,
                                      SendBufs);
}

#endif


/*
 *----------------------------------------------------------------------
 *
 * NsSockSendFileBufsIndirect --
 *
 *      Send a vector of buffers/files on a non-blocking socket using
 *      the given callback for socekt IO.  Not all data may be sent.
 *
 * Results:
 *      Number of bytes sent, -1 on error or timeout.
 *
 * Side effects:
 *      May block reading data from disk.
 *
 *      May wait for given timeout if first attempt to write to socket
 *      would block.
 *
 *----------------------------------------------------------------------
 */

ssize_t
NsSockSendFileBufsIndirect(Ns_Sock *sock, CONST Ns_FileVec *bufs, int nbufs,
                           Ns_Time *timeoutPtr, int flags,
                           Ns_DriverSendProc *sendProc)
{
    off_t         offset;
    size_t        tosend;
    ssize_t       sent, nwrote;
    struct iovec  iov;
    int           i, fd;

    nwrote = 0;
    sent = -1;

    for (i = 0; i < nbufs; i++) {

        offset = bufs[i].offset;
        tosend = bufs[i].length;
        fd     = bufs[i].fd;

        if (tosend > 0) {
            if (fd < 0) {
                Ns_SetVec(&iov, 0, (void *) (intptr_t) offset, tosend);
                sent = (*sendProc)(sock, &iov, 1, timeoutPtr, flags);
            } else {
                sent = SendFd(sock, fd, offset, tosend,
                              timeoutPtr, flags, sendProc);
            }
            if (sent > 0) {
                nwrote += sent;
            }
            if (sent != tosend) {
                break;
            }
        }
    }

    return nwrote ? nwrote : sent;
}


/*
 *----------------------------------------------------------------------
 *
 * SendFd --
 *
 *      Send the given range of bytes from fd to sock using the given
 *      callback. Not all data may be sent.
 *
 * Results:
 *      Number of bytes sent, -1 on error or timeout.
 *
 * Side effects:
 *      May block reading data from disk.
 *
 *      May wait for given timeout if first attempt to write to socket
 *      would block.
 *
 *----------------------------------------------------------------------
 */

static ssize_t
SendFd(Ns_Sock *sock, int fd, off_t offset, size_t length,
       Ns_Time *timeoutPtr, int flags,
       Ns_DriverSendProc *sendPtr)
{
    char          buf[4096];
    struct iovec  iov;
    ssize_t       nwrote, sent, nread;
    size_t        toread;

    toread = length;
    nwrote = 0;

    while (toread > 0) {

        nread = pread(fd, buf, MIN(toread, sizeof(buf)), offset);
        if (nread <= 0) {
            break;
        }
        toread -= nread;
        offset += nread;

        Ns_SetVec(&iov, 0, buf, nread);
        sent = (*sendPtr)(sock, &iov, 1, timeoutPtr, flags);
        if (sent > 0) {
            nwrote += sent;
        }

        if (sent != nread) {
            break;
        }
    }

    if (nwrote > 0) {
        return nwrote;
    } else {
        return -1;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * SendBufs --
 *
 *      Implements the driver send interface to send bufs directly
 *      to a OS socket.
 *
 * Results:
 *      Number of bytes sent, -1 on error or timeout.
 *
 * Side effects:
 *      See Ns_SockSendBufs.
 *
 *----------------------------------------------------------------------
 */

ssize_t
SendBufs(Ns_Sock *sock, struct iovec *bufs, int nbufs,
         Ns_Time *timeoutPtr, int flags)
{
    return Ns_SockSendBufs(sock->sock, bufs, nbufs, timeoutPtr, flags);
}
