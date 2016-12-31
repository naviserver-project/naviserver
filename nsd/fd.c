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
 * fd.c --
 *
 *      Manipulate file descriptors of open files.
 */

#include "nsd.h"

#ifdef _WIN32
# include <share.h>
#else
# ifdef USE_DUPHIGH
static int dupHigh = 0;
# endif
#endif


/*
 * The following structure maitains an open temp fd.
 */

typedef struct Tmp {
    struct Tmp *nextPtr;
    int fd;
} Tmp;

static Tmp      *firstTmpPtr = NULL;
static Ns_Mutex  lock;
static int       devNull;

/*
 * The following constants are defined for this file
 */

#ifndef F_CLOEXEC
# define F_CLOEXEC 1
#endif



/*
 *----------------------------------------------------------------------
 *
 * NsInitFd --
 *
 *      Initialize the fd API's.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will open a shared fd to /dev/null and ensure stdin, stdout,
 *      and stderr are open on something.
 *
 *----------------------------------------------------------------------
 */

void
NsInitFd(void)
{
#ifndef _WIN32
    struct rlimit  rl;
#endif
    int fd;

    /*
     * Ensure fd 0, 1, and 2 are open on at least /dev/null.
     */

    fd = ns_open(DEVNULL, O_RDONLY, 0);
    if (fd > 0) {
        (void) ns_close(fd);
    }
    fd = ns_open(DEVNULL, O_WRONLY, 0);
    if (fd > 0 && fd != 1) {
        (void) ns_close(fd);
    }
    fd = ns_open(DEVNULL, O_WRONLY, 0);
    if (fd > 0 && fd != 2) {
        (void) ns_close(fd);
    }

#ifndef _WIN32
    /*
     * The server now uses poll() but Tcl and other components may
     * still use select() which will likely break when fd's exceed
     * FD_SETSIZE.  We now allow setting the fd limit above FD_SETSIZE,
     * but do so at your own risk.
     */

    if (getrlimit(RLIMIT_NOFILE, &rl)) {
        Ns_Log(Warning, "fd: getrlimit(RLIMIT_NOFILE) failed: %s",
               strerror(errno));
    } else {
        if (rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &rl)) {
                if (rl.rlim_max != RLIM_INFINITY) {
                    Ns_Log(Warning, "fd: setrlimit(RLIMIT_NOFILE, %u) failed: %s",
                           (unsigned int) rl.rlim_max, strerror(errno));
                } else {
#ifdef __APPLE__
#include <sys/sysctl.h>
                    size_t len = sizeof(int);
                    rlim_t maxf;

                    if (!sysctlbyname("kern.maxfiles", &maxf, &len, NULL, 0)) {
                        rl.rlim_cur = rl.rlim_max = maxf > OPEN_MAX ? OPEN_MAX : maxf;
                    } else {
                        rl.rlim_cur = rl.rlim_max = OPEN_MAX;
                    }
#elif defined(OPEN_MAX)
                    rl.rlim_cur = rl.rlim_max = OPEN_MAX;
#else
                    rl.rlim_cur = rl.rlim_max = 256;
#endif /* __APPLE__ */
                    if (setrlimit(RLIMIT_NOFILE, &rl)) {
                        Ns_Log(Warning, "fd: setrlimit(RLIMIT_NOFILE, %u) failed: %s",
                               (unsigned int) rl.rlim_max, strerror(errno));
                    }
                }
            }
        }
#ifdef USE_DUPHIGH
        if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur > 256) {
            dupHigh = 1;
        }
#endif /* USE_DUPHIGH */
    }
