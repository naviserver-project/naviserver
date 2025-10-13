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
 * nsthread.h --
 *
 *  Core threading and system headers.
 *
 */

#ifndef NSTHREAD_H
#define NSTHREAD_H

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
# include "nsconfig.h"
#else
# if defined(_MSC_VER)
/* Hard-coded configuration for windows */
#  include "nsconfig-win32.h"
# endif
#endif

#ifdef __MINGW32__
# ifdef USE_TCL_STUBS
#  error USE_TCL_STUBS should be undefined
# endif
#endif

/*
 * Do we allow relative URI is the "Location" header field?
 *
 * RFC 2616 required an absolute URI in the "Location" header field. However,
 * in June 2014, RFC 2616 was replaced by RFC 7231, supporting relative
 * location URLs (https://www.rfc-editor.org/rfc/rfc7231#section-7.1.2).
 *
 * Allowing relative location URLs eases the construction of the location,
 * especially in situations, where it is hard (or often impossible) to provide
 * a validated location prefix of a URL.
 *
 * To obtain the old-style (RFC 2616) semantics, set the following flag to 0.
 */

#define NS_ALLOW_RELATIVE_REDIRECTS 1

/*
 * NS_INIT_ONCE: handle one-time initialization in a thread-safe manner.  The
 * macro addresses the concerns expressed in the "Double-checked Locking"
 * pattern (https://en.wikipedia.org/wiki/Double-checked_locking)
 *
 * The provided function should return NS_TRUE for compatibility with
 * windows. For Unix compilations, the return value is ignored.
 *
 * NS_INIT_ONCE is defined as a macro to provide different variables for the
 * controlling variables.
 */
#ifdef _WIN32
# define NS_INIT_ONCE(fn) \
    { static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT; \
        InitOnceExecuteOnce(&init_once, (PINIT_ONCE_FN)(fn), NULL, NULL); \
    }
#elif defined(HAVE_PTHREAD)
# define NS_INIT_ONCE(fn) \
    { static pthread_once_t init_once = PTHREAD_ONCE_INIT; \
        pthread_once(&init_once, (ns_funcptr_t)(fn));      \
    }
#else
# define NS_INIT_ONCE(fn) \
    { static volatile bool initialized = NS_FALSE; \
      if (!initialized) { \
          Ns_MasterLock(); \
          if (!initialized) { \
              (fn)(); \
              initialized = NS_TRUE; \
          } \
          Ns_MasterUnlock(); \
        } \
    }
#endif

#include <nscheck.h>
#include <fcntl.h>

#define UCHAR(c)                   ((unsigned char)(c))
#define INTCHAR(c)                 ((int)UCHAR((c)))

#ifndef NS_NO_DEPRECATED
# define NS_WITH_DEPRECATED
#endif
#define NS_WITH_DEPRECATED_5_0

/*
 * AFAICT, there is no reason to conditionalize NSTHREAD_EXPORTS
 * depending on the compiler used, it should ALWAYS be set:
 * --atp@piskorski.com, 2014/09/23 13:14 EDT
 */
#define NSTHREAD_EXPORTS

#if defined(__GNUC__) && (__GNUC__ > 2)
/* Use gcc branch prediction hint to minimize cost of e.g. DTrace
 * ENABLED checks.
 */
# define unlikely(x) (__builtin_expect(!!(x), 0))
# define likely(x) (__builtin_expect((x), 1))
#else
# define unlikely(x) (x)
# define likely(x) (x)
#endif

/* NS_ALIGNOF(T) -> alignment in bytes required for objects of type T */
#if defined(__cplusplus)
  /* C++11 and later */
  #define NS_ALIGNOF(T) alignof(T)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  /* C11 or newer in C mode */
  #define NS_ALIGNOF(T) _Alignof(T)
#elif defined(_MSC_VER)
  /* Older MSVC C mode: use MSVC extension */
  #define NS_ALIGNOF(T) __alignof(T)
#elif defined(__GNUC__) || defined(__clang__)
  /* GCC/Clang in non-C11 modes */
  #define NS_ALIGNOF(T) __alignof__(T)
#else
  /* Conservative fallback */
  #define NS_ALIGNOF(T) (sizeof(void *))
#endif


/***************************************************************
 * Main Windows defines, including
 *
 *  - mingw
 *  - Visual Studio
 *  - WIN32
 *  - WIN64
 ***************************************************************/
#ifdef _WIN32
# define NS_EXPORT                   __declspec(dllexport)
# define NS_IMPORT                   __declspec(dllimport)

# if defined(NSTHREAD_EXPORTS)
#  define NS_STORAGE_CLASS            NS_EXPORT
# else
#  define NS_STORAGE_CLASS            NS_IMPORT
# endif

# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif

/*
 * 0x0400  Windows NT 4.0
 * 0x0500  Windows 2000
 * 0x0501  Windows XP
 * 0x0502  Windows Server 2003
 * 0x0600  Windows Vista / Windows Server 2008
 * 0x0601  Windows 7
 * 0x0602  Windows 8
 * 0x0603  Windows 8.1
 * 0x0A00  Windows 10 and Windows 11
 */
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT                0x0600
# endif
# if _WIN32_WINNT < 0x0600
#  error _WIN32_WINNT should be >= 0x0600
# endif

# include <windows.h>
# include <winsock2.h>
# include <ws2tcpip.h>
# include <sys/timeb.h>
# include <sys/types.h>
# include <io.h>
# include <process.h>
# include <direct.h>

# define STDOUT_FILENO               1
# define STDERR_FILENO               2

# if defined(_MSC_VER)
/*
 * Visual Studio defines
 */

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
typedef          __int8 int8_t;
typedef unsigned __int8 uint8_t;

typedef          __int16 int16_t;
typedef unsigned __int16 uint16_t;

typedef          __int32 int32_t;
typedef unsigned __int32 uint32_t;

typedef          __int64 int64_t;
typedef unsigned __int64 uint64_t;

typedef          long int intmax_t;
typedef unsigned long int uintmax_t;
#endif

typedef          DWORD pid_t;
typedef          DWORD ns_sockerrno_t;
typedef          long uid_t;
typedef          long gid_t;
typedef          long suseconds_t;

#  define NS_INITGROUPS_GID_T int
#  define NS_MSG_IOVLEN_T int

#  define NS_SOCKET             SOCKET
#  define NS_INVALID_SOCKET     (INVALID_SOCKET)
#  define NS_INVALID_PID        (0)
#  define NS_INVALID_FD         (-1)

