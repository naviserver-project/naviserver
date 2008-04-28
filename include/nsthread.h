/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
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
 * nsthread.h --
 *
 *  Core threading and system headers.
 *
 *  $Header$
 */

#ifndef NSTHREAD_H
#define NSTHREAD_H

#ifdef HAVE_CONFIG_H
#include "nsconfig.h"
#endif

#include <nscheck.h>

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _REENTRANT
#define _REENTRANT
#endif

#ifdef _WIN32
#define NS_EXPORT                   __declspec(dllexport)
#define NS_IMPORT                   __declspec(dllimport)

#if defined(__GNUC__) || defined(__MINGW32__)
#define NSTHREAD_EXPORTS
#endif

#if defined(NSTHREAD_EXPORTS)
#define NS_STORAGE_CLASS            NS_EXPORT
#else
#define NS_STORAGE_CLASS            NS_IMPORT
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT                0x0400
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/timeb.h>
#include <sys/types.h>
#include <io.h>
#include <process.h>
#include <direct.h>
#include <fcntl.h>

#define STDOUT_FILENO               1
#define STDERR_FILENO               2

#define EINPROGRESS                 WSAEINPROGRESS
#define EWOULDBLOCK                 WSAEWOULDBLOCK

/*
 * Windows does not have this declared. The defined value has no meaning.
 * It just have to exist and it needs to be accepted by any strerror() call.
 */

#ifndef ETIMEDOUT
#define ETIMEDOUT                   1
#endif

#define NS_SIGHUP                   1
#define NS_SIGINT                   2
#define NS_SIGQUIT                  3
#define NS_SIGPIPE                  13
#define NS_SIGTERM                  15

#define DEVNULL	                    "nul:"

#define strcasecmp                  _stricmp
#define strncasecmp                 _strnicmp
#define vsnprintf                   _vsnprintf
#define snprintf                    _snprintf
#define mkdir(d,m)                  _mkdir((d))
#define sleep(n)                    (Sleep((n)*1000))
#define ftruncate(f,s)              chsize((f),(s))

/*
 * Under MinGW we use config.h, for MSVC we pre-define environment here
 */

#ifndef HAVE_CONFIG_H
#define F_OK                        0
#define W_OK                        2
#define R_OK                        4
#define X_OK                        R_OK
#define atoll                       _atoi64
#define va_copy(dst,src)            ((void)((dst) = (src)))
#define USE_TCLVFS                  1
#define USE_THREAD_ALLOC            1
#define VERSION                     NS_PATCH_LEVEL
#define _LARGEFILE64_SOURCE         1
#define _THREAD_SAFE                1
#define TCL_THREADS                 1
#define HAVE_GETADDRINFO            1
#define HAVE_GETNAMEINFO            1
#define HAVE_STRUCT_STAT64          1
#define PACKAGE                     "naviserver"
#define PACKAGE_NAME                "NaviServer"
#define PACKAGE_STRING              PACKAGE_NAME " " NS_PATCH_LEVEL
#define PACKAGE_TARNAME             PACKAGE
#define PACKAGE_VERSION             NS_VERSION
#define PACKAGE_BUGREPORT           "naviserver-devel@lists.sourceforge.net"

typedef unsigned long long int uint64_t;
typedef unsigned long int uintmax_t;
typedef long int intmax_t;
#endif

/*
 * The following structure defines an I/O scatter/gather buffer for WIN32.
 */

struct iovec {
    u_long      iov_len;     /* the length of the buffer */
    char FAR *  iov_base;    /* the pointer to the buffer */
};

/*
 * The following is for supporting our own poll() emulation.
 */

#define POLLIN                      0x0001
#define POLLPRI                     0x0002
#define POLLOUT                     0x0004
#define POLLERR                     0x0008
#define POLLHUP                     0x0010

struct pollfd {
    int            fd;
    unsigned short events;
    unsigned short revents;
};

/*
 * The following is for supporting opendir/readdir functionality
 */

struct dirent {
    char *d_name;
};

typedef struct DIR_ *DIR;

#else

#include <sys/types.h>
#include <dirent.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/uio.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <grp.h>
#include <pthread.h>
#include <sys/mman.h>
#include <poll.h>

#if defined(HAVE_SYS_UIO_H)
#include <sys/uio.h>
#elif defined(HAVE_UIO_H)
#include <uio.h>
#endif

#ifdef __linux
#include <sys/prctl.h>
#endif