#endif /* _WIN32 */

    /*
     * Open a fd on /dev/null which can be later re-used.
     */

    devNull = ns_open(DEVNULL, O_RDWR, 0);
    if (devNull < 0) {
        Ns_Fatal("fd: ns_open(%s) failed: %s", DEVNULL, strerror(errno));
    }
    (void) Ns_DupHigh(&devNull);
    (void) Ns_CloseOnExec(devNull);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CloseOnExec --
 *
 *      Set the close-on-exec flag for a file descriptor
 *
 * Results:
 *      Return NS_OK on success or NS_ERROR on failure
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_CloseOnExec(int fd)
{
#ifdef _WIN32
    intptr_t hh = _get_osfhandle(fd);

    if (hh != (intptr_t)INVALID_HANDLE_VALUE) {
        SetHandleInformation((HANDLE)hh, HANDLE_FLAG_INHERIT, (DWORD)0u);
    }
    return NS_OK;
#else
    int           i;
    Ns_ReturnCode status = NS_ERROR;

    i = fcntl(fd, F_GETFD);
    if (i != -1) {
        i |= F_CLOEXEC;
        (void) fcntl(fd, F_SETFD, i);
        status = NS_OK;
    }
    return status;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_NoCloseOnExec --
 *
 *      Clear the close-on-exec flag for a file descriptor
 *
 * Results:
 *      Return NS_OK on success or NS_ERROR on failure
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_NoCloseOnExec(int fd)
{
#ifdef _WIN32
    intptr_t hh = _get_osfhandle(fd);

    if (hh != (intptr_t)INVALID_HANDLE_VALUE) {
        SetHandleInformation((HANDLE)hh, HANDLE_FLAG_INHERIT, (DWORD)1u);
    }
    return NS_OK;
#else
    int           i;
    Ns_ReturnCode status = NS_ERROR;

    i = fcntl(fd, F_GETFD);
    if (i != -1) {
        i &= ~F_CLOEXEC;
        (void) fcntl(fd, F_SETFD, i);
        status = NS_OK;
    }
    return status;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DupHigh --
 *
 *      Dup a file descriptor to be 256 or higher
 *
 * Results:
 *      Returns new file discriptor.
 *
 * Side effects:
 *      Original file descriptor is closed.
 *
 *----------------------------------------------------------------------
 */

int
Ns_DupHigh(int *fdPtr)
{
#ifdef USE_DUPHIGH
    assert(fdPtr != NULL);
    
    if (dupHigh != 0) {
        int  nfd, ofd, flags;

        ofd = *fdPtr;
        if ((flags = fcntl(ofd, F_GETFD)) < 0) {
            Ns_Log(Warning, "fd: duphigh failed: fcntl(%d, F_GETFD): '%s'",
                   ofd, strerror(errno));
        } else if ((nfd = fcntl(ofd, F_DUPFD, 256)) < 0) {
            Ns_Log(Warning, "fd: duphigh failed: fcntl(%d, F_DUPFD, 256): '%s'",
                   ofd, strerror(errno));
        } else if (fcntl(nfd, F_SETFD, flags) < 0) {
            Ns_Log(Warning, "fd: duphigh failed: fcntl(%d, F_SETFD, %d): '%s'",
                   nfd, flags, strerror(errno));
            (void) ns_close(nfd);
        } else {
            ns_close(ofd);
            *fdPtr = nfd;
        }
    }
#endif
    return *fdPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetTemp --
 *
 *      Pop or allocate a temp file.  Temp files are immediately
 *      removed on Unix and marked non-shared and delete on close
 *      on NT to avoid snooping of data being sent to the CGI.
 *
 * Results:
 *      Open file descriptor.
 *
 * Side effects:
 *      File may be opened.
 *
 *----------------------------------------------------------------------
 */

int
Ns_GetTemp(void)
{
    Tmp *tmpPtr;
    Ns_Time now;
    Ns_DString ds;
    char *path, buf[64];
    int fd, flags, trys;

    Ns_MutexLock(&lock);
    tmpPtr = firstTmpPtr;
    if (tmpPtr != NULL) {
        firstTmpPtr = tmpPtr->nextPtr;
    }
    Ns_MutexUnlock(&lock);
    if (tmpPtr != NULL) {
        fd = tmpPtr->fd;
        ns_free(tmpPtr);
        return fd;
    }

    Ns_DStringInit(&ds);
    flags = O_RDWR|O_CREAT|O_TRUNC|O_EXCL;
    trys = 0;
#ifdef _WIN32
    flags |= _O_SHORT_LIVED|_O_NOINHERIT|_O_TEMPORARY|_O_BINARY;
#endif

    do {
        Ns_GetTime(&now);
        snprintf(buf, sizeof(buf), "nstmp.%" PRId64 ".%06ld", (int64_t)now.sec, now.usec);
        path = Ns_MakePath(&ds, P_tmpdir, buf, (char *)0);
#ifdef _WIN32
        fd = _sopen(path, flags, _SH_DENYRW, _S_IREAD|_S_IWRITE);
#else
        fd = ns_open(path, flags, 0600);
#endif
    } while (fd < 0 && trys++ < 10 && errno == EEXIST);

    if (fd < 0) {
        Ns_Log(Error, "tmp: could not open temp file %s: %s",
               path, strerror(errno));
#ifndef _WIN32
    } else {
        (void) Ns_DupHigh(&fd);
        (void) Ns_CloseOnExec(fd);
        if (unlink(path) != 0) {
            Ns_Log(Warning, "tmp: unlink(%s) failed: %s", path, strerror(errno));
        }
#endif
    }
    Ns_DStringFree(&ds);

    return fd;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ReleaseTemp --
 *
 *      Return a temp file to the pool.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      File may be closed on error.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ReleaseTemp(int fd)
{
    Tmp *tmpPtr;

    if (ns_lseek(fd, 0, SEEK_SET) != 0 || ftruncate(fd, 0) != 0) {
        (void) ns_close(fd);
    } else {
        tmpPtr = ns_malloc(sizeof(Tmp));
        tmpPtr->fd = fd;
        Ns_MutexLock(&lock);
        tmpPtr->nextPtr = firstTmpPtr;
        firstTmpPtr = tmpPtr;
        Ns_MutexUnlock(&lock);
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