#  ifdef _WIN64
#   define HAVE_64BIT 1
typedef int64_t ssize_t;
#  else
typedef int32_t ssize_t;
#  endif

/*
MSVC++ 5.0  _MSC_VER == 1100
MSVC++ 6.0  _MSC_VER == 1200
MSVC++ 7.0  _MSC_VER == 1300
MSVC++ 7.1  _MSC_VER == 1310 (Visual Studio 2003)
MSVC++ 8.0  _MSC_VER == 1400 (Visual Studio 2005)
MSVC++ 9.0  _MSC_VER == 1500 (Visual Studio 2008)
MSVC++ 10.0 _MSC_VER == 1600 (Visual Studio 2010)
MSVC++ 11.0 _MSC_VER == 1700 (Visual Studio 2012)
MSVC++ 12.0 _MSC_VER == 1800 (Visual Studio 2013)
MSVC++ 14.0 _MSC_VER == 1900 (Visual Studio 2015 version 14.0)
MSVC++ 14.1 _MSC_VER == 1910 (Visual Studio 2017 version 15.0)
MSVC++ 14.2 _MSC_VER == 1920 (Visual Studio 2019 version 16.0)
MSVC++ 14.10 _MSC_VER == 1910 (Visual Studio 2017 version 15.0)
MSVC++ 14.11 _MSC_VER == 1911 (Visual Studio 2017 version 15.3 / 15.4)
MSVC++ 14.12 _MSC_VER == 1912 (Visual Studio 2017 version 15.5)
MSVC++ 14.13 _MSC_VER == 1913 (Visual Studio 2017 version 15.6)
MSVC++ 14.14 _MSC_VER == 1914 (Visual Studio 2017 version 15.7)
MSVC++ 14.15 _MSC_VER == 1915 (Visual Studio 2017 version 15.8)
MSVC++ 14.16 _MSC_VER == 1916 (Visual Studio 2017 version 15.9)
MSVC++ 14.20 _MSC_VER == 1920 (Visual Studio 2019 version 16.0)
MSVC++ 14.21 _MSC_VER == 1921 (Visual Studio 2019 version 16.1)
MSVC++ 14.22 _MSC_VER == 1922 (Visual Studio 2019 version 16.2)
MSVC++ 14.23 _MSC_VER == 1923 (Visual Studio 2019 version 16.3)
MSVC++ 14.24 _MSC_VER == 1924 (Visual Studio 2019 version 16.4)
MSVC++ 14.25 _MSC_VER == 1925 (Visual Studio 2019 version 16.5)
MSVC++ 14.26 _MSC_VER == 1926 (Visual Studio 2019 version 16.6)
MSVC++ 14.27 _MSC_VER == 1927 (Visual Studio 2019 version 16.7)
MSVC++ 14.28 _MSC_VER == 1928 (Visual Studio 2019 version 16.8 / 16.9)
MSVC++ 14.29 _MSC_VER == 1929 (Visual Studio 2019 version 16.10 / 16.11)
MSVC++ 14.30 _MSC_VER == 1930 (Visual Studio 2022 version 17.0)
MSVC++ 14.31 _MSC_VER == 1931 (Visual Studio 2022 version 17.1)
MSVC++ 14.32 _MSC_VER == 1932 (Visual Studio 2022 version 17.2)
MSVC++ 14.33 _MSC_VER == 1933 (Visual Studio 2022 version 17.3)
MSVC++ 14.34 _MSC_VER == 1934 (Visual Studio 2022 version 17.4)
MSVC++ 14.35 _MSC_VER == 1935 (Visual Studio 2022 version 17.5)
MSVC++ 14.36 _MSC_VER == 1936 (Visual Studio 2022 version 17.6)
MSVC++ 14.37 _MSC_VER == 1937 (Visual Studio 2022 version 17.7)
MSVC++ 14.38 _MSC_VER == 1938 (Visual Studio 2022 version 17.8)
MSVC++ 14.39 _MSC_VER == 1939 (Visual Studio 2022 version 17.9)
MSVC++ 14.40 _MSC_VER == 1940 (Visual Studio 2022 version 17.10)
MSVC++ 14.41 _MSC_VER == 1941 (Visual Studio 2022 version 17.11)
MSVC++ 14.42 _MSC_VER == 1942 (Visual Studio 2022 version 17.12)
MSVC++ 14.43 _MSC_VER == 1943 (Visual Studio 2022 version 17.13)
MSVC++ 14.44 _MSC_VER == 1944 (Visual Studio 2022 version 17.14)
*/

/*
 * Cope with changes in Universal CRT in Visual Studio 2015 where
 * e.g. vsnprintf() is no longer identical to _vsnprintf()
 */
#  if _MSC_VER < 1900
#   define vsnprintf                  _vsnprintf
#   define snprintf                   ns_snprintf
#  endif

#  define strtoll                     _strtoi64

#  define strcoll_l                   _strcoll_l
#  define locale_t                    _locale_t
#  define freelocale                  _free_locale

#  define access                      _access
#  define chsize                      _chsize
#  define close                       _close
#  define dup                         _dup
#  define dup2                        _dup2
#  define fileno                      _fileno
#  define mktemp                      _mktemp
#  define open                        _open
#  define putenv                      _putenv
#  define unlink                      _unlink

#  define timezone                    _timezone
#  define daylight                    _daylight
#  define timegm                      _mkgmtime

#  define getpid()                    (pid_t)GetCurrentProcessId()
#  define ftruncate(f,s)              _chsize((f),(s))

#  if _MSC_VER > 1600
#   define HAVE_INTPTR_T
#   define HAVE_UINTPTR_T
#  endif

# else
/*
 * MinGW
 */

#  define NS_SOCKET             int
#  define NS_INVALID_PID        (-1)
#  define NS_INVALID_SOCKET     (-1)
#  define NS_INVALID_FD         (-1)

#  define strcoll_l             _strcoll_l
#  define locale_t              _locale_t
#  define gettimeofday          mingw_gettimeofday

typedef int ns_sockerrno_t;
typedef long uid_t;
typedef long gid_t;
typedef long suseconds_t;
# endif


/*
 * ALL _WIN32
 */
# include <sys/stat.h> /* for __stat64 */
# include <malloc.h>   /* for alloca   */

# define NS_SIGHUP                  1
# define NS_SIGINT                  2
# define NS_SIGQUIT                 3
# define NS_SIGPIPE                13
# define NS_SIGTERM                15