#ifdef __hp
#define seteuid(i) setresuid((-1),(i),(-1))
#endif

#ifdef __sun
#include <sys/filio.h>
#include <sys/systeminfo.h>
#define gethostname(b,s) (!(sysinfo(SI_HOSTNAME, b, s) > 0))
#endif

#ifdef __unixware
#include <sys/filio.h>
#endif

#if defined(__sgi) && !defined(_SGI_MP_SOURCE)
#define _SGI_MP_SOURCE
#endif

#if defined(__sun) && !defined(_POSIX_PTHREAD_SEMANTICS)
#define _POSIX_PTHREAD_SEMANTICS
#endif

#ifdef __OpenBSD__
#ifndef ENOTSUP
/*
 * Workaround until we have ENOTSUP in errno.h
 */
#define ENOTSUP                     EOPNOTSUPP
#endif
#endif

#define O_TEXT                      0
#define O_BINARY                    0
#define SOCKET                      int
#define INVALID_SOCKET              (-1)
#define SOCKET_ERROR                (-1)

#define NS_SIGHUP                   SIGHUP
#define NS_SIGINT                   SIGINT
#define NS_SIGQUIT                  SIGQUIT
#define NS_SIGPIPE                  SIGPIPE
#define NS_SIGTERM                  SIGTERM

#define DEVNULL	                    "/dev/null"

#define NS_EXPORT
#define NS_IMPORT
#define NS_STORAGE_CLASS

#endif /* _WIN32 */

#include "tcl.h"
#include <limits.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/stat.h>

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifndef S_ISREG
#define S_ISREG(m)                  ((m)&_S_IFREG)
#endif

#ifndef S_ISDIR
#define S_ISDIR(m)                  ((m)&_S_IFDIR)
#endif

#ifndef O_LARGEFILE
#define O_LARGEFILE                 0
#endif

#ifndef F_CLOEXEC
#define F_CLOEXEC                   1
#endif

#ifndef __linux
#ifdef FD_SETSIZE
#undef FD_SETSIZE
#endif
#define FD_SETSIZE                  1024
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* Some very old gcc versions do not have it defined, instead of messing with confiture here it
 * is a simple define for such cases
 */

#ifndef LLONG_MAX
#define LLONG_HALF                  (1LL << (sizeof (long long int) * CHAR_BIT - 2))
#define LLONG_MAX                   (LLONG_HALF - 1 + LLONG_HALF)
#define LLONG_MIN                   (-LLONG_MAX-1)
#endif

/*
 * This baroque pre-processor fiddling should be eventually
 * replaced with a decent configure option and/or logic.
 */

#ifndef UIO_MAXIOV
  #if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__)
    #define UIO_MAXIOV 1024
  #elif defined(__sun)
    #ifndef IOV_MAX
      #define UIO_MAXIOV 16
    #else
      #define UIO_MAXIOV IOV_MAX
    #endif
  #elif defined(IOV_MAX)
      #define UIO_MAXIOV IOV_MAX
  #else
    #define UIO_MAXIOV 16
  #endif
#endif

/*
 * Some systems (Solaris) lack useful MIN/MAX macros
 * normally defined in sys/param.h so define them here.
 */

#ifndef MIN
#define MIN(a,b)                    (((a)<(b))?(a):(b))
#endif

#ifndef MAX
#define MAX(a,b)                    (((a)>(b))?(a):(b))
#endif

/*
 * This macro is required for proper formatting
 */

#ifndef PRIu64
#define PRIu64                      TCL_LL_MODIFIER "d"
#endif

/*
 * Older Solaris version (2.8-) have older definitions
 * of pointer formatting macros.
 */


#ifndef PRIdPTR
#if defined(_LP64) || defined(_I32LPx)
#define PRIdPTR                     "ld"
#else
#define PRIdPTR                     "d"
#endif
#endif

#ifndef PRIoPTR
#if defined(_LP64) || defined(_I32LPx)
#define PRIoPTR                     "lo"
#else
#define PRIoPTR                     "o"
#endif
#endif

#ifndef PRIiPTR
#if defined(_LP64) || defined(_I32LPx)
#define PRIiPTR                     "li"
#else
#define PRIiPTR                     "i"
#endif
#endif

#ifndef PRIuPTR
#if defined(_LP64) || defined(_I32LPx)
#define PRIuPTR                     "lu"
#else
#define PRIuPTR                     "u"
#endif
#endif