# define DEVNULL                   "nul:"

/*
 * For the time being, don't try to be very clever
 * and define (platform-neutral) just those two modes
 * for mapping the files.
 * Although the underlying implementation(s) can do
 * much more, we really need only one (read-maps) now.
 */
# define NS_MMAP_READ               (FILE_MAP_READ)
# define NS_MMAP_WRITE              (FILE_MAP_WRITE)

# define sleep(n)                  (Sleep((n)*1000))
# define mkdir(d,m)                _mkdir((d))
# define strcasecmp                _stricmp
# define strncasecmp               _strnicmp

# define ns_sockclose              closesocket
# define ns_sockerrno              GetLastError()
# define ns_sockioctl              ioctlsocket
# define ns_sockstrerror           NsWin32ErrMsg

/*
 * Under MinGW we use nsconfig.h, for MSVC we pre-define environment here
 */

# ifndef HAVE_CONFIG_H
#  define F_OK                        0
#  define W_OK                        2
#  define R_OK                        4
#  define X_OK                        (R_OK)
#  ifndef va_copy
#   define va_copy(dst,src)           ((void)((dst) = (src)))
#  endif
#  define USE_TCLVFS                  1
#  define USE_THREAD_ALLOC            1
#  define VERSION                     (NS_PATCH_LEVEL)
#  define _LARGEFILE64_SOURCE         1
#  define _THREAD_SAFE                1
#  define TCL_THREADS                 1
#  define HAVE_GETADDRINFO            1
#  define HAVE_GETNAMEINFO            1
#  define HAVE_STRUCT_STAT64          1
#  define PACKAGE                     "naviserver"
#  define PACKAGE_NAME                "NaviServer"
#  define PACKAGE_STRING              (PACKAGE_NAME " " NS_PATCH_LEVEL)
#  define PACKAGE_TAG                 (PACKAGE_STRING)
#  define PACKAGE_TARNAME             (PACKAGE)
#  define PACKAGE_VERSION             (NS_VERSION)
#  define PACKAGE_BUGREPORT           "naviserver-devel@lists.sourceforge.net"
#  define TIME_T_MAX                  (LONG_MAX)
#  define HAVE_IPV6                   1
#  define HAVE_INET_NTOP              1
#  define HAVE_INET_PTON              1
#  ifndef NS_NAVISERVER
#  define NS_NAVISERVER               "c:/ns"
#  endif
# endif

/*
 * The following structure defines an I/O scatter/gather buffer for WIN32.
 */

struct iovec {
    size_t      iov_len;     /* the length of the buffer */
    char FAR *  iov_base;    /* the pointer to the buffer */
};

/*
 * The following is for supporting our own poll() emulation.
 */
# ifndef POLLIN
#  define POLLIN                      0x0001
#  define POLLPRI                     0x0002
#  define POLLOUT                     0x0004
#  define POLLERR                     0x0008
#  define POLLHUP                     0x0010

struct pollfd {
    NS_SOCKET fd;
    short     events;
    short     revents;
};
# endif


/*
 * Provide compatibility for shutdown() on sockets.
 */

# ifndef SHUT_RD
#  define SHUT_RD SD_RECEIVE
# endif
# ifndef SHUT_WR
#  define SHUT_WR SD_SEND
# endif
# ifndef SHUT_RDWR
#  define SHUT_RDWR SD_BOTH
# endif

/*
 * Provide compatibility for MSG_DONTWAIT
 */
# ifndef MSG_DONTWAIT
#  define MSG_DONTWAIT 0
# endif

/*
 * The following is for supporting opendir/readdir functionality
 */

struct dirent {
    char *d_name;
};

typedef struct DIR_ *DIR;
/*
 * End of Windows section
 */
# else
/***************************************************************
 *
 * Not windows
 *
 * mostly Unix style OSes, including macOS
 *
 ***************************************************************/
# include <sys/types.h>
# include <dirent.h>
# include <sys/time.h>
# include <netinet/in.h>
# include <arpa/inet.h>
# include <string.h>
# include <unistd.h>
# include <sys/socket.h>
# include <netdb.h>
# include <sys/uio.h>
# include <poll.h>
# include <sys/resource.h>
# include <sys/wait.h>
# include <sys/ioctl.h>
# include <ctype.h>
# include <grp.h>
# include <pthread.h>
# include <sys/mman.h>
# include <poll.h>

# define NS_SOCKET            int
# define NS_INVALID_SOCKET     (-1)
# define NS_INVALID_PID        (-1)
# define NS_INVALID_FD         (-1)

/*
 * Many modules use SOCKET and not NS_SOCKET; don't force updates for
 * the time being, although the use of SOCKET should be deprecated.
 */
# ifndef SOCKET
#  define SOCKET NS_SOCKET
# endif

typedef int ns_sockerrno_t;


# if defined(HAVE_SYS_UIO_H)
#  include <sys/uio.h>
# elif defined(HAVE_UIO_H)
#  include <uio.h>
# endif

# ifdef HAVE_NETINET_TCP_H
#  include <netinet/tcp.h>
# endif

# ifdef __linux
#  include <sys/prctl.h>
# endif

# ifdef __hp
#  define seteuid(i) setresuid((-1),(i),(-1))
# endif

# ifdef __sun
#  include <sys/filio.h>
#  include <sys/systeminfo.h>
#  include <alloca.h>
#  define gethostname(b,s) (!(sysinfo(SI_HOSTNAME, b, s) > 0))
# endif

# ifdef __unixware
#  include <sys/filio.h>
# endif

# if defined(__sgi) && (!defined(_SGI_MP_SOURCE))
#  define _SGI_MP_SOURCE
# endif

# if defined(__sun) && (!defined(_POSIX_PTHREAD_SEMANTICS))
#  define _POSIX_PTHREAD_SEMANTICS
# endif

# if defined(__APPLE__) || defined(__darwin__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#  ifndef s6_addr16
#   define s6_addr16 __u6_addr.__u6_addr16
#  endif
#  ifndef s6_addr32
#   define s6_addr32 __u6_addr.__u6_addr32
#  endif
#  define S6_ADDR16(x) ((uint16_t*)(x).s6_addr16)
#  define NS_INITGROUPS_GID_T int
#  define NS_MSG_IOVLEN_T int
# else
#  if defined(__sun)
#   define S6_ADDR16(x) ((uint16_t*)((char*)&(x).s6_addr))
#   ifndef s6_addr32
#    define s6_addr32 _S6_un._S6_u32
#   endif
#  else
#   define S6_ADDR16(x) ((uint16_t*)(x).s6_addr16)
#  endif
#  define NS_INITGROUPS_GID_T gid_t
#  if NS_MSG_IOVLEN_IS_SIZE_T
#   define NS_MSG_IOVLEN_T size_t
#  else
#   define NS_MSG_IOVLEN_T int
#  endif
# endif

# ifdef __OpenBSD__
#  ifndef ENOTSUP
/*
 * Workaround until we have ENOTSUP in errno.h
 */
#   define ENOTSUP                  EOPNOTSUPP
#  endif
# endif

# define SOCKET_ERROR               (-1)

# define NS_SIGHUP                  (SIGHUP)
# define NS_SIGINT                  (SIGINT)
# define NS_SIGQUIT                 (SIGQUIT)
# define NS_SIGPIPE                 (SIGPIPE)
# define NS_SIGTERM                 (SIGTERM)

# define DEVNULL                   "/dev/null"

# define NS_MMAP_READ               (PROT_READ)
# define NS_MMAP_WRITE              (PROT_WRITE)

# ifdef HAVE_MKDTEMP
#  define ns_mkdtemp                 mkdtemp
# endif
# define ns_mkstemp                 mkstemp

# define ns_recv                    recv
# define ns_send                    send
# define ns_sockclose               close
# define ns_sockdup(fd)             ns_dup((fd))
# define ns_sockerrno               errno
# define ns_sockioctl               ioctl
# define ns_socknbclose             close
# define ns_sockstrerror            strerror

# define ns_open                    open
# define ns_close                   close
# define ns_read                    read
# define ns_write                   write
# define ns_lseek                   lseek
# define ns_getline                 getline

#ifdef HAVE_MEMMEM
# define ns_memmem                  memmem
#endif

# if __GNUC__
#  if defined(__x86_64__) || defined(__ppc64__)
#   define HAVE_64BIT 1
#  endif
# endif

# if __GNUC__ >= 4
#  define NS_EXPORT                 __attribute__ ((visibility ("default")))
# else
#  define NS_EXPORT
# endif /* __GNUC__ >= 4 */
# define NS_IMPORT
# if defined(NSTHREAD_EXPORTS)
#  define NS_STORAGE_CLASS            NS_EXPORT
# else
#  define NS_STORAGE_CLASS            NS_IMPORT
# endif
#endif /* _WIN32 */

/***************************************************************
 *
 * Common part, Unix and Windows
 *
 ***************************************************************/

#include <tcl.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/stat.h>

/* Fallback if the compiler doesn't support __has_builtin */
#ifndef __has_builtin
# define __has_builtin(x) 0
#endif

/*
 * Define ns_bswap32 and use builtins if defined
 */
#if defined(_MSC_VER)
# define ns_bswap32 _byteswap_ulong
#elif __has_builtin(__builtin_bswap32)
 /* Clang (and GCC >=10 via __has_builtin) */
# define ns_bswap32 __builtin_bswap32
#elif defined(__GNUC__) || defined(__clang__)
 /* Older GCC/Clang: builtin exists even without __has_builtin */
# define ns_bswap32 __builtin_bswap32
#else
/* Portable fallback */
  static inline uint32_t ns_bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0xFF000000u) >> 24);
  }
#endif

#define NS_HAVE_PARSEHOST2_CONST 1

#ifndef O_TEXT
# define O_TEXT    (0)
#endif
#ifndef O_BINARY
# define O_BINARY  (0)
#endif
#ifndef O_CLOEXEC
# define O_CLOEXEC (0)
#endif
#ifndef SOCK_CLOEXEC
# define SOCK_CLOEXEC (0)
#endif

#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

#if TCL_MAJOR_VERSION<=8 && TCL_MINOR_VERSION<5
# define NS_TCL_PRE85
#endif

#if TCL_MAJOR_VERSION<=8 && TCL_MINOR_VERSION<6
# define NS_TCL_PRE86
#endif

#if TCL_MAJOR_VERSION<=8 && TCL_MINOR_VERSION<7
# define NS_TCL_PRE87
#endif

#ifndef NS_TCL_PRE87
# if TCL_MAJOR_VERSION<=8 && TCL_MINOR_VERSION>=7 && TCL_RELEASE_SERIAL>=6
#  define NS_TCL_HAVE_TIP629
# elif (TCL_MAJOR_VERSION>=9)
#  define NS_TCL_HAVE_TIP629
# endif
#endif

#if TCL_MAJOR_VERSION<9
# define NS_TCL_PRE9
#endif

#ifndef TCL_INDEX_NONE
# define TCL_INDEX_NONE -1
#endif

#ifndef TCL_IO_FAILURE
# define TCL_IO_FAILURE -1
#endif

#ifndef TCL_HASH_TYPE
# define TCL_HASH_TYPE unsigned
#endif

/*
 * The intended meaning of CONST86 is: some type is defined as "const" in Tcl
 * 8.6, but was NOT defined as such in Tcl 8.5.
 */
#ifndef CONST86
# ifdef NS_TCL_PRE86
#  define CONST86
# else
#  define CONST86 const
# endif
#endif

#ifdef NS_TCL_PRE9
# define TCL_SIZE_T           int
# define TCL_SIZE_MAX         INT_MAX
# define TCL_OBJCMDPROC_T     Tcl_ObjCmdProc
# define TCL_CREATEOBJCOMMAND Tcl_CreateObjCommand
#else
# define TCL_SIZE_T           Tcl_Size
# define TCL_OBJCMDPROC_T     Tcl_ObjCmdProc2
# define TCL_CREATEOBJCOMMAND Tcl_CreateObjCommand2
#endif

#if !defined(NS_POLL_NFDS_TYPE)
# define NS_POLL_NFDS_TYPE unsigned int
#endif

#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#ifdef HAVE_STDINT_H
# include <stdint.h>
#endif

#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif

#ifdef HAVE_IPV6
# ifndef AF_INET6
#  warning "Strange System: have no AF_INET6. Deactivating IPv6 support."
#  undef HAVE_IPV6
# endif
#endif

#ifdef HAVE_IPV6
# ifndef HAVE_INET_PTON
#  warning "Strange System: have AF_INET6 but no HAVE_INET_PTON. Deactivating IPv6 support."
#  undef HAVE_IPV6
# endif
#endif

#define NS_SENTINEL (char *)0L