#ifndef PRIxPTR
#if defined(_LP64) || defined(_I32LPx)
#define PRIxPTR                     "lx"
#else
#define PRIxPTR                     "x"
#endif
#endif

#ifdef __cplusplus
#define NS_EXTERN                   extern "C" NS_STORAGE_CLASS
#else
#define NS_EXTERN                   extern NS_STORAGE_CLASS
#endif

/*
 * Various constants.
 */

#define NS_OK                       0
#define NS_ERROR                    (-1)
#define NS_TIMEOUT                  (-2)
#define NS_FATAL                    (-3)

#define NS_THREAD_DETACHED          1
#define NS_THREAD_JOINED            2
#define NS_THREAD_EXITED            4
#define NS_THREAD_NAMESIZE          64
#define NS_THREAD_MAXTLS            100

/*
 * The following objects are defined as pointers to dummy structures
 * to ensure proper type checking.  The actual objects underlying
 * objects are platform specific.
 */

typedef struct Ns_Thread_   *Ns_Thread;
typedef struct Ns_Tls_      *Ns_Tls;
typedef struct Ns_Mutex_    *Ns_Mutex;
typedef struct Ns_Cond_     *Ns_Cond;
typedef struct Ns_Cs_       *Ns_Cs;
typedef struct Ns_Sema_     *Ns_Sema;
typedef struct Ns_RWLock_   *Ns_RWLock;

typedef struct Ns_Time {
    time_t  sec;
    long    usec;
} Ns_Time;

typedef void (Ns_ThreadProc) (void *arg);
typedef void (Ns_TlsCleanup) (void *arg);
typedef void (Ns_ThreadArgProc) (Tcl_DString *, void *proc, void *arg);

/*
 * pthread.c
 */

NS_EXTERN void Nsthreads_LibInit(void);

/*
 * fork.c:
 */

NS_EXTERN int ns_fork(void);

/*
 * master.c:
 */

NS_EXTERN void Ns_MasterLock(void);
NS_EXTERN void Ns_MasterUnlock(void);

/*
 * memory.c:
 */

NS_EXTERN void *ns_malloc(size_t size) NS_GNUC_MALLOC;
NS_EXTERN void *ns_calloc(size_t num, size_t size) NS_GNUC_MALLOC;
NS_EXTERN void ns_free(void *buf);
NS_EXTERN void *ns_realloc(void *buf, size_t size) NS_GNUC_WARN_UNUSED_RESULT;
NS_EXTERN char *ns_strdup(const char *string) NS_GNUC_MALLOC;
NS_EXTERN char *ns_strcopy(const char *string) NS_GNUC_MALLOC;
NS_EXTERN char *ns_strncopy(const char *string, int size) NS_GNUC_MALLOC;

/*
 * mutex.c:
 */

NS_EXTERN void Ns_MutexInit(Ns_Mutex *mutexPtr);
NS_EXTERN void Ns_MutexDestroy(Ns_Mutex *mutexPtr);
NS_EXTERN void Ns_MutexLock(Ns_Mutex *mutexPtr);
NS_EXTERN int  Ns_MutexTryLock(Ns_Mutex *mutexPtr);
NS_EXTERN void Ns_MutexUnlock(Ns_Mutex *mutexPtr);
NS_EXTERN void Ns_MutexSetName(Ns_Mutex *mutexPtr, CONST char *name);
NS_EXTERN void Ns_MutexSetName2(Ns_Mutex *mutexPtr, CONST char *prefix,
                                CONST char *name);
NS_EXTERN void Ns_MutexList(Tcl_DString *dsPtr);

/*
 * rwlock.c:
 */

NS_EXTERN void Ns_RWLockInit(Ns_RWLock *lockPtr);
NS_EXTERN void Ns_RWLockDestroy(Ns_RWLock *lockPtr);
NS_EXTERN void Ns_RWLockRdLock(Ns_RWLock *lockPtr);
NS_EXTERN void Ns_RWLockWrLock(Ns_RWLock *lockPtr);
NS_EXTERN void Ns_RWLockUnlock(Ns_RWLock *lockPtr);

/*
 * cslock.c;
 */

NS_EXTERN void Ns_CsInit(Ns_Cs *csPtr);
NS_EXTERN void Ns_CsDestroy(Ns_Cs *csPtr);
NS_EXTERN void Ns_CsEnter(Ns_Cs *csPtr);
NS_EXTERN void Ns_CsLeave(Ns_Cs *csPtr);