#ifdef HAVE_IPV6
# define NS_IP_LOOPBACK      "::1"
# define NS_IP_UNSPECIFIED   "::"
# define NS_SOCKADDR_STORAGE sockaddr_storage
# define NS_IPADDR_SIZE      INET6_ADDRSTRLEN
#else
# define NS_IP_LOOPBACK      "127.0.0.1"
# define NS_IP_UNSPECIFIED   "0.0.0.0"
# define NS_SOCKADDR_STORAGE sockaddr_in
# define NS_IPADDR_SIZE      INET_ADDRSTRLEN
#endif

#ifndef NS_RESTRICT
#  if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
      /* C99 or newer */
#    define NS_RESTRICT restrict
#  elif defined(_MSC_VER)
      /* MSVC */
#    define NS_RESTRICT __restrict
#  elif defined(__GNUC__) || defined(__clang__)
      /* GNU/Clang extensions in older modes */
#    define NS_RESTRICT __restrict__
#  else
#    define NS_RESTRICT
#  endif
#endif

/*
 * Well behaved compiler with C99 support should define __STDC_VERSION__
 */
#if defined(__STDC_VERSION__)
# if __STDC_VERSION__ >= 199901L
#  define NS_HAVE_C99
# endif
# if __STDC_VERSION__ >= 201112L
#  define NS_HAVE_C11
# endif
#endif

/*
 * Starting with Visual Studio 2013, Microsoft provides C99 library support.
 */
#if (!defined(NS_HAVE_C99)) && defined(_MSC_VER) && (_MSC_VER >= 1800)
# define NS_HAVE_C99
#endif

/*
 * Boolean type "bool" and constants
 */
#ifdef NS_HAVE_C99
   /*
    * C99
    */
# include <stdbool.h>
# define NS_TRUE                    true
# define NS_FALSE                   false
#else
   /*
    * Not C99
    */
# if defined(__cplusplus)
   /*
    * C++ is similar to C99, but no include necessary
    */
#  define NS_TRUE                    true
#  define NS_FALSE                   false
# else
   /*
    * If everything fails, use int type and int values for bool
    */
typedef int bool;
#  define NS_TRUE                    1
#  define NS_FALSE                   0
# endif
#endif


#ifdef _WIN32
/*
 * Starting with VS2010 constants like EWOULDBLOCK are defined in
 * errno.h differently to the WSA* counterparts.  Relevant to NaviServer are
 *
 *     EWOULDBLOCK != WSAEWOULDBLOCK
 *     EINPROGRESS != WSAEINPROGRESS
 *     EINTR       != WSAEINTR
 *
 * However, winsock2 continues to return the WSA values, but defined as well
 * the names without the "WSA" prefix.  So we have to abstract to NS_* to cope
 * with earlier versions and to provide cross_platform support.
 *
 * http://stackoverflow.com/questions/14714654/c-project-in-vs2008-works-but-in-vs2010-does-not
 * https://lists.gnu.org/archive/html/bug-gnulib/2011-10/msg00256.html
 */
# define NS_EWOULDBLOCK              WSAEWOULDBLOCK
# define NS_EAGAIN                   WSAEWOULDBLOCK
# define NS_EINPROGRESS              WSAEINPROGRESS
# define NS_EINTR                    WSAEINTR
# define NS_ETIMEDOUT                WSAETIMEDOUT

  /* Get last socket error */
# define NS_SOCK_ERRNO()             WSAGetLastError()
# ifndef P_tmpdir
#  define P_tmpdir "c:/temp"
# endif
#else
# define NS_EWOULDBLOCK              EWOULDBLOCK
# define NS_EINPROGRESS              EINPROGRESS
# define NS_EINTR                    EINTR
# define NS_EAGAIN                   EAGAIN
# define NS_ETIMEDOUT                ETIMEDOUT

# define NS_SOCK_ERRNO()             (errno)
#endif

#define NS_ERRNO_WOULDBLOCK(e) \
    ((e) == NS_EAGAIN || ((NS_EAGAIN != NS_EWOULDBLOCK) && (e) == NS_EWOULDBLOCK))

#define NS_ERRNO_SHOULD_RETRY(e) \
    ((e) == NS_EINTR || NS_ERRNO_WOULDBLOCK(e))

#ifndef S_ISREG
# define S_ISREG(m)                 ((m) & _S_IFREG)
#endif

#ifndef S_ISDIR
# define S_ISDIR(m)                 ((m) & _S_IFDIR)
#endif

#ifndef F_CLOEXEC
# define F_CLOEXEC                  1
#endif

#ifndef PATH_MAX
# define PATH_MAX 1024
#endif

/*
 * Some very old gcc versions do not have LLONG_* defined, instead of
 * messing with configure here it is a simple define for such cases
 */

#ifndef LLONG_MAX
# define LLONG_HALF                 (1LL << (sizeof (long long int) * CHAR_BIT - 2))
# define LLONG_MAX                  (LLONG_HALF - 1 + LLONG_HALF)
# define LLONG_MIN                  (-LLONG_MAX-1)
#endif

/*
 * This baroque pre-processor fiddling should be eventually
 * replaced with a decent configure option and/or logic.
 */

#ifndef UIO_MAXIOV
# ifdef IOV_MAX
#  define UIO_MAXIOV IOV_MAX
# else
#  if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__)
#   define UIO_MAXIOV 1024
#  elif defined(__sun)
#   define UIO_MAXIOV 16
#  else
#   define UIO_MAXIOV 16
#  endif
# endif
#endif

#ifndef UIO_SMALLIOV
# define UIO_SMALLIOV 8
#endif

#ifdef TCL_WIDE_INT_IS_LONG
# define WIDE_INT_MAX (LONG_MAX)
# define WIDE_INT_MIN (LONG_MIN)
#else
# define WIDE_INT_MAX (LLONG_MAX)
# define WIDE_INT_MIN (LLONG_MIN)
#endif

/*
 * Some systems (Solaris) lack useful MIN/MAX macros
 * normally defined in sys/param.h so define them here.
 */

#ifndef MIN
# define MIN(a,b)                    (((a)<(b))?(a):(b))
#endif

#ifndef MAX
# define MAX(a,b)                    (((a)>(b))?(a):(b))
#endif

/*
 * Define a few macros from inttypes.h which are
 * missing on some platforms.
 */
#if !defined(PRId64)
# define PRId64      "I64d"
#endif
#if !defined(PRId32)
# define PRId32      "I32d"
#endif
# if !defined(PRIuMAX)
# define PRIuMAX     "I64u"
#endif
#if !defined(PRIu64)
# define PRIu64      "I64u"
#endif
#if !defined(PRIx64)
# define PRIx64      "I64x"
#endif

#if !defined(SCNd64)
# if !defined __PRI64_PREFIX
#  if defined(HAVE_64BIT)
#   define __PRI64_PREFIX  "l"
#  else
#   define __PRI64_PREFIX  "ll"
#  endif
# endif
# define SCNd64      __PRI64_PREFIX "d"
#endif

/* We assume, HAVE_64BIT implies __WORDSIZE == 64 */
#if !defined(SCNxPTR)
# if !defined __PRIPTR_PREFIX
#   if defined(_WIN64)
#     define __PRIPTR_PREFIX  "I64"
#   elif defined(_WIN32)
#     define __PRIPTR_PREFIX  "I32"
#   elif defined(HAVE_64BIT)
#     define __PRIPTR_PREFIX  "l"
#   else
#     define __PRIPTR_PREFIX  "ll"
#   endif
# endif
# define SCNxPTR      __PRIPTR_PREFIX "x"
#endif


/*
 * There is apparently no platform independent print format for items
 * of size_t. Therefore, we invent here our own variant, trying to
 * stick to the naming conventions.
 */
#if !defined(PRIuz) && defined(_WIN64)
# define PRIuz "I64u"
#endif
#if !defined(PRIuz) && defined(_WIN32)
# define PRIuz "I32u"
#endif
#if !defined(PRIuz)
# define PRIuz "zu"
#endif

#ifdef NS_TCL_PRE9
# define PRITcl_Size "d"
#else
# define PRITcl_Size PRIuz
#endif

/*
 * There is apparently no platform independent print format for items
 * of ssize_t. Therefore, we invent here our own variant, trying to
 * stick to the naming conventions.
 */
#if (!defined(PRIdz)) && defined(_WIN64)
# define PRIdz PRId64
#endif
#if (!defined(PRIdz)) && defined(_WIN32)
# define PRIdz PRId32
#endif
#if !defined(PRIdz)
# define PRIdz "zd"
#endif

/*
 * There is apparently no platform independent print format for items
 * of off_t. Therefore, we invent here our own variant, trying to
 * stick to the naming conventions.
 */
#if !defined(PROTd)
# define PROTd PRId64
#endif

#if defined(__sun) && defined(__SVR4)
# define PRIiovlen "ld"
#else
# define PRIiovlen PRIdz
#endif

#define NS_TIME_FMT "%" PRId64 ".%06ld"

/*
 * Older Solaris version (2.8-) have older definitions
 * of pointer formatting macros.
 */
#if !defined(__PRIPTR_PREFIX)
# if defined(_LP64) || defined(_I32LPx) || defined(HAVE_64BIT) || defined(_WIN64) || defined(_WIN32)
#  if defined(_WIN32)
#   define __PRIPTR_PREFIX "ll"
#  else
#   define __PRIPTR_PREFIX "l"
#  endif
# else
#  define __PRIPTR_PREFIX
# endif
#endif


#ifndef PRIdPTR
# define PRIdPTR    __PRIPTR_PREFIX "d"
#endif

#ifndef PRIoPTR
# define PRIoPTR    __PRIPTR_PREFIX "o"
#endif

#ifndef PRIiPTR
# define PRIiPTR    __PRIPTR_PREFIX "i"
#endif

#ifndef PRIuPTR
# define PRIuPTR    __PRIPTR_PREFIX "u"
#endif

#ifndef PRIxPTR
# define PRIxPTR    __PRIPTR_PREFIX "x"
#endif

#if (!defined(INT2PTR)) && (!defined(PTR2INT))
#   if defined(HAVE_INTPTR_T) || defined(intptr_t)
#       define INT2PTR(p)  ((void *)(intptr_t)(p))
#       define PTR2INT(p)  ((int)(intptr_t)(p))
#       define UINT2PTR(p) ((void *)(uintptr_t)(p))
#       define PTR2UINT(p) ((unsigned int)(uintptr_t)(p))
#   else
#       define INT2PTR(p)  ((void *)(p))
#       define PTR2INT(p)  ((int)(p))
#       define UINT2PTR(p) ((void *)(p))
#       define PTR2UINT(p) ((unsigned int)(p))
#   endif
#endif

#if defined(HAVE_INTPTR_T) || defined(intptr_t)
# define LONG2PTR(p) ((void*)(intptr_t)(p))
# define PTR2LONG(p) ((long)(intptr_t)(p))
#else
# define LONG2PTR(p) ((void*)(p))
# define PTR2LONG(p) ((long)(p))
#endif

#ifdef _WIN32
# define PTR2NSSOCK(p) PTR2UINT(p)
# define NSSOCK2PTR(p) UINT2PTR(p)
#else
# define PTR2NSSOCK(p) PTR2INT(p)
# define NSSOCK2PTR(p) INT2PTR(p)
#endif

#ifdef NS_TCL_PRE9
# define PTR2TCL_SIZE(p) PTR2UINT(p)
#else
# define PTR2TCL_SIZE(p) ((uintptr_t)(p))
#endif


#ifndef	SSIZE_MAX
/* We assume, HAVE_64BIT implies __WORDSIZE == 64 */
# if defined(HAVE_64BIT)
#  define SSIZE_MAX	LONG_MAX
# else
#  define SSIZE_MAX	INT_MAX
# endif
#endif


#if defined(F_DUPFD_CLOEXEC)
# define ns_dup(fd)     fcntl((fd), F_DUPFD_CLOEXEC, 0)
#else
# define ns_dup(fd)     dup((fd))
#endif
# define ns_dup2        dup2

#ifdef __cplusplus
# define NS_EXTERN                   extern "C" NS_STORAGE_CLASS
#else
# define NS_EXTERN                   extern NS_STORAGE_CLASS
#endif

/*
 * NaviServer return codes. Similar to Tcl return codes, but not compatible,
 * since negative numbers denote different kinds of non-success.
 */
typedef enum {
    NS_OK =               ( 0), /* success */
    NS_ERROR =            (-1), /* error */
    NS_TIMEOUT =          (-2), /* timeout occurred */
    NS_UNAUTHORIZED =     (-3), /* authorize result, not authorized, let user retry */
    NS_FORBIDDEN =        (-4), /* authorize result, not authorized, don't allow retry */
    NS_FILTER_BREAK =     (-5), /* filter result, returned by e.g. Ns_FilterProc */
    NS_FILTER_RETURN =    (-6)  /* filter result, returned by e.g. Ns_FilterProc */
} Ns_ReturnCode;