/*
 * cond.c:
 */

NS_EXTERN void Ns_CondInit(Ns_Cond *condPtr);
NS_EXTERN void Ns_CondDestroy(Ns_Cond *condPtr);
NS_EXTERN void Ns_CondSignal(Ns_Cond *condPtr);
NS_EXTERN void Ns_CondBroadcast(Ns_Cond *condPtr);
NS_EXTERN void Ns_CondWait(Ns_Cond *condPtr, Ns_Mutex *lockPtr);
NS_EXTERN int Ns_CondTimedWait(Ns_Cond *condPtr, Ns_Mutex *lockPtr,
                Ns_Time *timePtr);

/*
 * reentrant.c:
 */

NS_EXTERN struct dirent *ns_readdir(DIR * pDir);
NS_EXTERN struct tm *ns_localtime(const time_t * clock);
NS_EXTERN struct tm *ns_gmtime(const time_t * clock);
NS_EXTERN char *ns_ctime(const time_t * clock);
NS_EXTERN char *ns_asctime(const struct tm *tmPtr);
NS_EXTERN char *ns_strtok(char *s1, const char *s2);
NS_EXTERN char *ns_inet_ntoa(struct in_addr addr);

/*
 * sema.c:
 */

NS_EXTERN void Ns_SemaInit(Ns_Sema *semaPtr, int initCount);
NS_EXTERN void Ns_SemaDestroy(Ns_Sema *semaPtr);
NS_EXTERN void Ns_SemaWait(Ns_Sema *semaPtr);
NS_EXTERN void Ns_SemaPost(Ns_Sema *semaPtr, int count);

/*
 * signal.c:
 */

#ifndef _WIN32
NS_EXTERN int ns_sigmask(int how, sigset_t * set, sigset_t * oset);
NS_EXTERN int ns_sigwait(sigset_t * set, int *sig);
NS_EXTERN int ns_signal(int sig, void (*proc)(int));
#endif

/*
 * thread.c:
 */

NS_EXTERN void Ns_ThreadCreate(Ns_ThreadProc *proc, void *arg, long stackSize,
                Ns_Thread *resultPtr);
NS_EXTERN void Ns_ThreadExit(void *arg);
NS_EXTERN void Ns_ThreadJoin(Ns_Thread *threadPtr, void **argPtr);
NS_EXTERN void Ns_ThreadYield(void);
NS_EXTERN void Ns_ThreadSetName(char *name, ...);
NS_EXTERN uintptr_t Ns_ThreadId(void);
NS_EXTERN void Ns_ThreadSelf(Ns_Thread *threadPtr);
NS_EXTERN char *Ns_ThreadGetName(void);
NS_EXTERN char *Ns_ThreadGetParent(void);
NS_EXTERN long Ns_ThreadStackSize(long size);
NS_EXTERN void Ns_ThreadList(Tcl_DString *dsPtr, Ns_ThreadArgProc *proc);

/*
 * time.c:
 */

NS_EXTERN void Ns_GetTime(Ns_Time *timePtr);
NS_EXTERN void Ns_AdjTime(Ns_Time *timePtr);
NS_EXTERN int  Ns_DiffTime(Ns_Time *t1, Ns_Time *t0, Ns_Time *resultPtr);
NS_EXTERN void Ns_IncrTime(Ns_Time *timePtr, time_t sec, long usec);
NS_EXTERN Ns_Time *Ns_AbsoluteTime(Ns_Time *absPtr, Ns_Time *adjPtr);

/*
 * tls.c:
 */

NS_EXTERN void Ns_TlsAlloc(Ns_Tls *tlsPtr, Ns_TlsCleanup *cleanup);
NS_EXTERN void Ns_TlsSet(Ns_Tls *tlsPtr, void *value);
NS_EXTERN void *Ns_TlsGet(Ns_Tls *tlsPtr);

/*
 * winthread.c:
 */

#ifdef _WIN32
NS_EXTERN DIR *opendir(char *pathname);
NS_EXTERN struct dirent *readdir(DIR *dp);
NS_EXTERN int closedir(DIR *dp);
NS_EXTERN int truncate(char *file, off_t size);
NS_EXTERN int link(char *from, char *to);
NS_EXTERN int symlink(char *from, char *to);
NS_EXTERN int kill(int pid, int sig);
#endif

#endif /* NSTHREAD_H */