/*
 * The following are the possible values for specifying read/write operations.
 */
typedef enum {
    NS_READ,
    NS_WRITE
} NS_RW;

/*
 * Constants for nsthread
 */
#define NS_THREAD_DETACHED          0x01u
#define NS_THREAD_JOINED            0x02u
#define NS_THREAD_EXITED            0x04u

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
typedef void (Ns_ThreadArgProc) (Tcl_DString *dsPtr, Ns_ThreadProc proc, const void *arg);

/*
 * pthread.c
 */

NS_EXTERN void Nsthreads_LibInit(void);

/*
 * fork.c:
 */

NS_EXTERN pid_t ns_fork(void);

/*
 * master.c:
 */

NS_EXTERN void Ns_MasterLock(void);
NS_EXTERN void Ns_MasterUnlock(void);

/*
 * memory.c:
 */

NS_EXTERN void *ns_malloc(size_t size) NS_GNUC_MALLOC NS_ALLOC_SIZE1(1) NS_GNUC_WARN_UNUSED_RESULT;
NS_EXTERN void *ns_calloc(size_t num, size_t size) NS_GNUC_MALLOC NS_ALLOC_SIZE2(1,2) NS_GNUC_WARN_UNUSED_RESULT;
NS_EXTERN void ns_free(void *buf);
NS_EXTERN void *ns_realloc(void *buf, size_t size) NS_ALLOC_SIZE1(2) NS_GNUC_WARN_UNUSED_RESULT;
NS_EXTERN char *ns_strdup(const char *string) NS_GNUC_NONNULL(1) NS_GNUC_MALLOC NS_GNUC_WARN_UNUSED_RESULT;
NS_EXTERN char *ns_strcopy(const char *string) NS_GNUC_MALLOC;
NS_EXTERN void *ns_align_up(void *p, size_t a) NS_GNUC_NONNULL(1) NS_GNUC_PURE;
NS_EXTERN char *ns_strncopy(const char *string, ssize_t size) NS_GNUC_MALLOC;
NS_EXTERN int   ns_uint32toa(char *buffer, uint32_t n) NS_GNUC_NONNULL(1);
NS_EXTERN int   ns_uint64toa(char *buffer, uint64_t n) NS_GNUC_NONNULL(1);

#ifndef HAVE_MEMMEM
NS_EXTERN void *ns_memmem(const void *haystack, size_t haystackLength, const void *const needle, const size_t needleLength)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
#endif

/*
 *----------------------------------------------------------------------
 * ns_free_const --
 *
 *      Wrapper around ns_free() which accepts a const-qualified pointer.
 *      This helper explicitly discards the const qualifier before freeing,
 *      which is occasionally needed when the ownership of a buffer is
 *      logically transferred but its declaration was const.
 *
 *      The cast is wrapped in diagnostic pragmas to suppress compiler
 *      warnings (-Wcast-qual) on GCC/Clang, as the operation is deliberate
 *      and safe when the memory was originally obtained from ns_malloc()
 *      or equivalent.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Frees the provided memory block.
 *
 *----------------------------------------------------------------------
 */
static inline void ns_free_const(const void *p) {
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wcast-qual"
#endif
    ns_free((void *)p);       /* dropping const is intentional here */
#if defined(__GNUC__) || defined(__clang__)
# pragma GCC diagnostic pop
#endif
}

/*
 *----------------------------------------------------------------------
 * ns_iov_set --
 *
 *      Safely initialize an iovec structure with the specified base pointer
 *      and length.  The pointer assignment is performed via memcpy to avoid
 *      strict-aliasing and const-discard warnings when setting iov_base.
 *      This approach is portable across compilers that treat iov_base as
 *      a non-const void pointer.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The provided iovec structure is modified in place.
 *
 *----------------------------------------------------------------------
 */
static inline void ns_iov_set(struct iovec *v, const void *base, size_t len) {
    memcpy(&v->iov_base, &base, sizeof base); /* copy pointer value */
    v->iov_len = len;
}

static inline void *ns_const2voidp(const void *p) {
    void *q;
    memcpy(&q, &p, sizeof q);   // copy pointer bits; avoids -Wcast-qual
    return q;
}
/*
 * mutex.c:
 */

NS_EXTERN void Ns_MutexInit(Ns_Mutex *mutexPtr)       NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_MutexDestroy(Ns_Mutex *mutexPtr);
NS_EXTERN void Ns_MutexLock(Ns_Mutex *mutexPtr)       NS_GNUC_NONNULL(1);
NS_EXTERN Ns_ReturnCode Ns_MutexTryLock(Ns_Mutex *mutexPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_MutexUnlock(Ns_Mutex *mutexPtr)     NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_MutexList(Tcl_DString *dsPtr)       NS_GNUC_NONNULL(1);
NS_EXTERN const char *Ns_MutexGetName(Ns_Mutex *mutexPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_MutexSetName(Ns_Mutex *mutexPtr, const char *name)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

NS_EXTERN void Ns_MutexSetName2(Ns_Mutex *mutexPtr, const char *prefix, const char *name)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 * rwlock.c:
 */

NS_EXTERN void Ns_RWLockInit(Ns_RWLock *lockPtr)      NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_RWLockDestroy(Ns_RWLock *lockPtr);
NS_EXTERN void Ns_RWLockRdLock(Ns_RWLock *lockPtr)    NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_RWLockWrLock(Ns_RWLock *lockPtr)    NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_RWLockUnlock(Ns_RWLock *lockPtr)    NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_RWLockList(Tcl_DString *dsPtr)      NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_RWLockSetName2(Ns_RWLock *rwPtr, const char *prefix, const char *name)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * cslock.c;
 */

NS_EXTERN void Ns_CsInit(Ns_Cs *csPtr)       NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_CsDestroy(Ns_Cs *csPtr);
NS_EXTERN void Ns_CsEnter(Ns_Cs *csPtr);
NS_EXTERN void Ns_CsLeave(Ns_Cs *csPtr)      NS_GNUC_NONNULL(1);

/*
 * pthread.c:
 */

NS_EXTERN void Ns_CondInit(Ns_Cond *condPtr)          NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_CondDestroy(Ns_Cond *condPtr);
NS_EXTERN void Ns_CondSignal(Ns_Cond *condPtr)        NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_CondBroadcast(Ns_Cond *condPtr)     NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_CondWait(Ns_Cond *condPtr, Ns_Mutex *lockPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN Ns_ReturnCode Ns_CondTimedWait(Ns_Cond *condPtr, Ns_Mutex *lockPtr,
                                         const Ns_Time *timePtr)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * reentrant.c:
 */

NS_EXTERN struct dirent *ns_readdir(DIR *pDir)           NS_GNUC_NONNULL(1);
NS_EXTERN struct tm *ns_localtime(const time_t *timep)   NS_GNUC_NONNULL(1);
NS_EXTERN struct tm *ns_localtime_r(const time_t *timer, struct tm *buf) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN struct tm *ns_gmtime(const time_t *timep)      NS_GNUC_NONNULL(1);
NS_EXTERN char *ns_strtok(char *s1, const char *s2)      NS_GNUC_NONNULL(2);
NS_EXTERN char *ns_inet_ntoa(struct sockaddr *saPtr) NS_GNUC_RETURNS_NONNULL NS_GNUC_NONNULL(1);

/*
 * sema.c:
 */

NS_EXTERN void Ns_SemaInit(Ns_Sema *semaPtr, TCL_SIZE_T initCount) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_SemaDestroy(Ns_Sema *semaPtr)                    NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_SemaWait(Ns_Sema *semaPtr)                       NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_SemaPost(Ns_Sema *semaPtr, TCL_SIZE_T count)     NS_GNUC_NONNULL(1);

/*
 * signal.c:
 */

#ifndef _WIN32
NS_EXTERN int ns_sigmask(int how, sigset_t *set, sigset_t *oset) NS_GNUC_NONNULL(2);
NS_EXTERN int ns_sigwait(sigset_t *set, int *sig) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN int ns_signal(int sig, void (*proc)(int));
#endif

/*
 * thread.c:
 */

NS_EXTERN void Ns_ThreadCreate(Ns_ThreadProc *proc, void *arg, ssize_t stackSize,
                               Ns_Thread *resultPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_ThreadExit(void *arg)              NS_GNUC_NORETURN;
NS_EXTERN void* Ns_ThreadResult(void *arg) NS_GNUC_CONST;
NS_EXTERN void Ns_ThreadJoin(Ns_Thread *threadPtr, void **argPtr) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_ThreadYield(void);
NS_EXTERN void Ns_ThreadSetName(const char *fmt, ...) NS_GNUC_NONNULL(1) NS_GNUC_PRINTF(1, 2);
NS_EXTERN uintptr_t Ns_ThreadId(void);
NS_EXTERN void Ns_ThreadSelf(Ns_Thread *threadPtr) NS_GNUC_NONNULL(1);
NS_EXTERN const char *Ns_ThreadGetName(void)       NS_GNUC_RETURNS_NONNULL;
NS_EXTERN const char *Ns_ThreadGetParent(void)     NS_GNUC_RETURNS_NONNULL;
NS_EXTERN ssize_t Ns_ThreadStackSize(ssize_t size);
NS_EXTERN void Ns_ThreadList(Tcl_DString *dsPtr, Ns_ThreadArgProc *proc) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_ThreadGetThreadInfo(size_t *maxStackSize, size_t *estimatedSize)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
extern void  *NsThreadResult(void *arg) NS_GNUC_CONST;

/*
 * time.c:
 */

NS_EXTERN void Ns_GetTime(Ns_Time *timePtr) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_AdjTime(Ns_Time *timePtr) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_IncrTime(Ns_Time *timePtr, time_t sec, long usec)  NS_GNUC_NONNULL(1);
NS_EXTERN Ns_Time *Ns_AbsoluteTime(Ns_Time *absPtr, Ns_Time *adjPtr)  NS_GNUC_NONNULL(1);
NS_EXTERN Ns_Time *Ns_RelativeTime(Ns_Time *relTimePtr, Ns_Time *timePtr)  NS_GNUC_NONNULL(1);
NS_EXTERN long Ns_DiffTime(const Ns_Time *t1, const Ns_Time *t0, Ns_Time *resultPtr)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
NS_EXTERN time_t Ns_TimeToMilliseconds(const Ns_Time *timePtr)  NS_GNUC_NONNULL(1) NS_GNUC_PURE;

/*
 * tls.c:
 */

NS_EXTERN void Ns_TlsAlloc(Ns_Tls *tlsPtr, Ns_TlsCleanup *cleanup) NS_GNUC_NONNULL(1);
NS_EXTERN void Ns_TlsSet(const Ns_Tls *tlsPtr, void *value) NS_GNUC_NONNULL(1);
NS_EXTERN void *Ns_TlsGet(const Ns_Tls *tlsPtr) NS_GNUC_NONNULL(1);

/*
 * winthread.c:
 */

#ifdef _WIN32
NS_EXTERN DIR *opendir(char *pathname);
NS_EXTERN struct dirent *readdir(DIR *dp);
NS_EXTERN int closedir(DIR *dp);
NS_EXTERN int link(char *from, char *to);
NS_EXTERN int symlink(const char *from, const char *to);
NS_EXTERN int kill(pid_t pid, int sig);

# ifdef _MSC_VER
NS_EXTERN int truncate(const char *path, off_t length);
# endif
#endif

/*
 * nswin32.c:
 */
#ifdef _WIN32
NS_EXTERN int     ns_open(const char *path, int oflag, int mode);
NS_EXTERN int     ns_close(int fildes);
NS_EXTERN ssize_t ns_write(int fildes, const void *buf, size_t nbyte);
NS_EXTERN ssize_t ns_read(int fildes, void *buf, size_t nbyte);
NS_EXTERN off_t   ns_lseek(int fildes, off_t offset, int whence);
NS_EXTERN ssize_t ns_recv(NS_SOCKET socket, void *buffer, size_t length, int flags);
NS_EXTERN ssize_t ns_send(NS_SOCKET socket, const void *buffer, size_t length, int flags);
NS_EXTERN ssize_t ns_getline(char **lineptr, size_t *n, FILE *stream);
NS_EXTERN int     ns_snprintf(char *buf, size_t len, const char *fmt, ...);
#endif


/*
 * Tcl 8.6 and TIP 330/336 compatibility
 */

#if (TCL_MAJOR_VERSION < 8) || ((TCL_MAJOR_VERSION == 8) && (TCL_MINOR_VERSION < 6))
#define Tcl_GetErrorLine(interp) ((interp)->errorLine)
#endif

NS_EXTERN int NS_finalshutdown;
NS_EXTERN bool NS_mutexlocktrace;
#endif /* NSTHREAD_H */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
