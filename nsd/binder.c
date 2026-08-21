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
 * binder.c --
 *
 * Support for pre-bound privileged ports for Unix
 */

#include "nsd.h"

#ifndef _WIN32
# include <sys/un.h>
# include <sys/uio.h>

# if defined(HAVE_SD_LISTEN_FDS)
#  include <systemd/sd-daemon.h>
# elif defined( __APPLE__)
#  include <launch.h>
# endif

# define REQUEST_SIZE  (sizeof(int) + sizeof(int) + sizeof(int) + NS_IPADDR_SIZE)
# define RESPONSE_SIZE (sizeof(int))

typedef struct Prebind {
    size_t count;
    NS_SOCKET sockets[1];
} Prebind;

#endif

/*
 * Local variables defined in this file
 */

static Ns_Mutex      lock = NULL;
static Tcl_HashTable preboundTcp;
static Tcl_HashTable preboundUdp;
static Tcl_HashTable preboundRaw;
static Tcl_HashTable preboundUnix;

static bool binderRunning = NS_FALSE;
static NS_SOCKET binderRequest[2]  = { NS_INVALID_SOCKET, NS_INVALID_SOCKET };
static NS_SOCKET binderResponse[2] = { NS_INVALID_SOCKET, NS_INVALID_SOCKET };

/*
 * Local functions defined in this file
 */
#ifndef _WIN32
static Ns_ReturnCode PrebindSockets(const char *spec)
    NS_GNUC_NONNULL(1);

static void Binder(void);

static struct Prebind* PrebindAlloc(const char *proto, size_t reuses, struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1,3);

static bool PrebindGet(const char *proto, struct sockaddr *saPtr, NS_SOCKET *sockPtr)
    NS_GNUC_NONNULL(1,2,3);

static void PrebindCloseSockets(const char *proto, struct sockaddr *saPtr, struct Prebind *pPtr)
    NS_GNUC_NONNULL(1,2,3);

static Ns_ReturnCode PrebindFile(const char *file)
    NS_GNUC_NONNULL(1);

# if defined(HAVE_SD_LISTEN_FDS)
static Ns_ReturnCode PrebindSystemdSockets(size_t *countPtr);
# elif defined(__APPLE__)
static Ns_ReturnCode PrebindLaunchdSockets(size_t *countPtr);
# endif

# if defined(HAVE_SD_LISTEN_FDS) || defined(__APPLE__)
static Ns_ReturnCode PrebindRegisterSocket(NS_SOCKET sock);
static Ns_ReturnCode PrebindRegisterInetSocket(const char *proto, NS_SOCKET sock, const struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1,3);
static struct Prebind *PrebindAppendSocket(struct Prebind *pPtr, NS_SOCKET sock);
# endif

static Tcl_HashEntry *PrebindCreateHashEntry(
    Tcl_HashTable *tablePtr, const struct sockaddr *saPtr, int *isNewPtr)
    NS_GNUC_NONNULL(1,2,3);

static void PrebindSockaddrKey(struct NS_SOCKADDR_STORAGE *keyPtr, const struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1,2);

#endif /* !_WIN32 */

#ifdef LOGBIND
static FILE *log_fp = NULL;

static void log_bind(const char* proto, const char *addr, unsigned short port, const char*label) {
    if (log_fp == NULL) {
        log_fp = fopen("/tmp/binder.log", "a");
    }
    fprintf(log_fp, "DEBUG: prebind proto %s addr %s port %hu: %s\n",
            proto, addr, port, label);
}
#endif

#ifndef _WIN32


/*
 *----------------------------------------------------------------------
 *
 * PrebindSize --
 *
 *      Determine the allocation size of a Prebind structure capable of
 *      holding the requested number of socket descriptors.
 *
 * Results:
 *      Number of bytes required for the allocation.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static size_t
PrebindSize(size_t count)
{
    assert(count > 0u);

    return sizeof(Prebind) + (count - 1u) * sizeof(NS_SOCKET);
}

/*
 *----------------------------------------------------------------------
 *
 * PrebindCreateHashEntry --
 *
 *      Create a hash-table entry for the supplied Internet socket
 *      address. Normalize the address before using it as a hash key so
 *      addresses obtained from different operating-system interfaces
 *      produce identical keys.
 *
 * Results:
 *      Pointer to the created or existing hash-table entry. The value
 *      referenced by isNewPtr indicates whether a new entry was created.
 *
 * Side effects:
 *      May add an entry to the supplied hash table.
 *
 *----------------------------------------------------------------------
 */
static Tcl_HashEntry *
PrebindCreateHashEntry(Tcl_HashTable *tablePtr,
                       const struct sockaddr *saPtr,
                       int *isNewPtr)
{
    struct NS_SOCKADDR_STORAGE key;

    PrebindSockaddrKey(&key, saPtr);

    /*
     * The fixed-size array-key implementation copies the key.
     */
    return Tcl_CreateHashEntry(tablePtr, (const char *)&key, isNewPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * PrebindAlloc --
 *
 *      Create a Prebind structure with potentially multiple sockets
 *      binding to the identical port. This is needed for e.g. multiple
 *      listeners with SO_REUSEPORT.
 *
 * Results:
 *      Either a prebind structure or NULL inc ase of failure.
 *
 * Side effects:
 *      Allocating memory, binding of TCP or UDP sockets.
 *
 *----------------------------------------------------------------------
 */
static struct Prebind*
PrebindAlloc(const char *proto, size_t reuses, struct sockaddr *saPtr)
{
    struct Prebind *pPtr;

    NS_NONNULL_ASSERT(proto != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);

    pPtr = ns_malloc(PrebindSize(reuses));
    if (pPtr != NULL) {
        bool   reuseport;
        size_t i;

        pPtr->count = reuses;

        reuseport = (reuses > 1);

        for (i = 0u; i < reuses; i++) {
            if (*proto == 't') {
                pPtr->sockets[i] = Ns_SockBind(saPtr, reuseport);
            } else if (*proto == 'u') {
                pPtr->sockets[i] = Ns_SockBindUdp(saPtr, reuseport);
            } else {
                Ns_Log(Error, "prebind: invalid protocol %s", proto);
                ns_free(pPtr);
                pPtr = NULL;
                break;
            }

            if (pPtr->sockets[i] == NS_INVALID_SOCKET) {
                Ns_LogSockaddr(Error, "prebind error on ", (const struct sockaddr *)saPtr);
                Ns_Log(Error, "prebind error: %s", strerror(errno));
                if (i == 0) {
                    /*
                     * Could not bind to a single port. Return NULL to
                     * signal an invalid attempt.
                     */
                    ns_free(pPtr);
                    pPtr = NULL;
                    break;
                }
            }
        }
    }
    return pPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * PrebindGet --
 *
 *      Get a single socket from the prebind structure. In case of
 *      success, the function returns in its last argument the prebound
 *      socket and removes it from the set of available sockets. When
 *      all sockets are consumed the prebind structure is freed and the
 *      hash entry is removed.
 *
 * Results:
 *
 *      NS_TRUE in case, there is a prebind structure for the provided
 *      sockaddr or NS_FALSE on failure.
 *
 * Side effects:
 *      Potentially freeing memory.
 *
 *----------------------------------------------------------------------
 */
static bool
PrebindGet(const char *proto, struct sockaddr *saPtr, NS_SOCKET *sockPtr)
{
    struct NS_SOCKADDR_STORAGE key;
    Tcl_HashTable *tablePtr;
    Tcl_HashEntry *hPtr;
    bool           foundEntry = NS_FALSE;

    NS_NONNULL_ASSERT(proto != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(sockPtr != NULL);

    if (*proto == 't') {
        tablePtr = &preboundTcp;
    } else {
        tablePtr = &preboundUdp;
    }

    PrebindSockaddrKey(&key, saPtr);

    Ns_MutexLock(&lock);
    hPtr = Tcl_FindHashEntry(tablePtr, (char *)&key);
    if (hPtr != NULL) {
        struct Prebind *pPtr;
        size_t          i;
        bool            allConsumed = NS_TRUE;

        /*
         * We found a prebound entry.
         */
        foundEntry = NS_TRUE;

        pPtr = (struct Prebind *)Tcl_GetHashValue(hPtr);
        for (i = 0u; i < pPtr->count; i++) {
            /*
             * Find an entry, which is usable
             */
            if (pPtr->sockets[i] != NS_INVALID_SOCKET) {
                *sockPtr = pPtr->sockets[i];
                pPtr->sockets[i] = NS_INVALID_SOCKET;
                break;
            }
        }
        if (*sockPtr !=  NS_INVALID_SOCKET) {
            /*
             * Check, if there are more unconsumed entries.
             */
            for (; i < pPtr->count; i++) {
                if (pPtr->sockets[i] != NS_INVALID_SOCKET) {
                    /*
                     * Yes, there are more unconsumed entries.
                     */
                    allConsumed = NS_FALSE;
                    break;
                }
            }
        }
        if (allConsumed) {
            ns_free(pPtr);
            Tcl_DeleteHashEntry(hPtr);
        }
    }
    Ns_MutexUnlock(&lock);

    return foundEntry;
}


/*
 *----------------------------------------------------------------------
 *
 * PrebindCloseSockets --
 *
 *      Close the remaining prebound sockets.
 *
 * Results:
 *
 *      None.
 *
 * Side effects:
 *      Freeing memory.
 *
 *----------------------------------------------------------------------
 */
static void
PrebindCloseSockets(const char *proto, struct sockaddr *saPtr, struct Prebind *pPtr)
{
    size_t         i;
    unsigned short port;
    const char    *addr;
    char           ipString[NS_IPADDR_SIZE];
    int            count = 0;

    NS_NONNULL_ASSERT(proto != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(pPtr != NULL);

    addr = ns_inet_ntop((struct sockaddr *)saPtr, ipString, sizeof(ipString));
    port = Ns_SockaddrGetPort((struct sockaddr *)saPtr);

    for (i = 0u; i < pPtr->count; i++) {
        NS_SOCKET sock = pPtr->sockets[i];

        if (sock != NS_INVALID_SOCKET) {
            count ++;
            Ns_Log(Debug, "prebind closing %s socket %d\n", proto, sock);
            (void)ns_sockclose(sock);
        }
    }
    ns_free(pPtr);
    Ns_Log(Warning, "prebind: closed unused %d %s socket(s): [%s]:%hd",
           count, proto, addr, port);
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenEx --
 *
 *      Create a new TCP socket bound to the specified port and
 *      listening for new connections.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

#ifndef _WIN32
NS_SOCKET
Ns_SockListenEx(const char *address, unsigned short port, int backlog, bool reuseport)
{
    NS_SOCKET           sock = NS_INVALID_SOCKET;
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr     *saPtr = (struct sockaddr *)&sa;

    if (Ns_GetSockAddr(saPtr, address, port) == NS_OK) {
        bool found;

        found = PrebindGet("tcp", saPtr, &sock);
        if (!found) {
            /*
             * Prebind did not find a prebound entry, try to bind now.
             */
            sock = Ns_SockBind(saPtr, reuseport);
            //fprintf(stderr, "listen on port %hd binding with reuseport %d\n", port, reuseport);

            if (sock == NS_INVALID_SOCKET && Ns_GetSockErrno() == EALREADY) {
                /*
                 * soft-skip: already covered by dual-stack wildcard
                 */
                return NS_INVALID_SOCKET;
            }

        } else {
            //fprintf(stderr, "listen on port %hd already prebound\n", port);
        }

        if (sock != NS_INVALID_SOCKET && listen(sock, backlog) == -1) {
            /*
             * Can't listen; close the opened socket
             */
            ns_sockerrno_t err = ns_sockerrno;

            (void)ns_sockclose(sock);
            errno = err;
            sock = NS_INVALID_SOCKET;
            Ns_SetSockErrno(err);
        }
    } else {
        /*
         * We could not even get the sockaddr, so make clear, that saPtr
         * is invalid.
         */
        saPtr = NULL;
    }

    /*
     * If forked binder is running and we could not allocate socket
     * directly, try to do it through the binder
     */
    if (sock == NS_INVALID_SOCKET
        && binderRunning
        && saPtr != NULL
        && Ns_GetSockErrno() != EALREADY) {
        sock = Ns_SockBinderListen('T', address, port, backlog);
    }

    return sock;
}
#endif /* _WIN32 */


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenUdp --
 *
 *      Listen on the UDP socket for the given IP address and port.  The
 *      given address might be NULL, which implies the unspecified IP
 *      address ("0.0.0.0" or "::").
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      May create a new socket if none prebound.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListenUdp(const char *address, unsigned short port, bool reuseport)
{
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr *saPtr = (struct sockaddr *)&sa;
    NS_SOCKET        sock = NS_INVALID_SOCKET;

    if (Ns_GetSockAddr(saPtr, address, port) == NS_OK) {
        bool found;

#ifndef _WIN32
        found = PrebindGet("udp", saPtr, &sock);
#else
        found = NS_FALSE;
#endif
        if (!found) {
            /*
             * Not prebound, bind now
             */
            sock = Ns_SockBindUdp(saPtr, reuseport);
        }
    } else {
        saPtr = NULL;
    }

    /*
     * If forked binder is running and we could not allocate socket
     * directly, try to do it through the binder
     */

    if (sock == NS_INVALID_SOCKET
        && binderRunning
        && saPtr != NULL
        && Ns_GetSockErrno() != EALREADY
        ) {
        sock = Ns_SockBinderListen('U', address, port, 0);
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenRaw --
 *
 *      Listen on the raw socket addressed by the given protocol.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      May create a new socket if none prebound.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListenRaw(int proto)
{
    NS_SOCKET       sock = NS_INVALID_SOCKET;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    Ns_MutexLock(&lock);
    hPtr = Tcl_FirstHashEntry(&preboundRaw, &search);
    while (hPtr != NULL) {
        if (proto == PTR2INT(Tcl_GetHashValue(hPtr))) {
            sock = PTR2NSSOCK(Tcl_GetHashKey(&preboundRaw, hPtr));
            Tcl_DeleteHashEntry(hPtr);
            break;
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&lock);
    if (hPtr == NULL) {
        /*
         * Not prebound, bind now
         */
        sock = Ns_SockBindRaw(proto);
    }

    /*
     * If forked binder is running and we could not allocate socket
     * directly, try to do it through the binder
     */

    if (sock == NS_INVALID_SOCKET && binderRunning) {
        sock = Ns_SockBinderListen('R', NULL, 0u, proto);
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenUnix --
 *
 *      Listen on the Unix-domain socket addressed by the given path.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      May create a new socket if none prebound. If backlog is zero,
 *      DGRAM socket will be created otherwise STREAM socket
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListenUnix(const char *path, int backlog, unsigned short mode)
{
    NS_SOCKET      sock = NS_INVALID_SOCKET;
#ifndef _WIN32
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    NS_NONNULL_ASSERT(path != NULL);

    /*
     * Check if already prebound
     */
    Ns_MutexLock(&lock);
    hPtr = Tcl_FirstHashEntry(&preboundUnix, &search);
    while (hPtr != NULL) {
        const char *value = (char*) Tcl_GetHashValue(hPtr);

        if (STREQ(path, value)) {
            sock = PTR2NSSOCK(Tcl_GetHashKey(&preboundRaw, hPtr));
            Tcl_DeleteHashEntry(hPtr);
            break;
        }
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&lock);

    if (hPtr == NULL) {
        /*
         * Not prebound, bind now
         */
        sock = Ns_SockBindUnix(path, backlog > 0 ? SOCK_STREAM : SOCK_DGRAM, mode);
    }
    if (sock >= 0 && backlog > 0 && listen(sock, backlog) == -1) {
        /*
         * Can't listen; close the opened socket
         */
        ns_sockerrno_t err = ns_sockerrno;

        (void)ns_sockclose(sock);
        errno = err;
        sock = NS_INVALID_SOCKET;
        Ns_SetSockErrno(err);
    }

    /*
     * If forked binder is running and we could not allocate socket
     * directly, try to do it through the binder
     */

    if (sock == NS_INVALID_SOCKET && binderRunning) {
        sock = Ns_SockBinderListen('D', path, mode, backlog);
    }
#endif /* _WIN32 */
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBindUdp --
 *
 *      Create a UDP socket and bind it to the passed-in address.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockBindUdp(const struct sockaddr *saPtr, bool reusePort)
{
    NS_SOCKET sock;

    NS_NONNULL_ASSERT(saPtr != NULL);

    sock = (NS_SOCKET)socket((int)saPtr->sa_family, SOCK_DGRAM, 0);

    if (sock != NS_INVALID_SOCKET) {
        int n = 1;

        (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const void *)&n,
                         (socklen_t)sizeof(n));
        (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const void *)&n,
                         (socklen_t)sizeof(n));

#if defined(SO_REUSEPORT)
        if (reusePort) {
            int optval = 1;
            (void)setsockopt(sock, SOL_SOCKET, SO_REUSEPORT,
                             (const void *)&optval, (socklen_t)sizeof(optval));
        }
#endif

#ifdef HAVE_IPV6
        if (saPtr->sa_family == AF_INET6) {
            /*
             * Prefer dual-stack if the platform allows it (same rationale as TCP).
             */
            int v6only = 0;
            (void)setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
                             (const void *)&v6only, (socklen_t)sizeof(v6only));
        }
#endif

        if (bind(sock, saPtr, Ns_SockaddrGetSockLen(saPtr)) == -1) {
            ns_sockerrno_t err = ns_sockerrno;

            if (saPtr->sa_family == AF_INET
                && err == EADDRINUSE
                && Ns_SockaddrInAny(saPtr)) {

                unsigned short port = Ns_SockaddrGetPort(saPtr);

                Ns_Log(Notice,
                       "skipping UDP bind on [0.0.0.0]:%hu: already covered by [::]:%hu",
                       port, port);

                (void)ns_sockclose(sock);
                sock = NS_INVALID_SOCKET;

                errno = EALREADY;
                Ns_SetSockErrno(EALREADY);

                return sock;
            }

            (void)ns_sockclose(sock);
            sock = NS_INVALID_SOCKET;
            Ns_SetSockErrno(err);
        }
    }

    return sock;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBindUnix --
 *
 *      Create a Unix-domain socket and bind it to the passed-in
 *      file path.
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockBindUnix(const char *path, int socktype, unsigned short mode)
{
#ifdef _WIN32
    return NS_INVALID_SOCKET;
#else
    NS_SOCKET          sock;
    struct sockaddr_un addr;
    size_t             pathLength;

    NS_NONNULL_ASSERT(path != NULL);

    pathLength = strlen(path);

    if (pathLength >= sizeof(addr.sun_path)) {
        Ns_Log(Error, "provided path exceeds maximum length: %s\n", path);
        return NS_INVALID_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, pathLength + 1);
    unlink(path);

    sock = socket(AF_UNIX, socktype > 0 ? socktype : SOCK_STREAM, 0);
    /*
     * There is a small race condition below, since the permissions on
     * the socket are checked not in an atomic fashion and might be
     * changed immediately after the bind operation. Unfortunately,
     * fchmod is not portable.
     */
    if (sock != NS_INVALID_SOCKET
        && (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1
            || (mode != 0u && chmod(path, mode) == -1))
        ) {
        ns_sockerrno_t err = errno;

        (void)ns_sockclose(sock);
        sock = NS_INVALID_SOCKET;
        Ns_SetSockErrno(err);
    }

    return sock;
#endif /* _WIN32 */
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBindRaw --
 *
 *      Create a raw socket. It does not bind, hence the call name
 *      is not entirely correct but is on-pair with other types of
 *      sockets (udp, tcp, unix).
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockBindRaw(int proto)
{
    NS_SOCKET sock;

    sock = (NS_SOCKET)socket(AF_INET, SOCK_RAW, proto);

    if (sock == NS_INVALID_SOCKET) {
        ns_sockerrno_t err = ns_sockerrno;

        Ns_SetSockErrno(err);
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitBinder --
 *
 *      Initialize the pre-bind tables.
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
NsInitBinder(void)
{
    Ns_MutexInit(&lock);
    Ns_MutexSetName(&lock, "binder");

    Tcl_InitHashTable(&preboundTcp, (int)(sizeof(struct NS_SOCKADDR_STORAGE) / sizeof(int)));
    Tcl_InitHashTable(&preboundUdp, (int)(sizeof(struct NS_SOCKADDR_STORAGE) / sizeof(int)));
    Tcl_InitHashTable(&preboundRaw, TCL_ONE_WORD_KEYS);
    Tcl_InitHashTable(&preboundUnix, TCL_STRING_KEYS);
}

/*
 *----------------------------------------------------------------------
 *
 * PrebindFile --
 *
 *      Read pre-bind specifications from the specified file and pass
 *      every non-empty line to PrebindSockets().
 *
 * Results:
 *      NS_OK when the file was read and all specifications were
 *      processed successfully, otherwise NS_ERROR.
 *
 * Side effects:
 *      Opens and closes the specified file. Sockets may be created and
 *      added to the pre-bind hash tables.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
PrebindFile(const char *file)
{
    Ns_ReturnCode status = NS_OK;
    Tcl_Channel   chan = Tcl_OpenFileChannel(NULL, file, "r", 0);

    if (chan == NULL) {
        Ns_Log(Error, "NsPreBind: can't open file '%s': '%s'", file,
               strerror(Tcl_GetErrno()));
        status = NS_ERROR;
    } else {
        Tcl_DString line;

        Tcl_DStringInit(&line);
        while (Tcl_Eof(chan) == 0) {
            Tcl_DStringSetLength(&line, 0);
            if (Tcl_Gets(chan, &line) > 0) {
                status = PrebindSockets(Tcl_DStringValue(&line));
                if (status != NS_OK) {
                    break;
                }
            }
        }
        Tcl_DStringFree(&line);
        Tcl_Close(NULL, chan);
    }
    return status;
}

#if defined(HAVE_SD_LISTEN_FDS)
/*
 *----------------------------------------------------------------------
 *
 * PrebindSystemdSockets --
 *
 *      Obtain socket descriptors passed through systemd socket
 *      activation and add supported descriptors to the pre-bind hash
 *      tables. The value referenced by countPtr is set to the number
 *      of descriptors adopted successfully.
 *
 * Results:
 *      NS_OK when the socket-activation descriptors were obtained and
 *      adopted successfully, otherwise NS_ERROR.
 *
 * Side effects:
 *      Calls sd_listen_fds() with environment unsetting enabled.
 *      Adopted descriptors become managed by the pre-bind subsystem
 *      and are closed by NsClosePreBound() when not consumed.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
PrebindSystemdSockets(size_t *countPtr)
{
    int n;

    NS_NONNULL_ASSERT(countPtr != NULL);

    *countPtr = 0u;
    n = sd_listen_fds(1);

    if (n < 0) {
        Ns_Log(Error, "prebind: sd_listen_fds failed: %s",
               strerror(-n));
        return NS_ERROR;
    }

    for (int i = 0; i < n; i++) {
        if (PrebindRegisterSocket(SD_LISTEN_FDS_START + i) != NS_OK) {
            return NS_ERROR;
        }
        (*countPtr)++;
    }

    return NS_OK;
}

#elif defined(__APPLE__)
/*
 *----------------------------------------------------------------------
 *
 * PrebindLaunchdSockets --
 *
 *      Obtain socket descriptors supplied through launchd socket
 *      activation under the NaviServerListeners socket name and add
 *      supported descriptors to the pre-bind hash tables. The value
 *      referenced by countPtr is set to the number of descriptors
 *      adopted successfully.
 *
 * Results:
 *      NS_OK when no matching launchd socket entry exists or all
 *      supplied descriptors were adopted successfully; otherwise
 *      NS_ERROR.
 *
 * Side effects:
 *      Calls launch_activate_socket(). Adopted descriptors become
 *      managed by the pre-bind subsystem and are closed by
 *      NsClosePreBound() when not consumed.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
PrebindLaunchdSockets(size_t *countPtr)
{
    Ns_ReturnCode status = NS_OK;
    int          *fds = NULL;
    size_t        count = 0u;
    int           errorCode;

    NS_NONNULL_ASSERT(countPtr != NULL);

    *countPtr = 0u;

    errorCode = launch_activate_socket("NaviServerListeners",
                                       &fds, &count);

    if (errorCode == ESRCH || errorCode == ENOENT) {
        /*
         * The process is not managed by launchd, or the job does not
         * provide a socket entry with this name.
         */
        return NS_OK;
    }

    if (errorCode != 0) {
        Ns_Log(Error, "prebind: launch_activate_socket failed: %s",
               strerror(errorCode));
        return NS_ERROR;
    }

    for (size_t i = 0u; i < count; i++) {
        status = PrebindRegisterSocket(fds[i]);
        if (status != NS_OK) {
            break;
        }
        (*countPtr)++;
    }

    free(fds);

    return status;
}
#endif /* HAVE_SD_LISTEN_FDS || __APPLE__ */

/*
 *----------------------------------------------------------------------
 *
 * PrebindAppendSocket --
 *
 *      Allocate a Prebind structure, or enlarge an existing one, and
 *      append the specified socket. The pPtr argument may be NULL.
 *
 * Results:
 *      A pointer to the allocated or resized Prebind structure, or NULL
 *      when the allocation failed.
 *
 * Side effects:
 *      Allocates or reallocates memory. A successful reallocation may
 *      change the address of the supplied Prebind structure.
 *
 *----------------------------------------------------------------------
 */
static struct Prebind *
PrebindAppendSocket(struct Prebind *pPtr, NS_SOCKET sock)
{
    struct Prebind *newPtr;
    size_t          oldCount;

    oldCount = pPtr != NULL ? pPtr->count : 0u;

    if (pPtr == NULL) {
        newPtr = ns_malloc(PrebindSize(1u));
    } else {
        newPtr = ns_realloc(pPtr, PrebindSize(oldCount + 1u));
    }

    if (newPtr != NULL) {
        newPtr->sockets[oldCount] = sock;
        newPtr->count = oldCount + 1u;
    }

    return newPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * PrebindSockaddrKey --
 *
 *      Construct a normalized hash key from an Internet socket address.
 *      Copy only fields relevant for identifying the local endpoint and
 *      clear all remaining bytes. This ensures that equivalent
 *      addresses obtained from different operating-system interfaces
 *      produce identical fixed-size hash keys. Needed with and without
 *      the externally prebound sockets.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Stores the normalized address in the storage referenced by
 *      keyPtr.
 *
 *----------------------------------------------------------------------
 */
static void
PrebindSockaddrKey(struct NS_SOCKADDR_STORAGE *keyPtr,
                   const struct sockaddr *saPtr)
{
    NS_NONNULL_ASSERT(keyPtr != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);

    memset(keyPtr, 0, sizeof(*keyPtr));

    switch (saPtr->sa_family) {
    case AF_INET:
        {
            struct sockaddr_in src;
            struct sockaddr_in dst;

            memcpy(&src, saPtr, sizeof(src));
            memset(&dst, 0, sizeof(dst));

            dst.sin_family = src.sin_family;
            dst.sin_port = src.sin_port;
            dst.sin_addr = src.sin_addr;

            memcpy(keyPtr, &dst, sizeof(dst));
        }
        break;

    case AF_INET6:
        {
            struct sockaddr_in6 src;
            struct sockaddr_in6 dst;

            memcpy(&src, saPtr, sizeof(src));
            memset(&dst, 0, sizeof(dst));

            dst.sin6_family = src.sin6_family;
            dst.sin6_port = src.sin6_port;
            dst.sin6_addr = src.sin6_addr;
            dst.sin6_scope_id = src.sin6_scope_id;

            memcpy(keyPtr, &dst, sizeof(dst));
        }
        break;

    default:
        assert(0);
        break;
    }
}

# if defined(HAVE_SD_LISTEN_FDS) || defined(__APPLE__)
/*
 *----------------------------------------------------------------------
 *
 * PrebindRegisterInetSocket --
 *
 *      Add an already-bound Internet socket to the TCP or UDP pre-bind
 *      table, using its local socket address as the hash key. When an
 *      entry for the address already exists, append the descriptor to
 *      that entry.
 *
 * Results:
 *      NS_OK when the socket was added successfully, otherwise NS_ERROR.
 *
 * Side effects:
 *      Allocates or reallocates a Prebind structure and modifies the
 *      selected pre-bind hash table. On success, the socket becomes
 *      managed by the pre-bind subsystem.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
PrebindRegisterInetSocket(const char *proto, NS_SOCKET sock,
                          const struct sockaddr *saPtr)
{
    Tcl_HashTable   *tablePtr;
    Tcl_HashEntry   *hPtr;
    struct Prebind  *pPtr, *newPtr;
    int              isNew = 0;

    NS_NONNULL_ASSERT(proto != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);

    tablePtr = *proto == 't' ? &preboundTcp : &preboundUdp;

    hPtr = PrebindCreateHashEntry(tablePtr, saPtr, &isNew);
    pPtr = isNew != 0 ? NULL : Tcl_GetHashValue(hPtr);

    newPtr = PrebindAppendSocket(pPtr, sock);
    if (newPtr == NULL) {
        if (isNew != 0) {
            Tcl_DeleteHashEntry(hPtr);
        }
        return NS_ERROR;
    }

    Tcl_SetHashValue(hPtr, newPtr);

    Ns_LogSockaddr(Notice, "prebind: adopted inherited socket for ",
                   saPtr);
    Ns_Log(Notice, "prebind: adopted %s socket fd %d",
           proto, sock);

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PrebindRegisterSocket --
 *
 *      Inspect an already-open socket descriptor, validate its address
 *      family, type, protocol, and listening state, and register it in
 *      the appropriate pre-bind hash table. Supported descriptors are
 *      TCP and UDP Internet sockets and pathname-based Unix-domain
 *      stream sockets.
 *
 * Results:
 *      NS_OK when the descriptor was recognized and adopted, otherwise
 *      NS_ERROR.
 *
 * Side effects:
 *      Queries socket properties and, on success, modifies a pre-bind
 *      hash table. The adopted socket becomes managed by the pre-bind
 *      subsystem.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
PrebindRegisterSocket(NS_SOCKET sock)
{
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr           *saPtr = (struct sockaddr *)&sa;
    socklen_t                  saLen, optLen;
    int                        type;

    memset(&sa, 0, sizeof(sa));
    saLen = (socklen_t)sizeof(sa);

    if (getsockname(sock, saPtr, &saLen) != 0) {
        Ns_Log(Error,
               "prebind: cannot obtain address of inherited socket %d: %s",
               sock, strerror(errno));
        return NS_ERROR;
    }

    optLen = (socklen_t)sizeof(type);
    if (getsockopt(sock, SOL_SOCKET, SO_TYPE,
                   (void *)&type, &optLen) != 0) {
        Ns_Log(Error,
               "prebind: cannot obtain type of inherited socket %d: %s",
               sock, strerror(errno));
        return NS_ERROR;
    }

    /*
     * A stream descriptor supplied with Accept=no must be a listening
     * socket. A connected descriptor usually indicates Accept=yes.
     */
    if (type == SOCK_STREAM) {
        int accepting = 0;

        optLen = (socklen_t)sizeof(accepting);
        if (getsockopt(sock, SOL_SOCKET, SO_ACCEPTCONN,
                       (void *)&accepting, &optLen) == 0) {
            if (accepting == 0) {
                Ns_Log(Error,
                       "prebind: inherited stream socket %d is not listening",
                       sock);
                return NS_ERROR;
            }
        } else if (errno == ENOPROTOOPT) {
            /*
             * SO_ACCEPTCONN is defined but cannot be queried on this
             * platform. At least reject a connected stream descriptor.
             */
            struct NS_SOCKADDR_STORAGE peer;
            socklen_t                  peerLen;

            memset(&peer, 0, sizeof(peer));
            peerLen = (socklen_t)sizeof(peer);

            if (getpeername(sock, (struct sockaddr *)&peer, &peerLen) == 0) {
                Ns_Log(Error,
                       "prebind: inherited stream socket %d is connected "
                       "rather than listening",
                       sock);
                return NS_ERROR;
            }

            if (errno != ENOTCONN) {
                Ns_Log(Error,
                       "prebind: cannot determine state of inherited "
                       "stream socket %d: %s",
                       sock, strerror(errno));
                return NS_ERROR;
            }

            /*
             * ENOTCONN cannot distinguish a listening socket from an
             * unconnected stream socket. Socket-activation providers are
             * trusted to supply a passive listener.
             */
        } else {
            Ns_Log(Error,
                   "prebind: cannot inspect inherited stream socket %d: %s",
                   sock, strerror(errno));
            return NS_ERROR;
        }
    }

    switch (saPtr->sa_family) {
    case AF_INET:
    case AF_INET6:
        {
            const char *proto;

            if (type == SOCK_STREAM) {
                proto = "tcp";
            } else if (type == SOCK_DGRAM) {
                proto = "udp";
            } else {
                Ns_Log(Error,
                       "prebind: inherited Internet socket %d has "
                       "unsupported type %d",
                       sock, type);
                return NS_ERROR;
            }

#ifdef SO_PROTOCOL
            {
                int protocol;

                optLen = (socklen_t)sizeof(protocol);
                if (getsockopt(sock, SOL_SOCKET, SO_PROTOCOL,
                               (void *)&protocol, &optLen) == 0) {
                    if ((type == SOCK_STREAM && protocol != IPPROTO_TCP)
                        || (type == SOCK_DGRAM && protocol != IPPROTO_UDP)) {
                        Ns_Log(Error,
                               "prebind: inherited socket %d has "
                               "unsupported protocol %d",
                               sock, protocol);
                        return NS_ERROR;
                    }
                } else if (errno != ENOPROTOOPT) {
                    Ns_Log(Error,
                           "prebind: cannot obtain protocol of inherited "
                           "socket %d: %s",
                           sock, strerror(errno));
                    return NS_ERROR;
                }
            }
#endif

            return PrebindRegisterInetSocket(proto, sock, saPtr);
        }

#ifdef AF_UNIX
    case AF_UNIX:
        {
            const struct sockaddr_un *unPtr;
            Tcl_HashEntry            *hPtr;
            size_t                    offset, pathBytes, pathLength;
            const char               *terminator;
            char                      path[sizeof(((struct sockaddr_un *)0)->sun_path) + 1u];
            int                       isNew = 0;

            if (type != SOCK_STREAM) {
                Ns_Log(Error,
                       "prebind: inherited Unix-domain socket %d has "
                       "unsupported type %d",
                       sock, type);
                return NS_ERROR;
            }

            unPtr = (const struct sockaddr_un *)saPtr;
            offset = offsetof(struct sockaddr_un, sun_path);

            if ((size_t)saLen <= offset) {
                Ns_Log(Error,
                       "prebind: inherited Unix-domain socket %d "
                       "has no pathname",
                       sock);
                return NS_ERROR;
            }

            pathBytes = (size_t)saLen - offset;
            if (pathBytes > sizeof(unPtr->sun_path)) {
                pathBytes = sizeof(unPtr->sun_path);
            }

            /*
             * An initial NUL denotes an abstract Unix-domain socket.
             * The existing pathname-keyed table cannot represent it.
             */
            if (unPtr->sun_path[0] == '\0') {
                Ns_Log(Error,
                       "prebind: inherited abstract Unix-domain socket %d "
                       "is not supported",
                       sock);
                return NS_ERROR;
            }

            terminator = memchr(unPtr->sun_path, '\0', pathBytes);
            if (terminator != NULL) {
                pathLength = (size_t)(terminator - unPtr->sun_path);
            } else {
                pathLength = pathBytes;
            }

            memcpy(path, unPtr->sun_path, pathLength);
            path[pathLength] = '\0';

            if (Ns_PathIsAbsolute(path) != NS_TRUE) {
                Ns_Log(Error,
                       "prebind: inherited Unix-domain socket %d has "
                       "non-absolute pathname '%s'",
                       sock, path);
                return NS_ERROR;
            }

            hPtr = Tcl_CreateHashEntry(&preboundUnix, path, &isNew);
            if (isNew == 0) {
                Ns_Log(Error,
                       "prebind: duplicate inherited Unix-domain socket: %s",
                       path);
                return NS_ERROR;
            }

            Tcl_SetHashValue(hPtr, NSSOCK2PTR(sock));
            Ns_Log(Notice,
                   "prebind: adopted inherited Unix-domain socket: %s = %d",
                   path, sock);

            return NS_OK;
        }
#endif

    default:
        Ns_Log(Error,
               "prebind: inherited socket %d has unsupported "
               "address family %d",
               sock, (int)saPtr->sa_family);
        return NS_ERROR;
    }
}
# endif /* HAVE_SD_LISTEN_FDS || __APPLE__ */

/*
 *----------------------------------------------------------------------
 *
 * NsPreBind --
 *
 *      Adopt sockets supplied by a supported service manager and
 *      pre-bind sockets specified by the command-line bind arguments
 *      or bind file. Called by Ns_Main during startup before possible
 *      privilege or root-directory changes.
 *
 * Results:
 *      NS_OK on success, otherwise NS_ERROR.
 *
 * Side effects:
 *      May open or adopt sockets and store them in the pre-bind tables
 *      for later use by socket drivers.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
NsPreBind(const char *args, const char *file)
{
    Ns_ReturnCode status = NS_OK;

#ifndef _WIN32
    size_t inherited = 0u;

#if defined(HAVE_SD_LISTEN_FDS)
    status = PrebindSystemdSockets(&inherited);
#elif defined(__APPLE__)
    status = PrebindLaunchdSockets(&inherited);
#endif /* HAVE_SD_LISTEN_FDS || __APPLE__ */

    if (status != NS_OK) {
        return status;
    }

    /*
     * When inherited sockets are provided, reject traditional prebind
     * options for now and treat the two mechanisms as alternatives.
     */
    if (inherited > 0u && (args != NULL || file != NULL)) {
        Ns_Log(Error,
               "prebind: socket activation supplied %zu socket%s, but explicit "
               "prebind options were specified as well",
               inherited, inherited == 1u ? "" : "s");
        return NS_ERROR;
    }

    if (inherited == 0u) {
        if (args != NULL) {
            status = PrebindSockets(args);
        }

        if (status == NS_OK && file != NULL) {
            status = PrebindFile(file);
        }
    }
#endif /* _WIN32 */
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsClosePreBound --
 *
 *      Close remaining pre-bound sockets not consumed by anybody.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Pre-bind hash-tables are cleaned and re-initialized.
 *
 *----------------------------------------------------------------------
 */

void
NsClosePreBound(void)
{
#ifndef _WIN32
    Tcl_HashEntry         *hPtr;
    Tcl_HashSearch         search;
    NS_SOCKET              sock;
    struct sockaddr       *saPtr;

    Ns_MutexLock(&lock);

    /*
     * Close TCP sockets
     */
    hPtr = Tcl_FirstHashEntry(&preboundTcp, &search);
    while (hPtr != NULL) {
        saPtr = (struct sockaddr *)Tcl_GetHashKey(&preboundTcp, hPtr);
        PrebindCloseSockets("tcp", saPtr, Tcl_GetHashValue(hPtr));
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundTcp);
    Tcl_InitHashTable(&preboundTcp, sizeof(struct NS_SOCKADDR_STORAGE)/sizeof(int));

    /*
     * Close UDP sockets
     */
    hPtr = Tcl_FirstHashEntry(&preboundUdp, &search);
    while (hPtr != NULL) {
        saPtr = (struct sockaddr *)Tcl_GetHashKey(&preboundUdp, hPtr);
        PrebindCloseSockets("udp", saPtr, Tcl_GetHashValue(hPtr));
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundUdp);
    Tcl_InitHashTable(&preboundUdp, sizeof(struct NS_SOCKADDR_STORAGE)/sizeof(int));

    /*
     * Close raw sockets
     */
    hPtr = Tcl_FirstHashEntry(&preboundRaw, &search);
    while (hPtr != NULL) {
        int port;

        sock = PTR2NSSOCK(Tcl_GetHashKey(&preboundRaw, hPtr));
        port = PTR2INT(Tcl_GetHashValue(hPtr));
        Ns_Log(Warning, "prebind: closed unused raw socket: %d = %d",
               port, sock);
        (void)ns_sockclose(sock);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundRaw);
    Tcl_InitHashTable(&preboundRaw, TCL_ONE_WORD_KEYS);

    /*
     * Close Unix-domain sockets
     */
    hPtr = Tcl_FirstHashEntry(&preboundUnix, &search);
    while (hPtr != NULL) {
        const char *addr = (char *) Tcl_GetHashKey(&preboundUnix, hPtr);

        sock = PTR2NSSOCK(Tcl_GetHashValue(hPtr));
        Ns_Log(Warning, "prebind: closed unused Unix-domain socket: [%s] %d",
               addr, sock);
        (void)ns_sockclose(sock);
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&preboundUnix);
    Tcl_InitHashTable(&preboundUnix, TCL_STRING_KEYS);

    Ns_MutexUnlock(&lock);
#endif /* _WIN32 */
}


/*
 *----------------------------------------------------------------------
 *
 * PreBind --
 *
 *      Pre-bind to one or more ports in a comma-separated list:
 *
 *          addr:port[/protocol][#number]
 *          port[/protocol][#number]
 *          0/icmp[/count]
 *          /path[|mode]
 *
 *       protocol: tcp|udp
 *       mode: mode bits as used by "chmod" specified as octal value
 *
 *      Example: nsd -c -b /tmp/foo,localhost:9999/tcp#2,localhost:9998,udp:9997
 * Results:
 *      None.
 *
 * Side effects:
 *      Sockets are left in bound state for later listen
 *      in Ns_SockListen*().
 *
 *----------------------------------------------------------------------
 */
#ifndef _WIN32

static Ns_ReturnCode
PrebindSockets(const char *spec)
{
    Tcl_HashEntry         *hPtr;
    int                    isNew = 0, specCount = 0;
    char                  *next, *line, *lines;
    Ns_ReturnCode          status = NS_OK;
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr       *saPtr = (struct sockaddr *)&sa;

    NS_NONNULL_ASSERT(spec != NULL);

    line = lines = ns_strdup(spec);

    for (; line != NULL; line = next) {
        const char     *proto;
        char           *p, *str = NULL, *end;
        const char     *addr;
        unsigned short  port = 0u;
        long            reuses;
        struct Prebind *pPtr;

        specCount ++;
        /*
         * Find the next comma separated token.
         */
        next = strchr(line, INTCHAR(','));
        if (next != NULL) {
            *next++ = '\0';
        }

        /*
         * Set default proto and addr.
         */
        proto = "*";
        addr = NS_IP_UNSPECIFIED;
        reuses = 1;

        /*
         * Parse reuses count
         */
        p = strrchr(line, INTCHAR('#'));
        if (p != NULL) {
            *p++ = '\0';
            reuses = strtol(p, NULL, 10);
            if (reuses < 1) {
                Ns_Log(Warning, "prebind: ignore invalid number of protoport reuses: '%s'", p);
                reuses = 1;
            }
        }

        /*
         * Parse "addr:port" or "port"
         *
         *    addr:port[/protocol][#number]
         *    port[/protocol][#number]
         *    0/icmp[/count]
         */
        {
            const char *portStr;
            bool        hostParsedOk = Ns_HttpParseHost2(line, NS_TRUE, &addr, &portStr, &end);

            if (hostParsedOk && line != end && addr != portStr ) {
                long l;

                if (portStr != NULL) {
                    l = strtol(portStr, NULL, 10);
                } else {
                    assert(addr != NULL);
                    l = strtol(addr, NULL, 10);
                    addr = NS_IP_UNSPECIFIED;
                }
                port = (l >= 0) ? (unsigned short)l : 0u;

                line = end;
                /*
                 * Parse protocol
                 */
                if (*line == '/') {
                    *line++ = '\0';
                    proto = line;
                }
            } else {
                line = end;
                Ns_Log(Debug, "prebind: line <%s> was not parsed ok, must be UNIX", line);
                proto = "unix";
            }
        }

        /*
         * TCP
         */
#ifdef LOGBIND
        log_bind(proto, addr, port, "try add entry");
#endif

        Ns_Log(Notice, "prebind: try proto %s addr %s port %d reuses %ld",
               proto, addr, port, reuses);

        if ((STREQ(proto, "tcp") || *proto == '*') && port > 0) {
            if (Ns_GetSockAddr(saPtr, addr, port) != NS_OK) {
                Ns_Log(Error, "prebind: tcp: invalid address: [%s]:%d", addr, port);
                continue;
            }

            hPtr = PrebindCreateHashEntry(&preboundTcp, saPtr, &isNew);
            if (isNew == 0) {
                Ns_Log(Error, "prebind: tcp: duplicate entry: [%s]:%d",
                       addr, port);
                continue;
            }

            Ns_LogSockaddr(Notice, "prebind adds", (const struct sockaddr *)saPtr);

            pPtr = PrebindAlloc("tcp", (size_t)reuses, saPtr);
            if (pPtr == NULL) {
                Tcl_DeleteHashEntry(hPtr);
                status = NS_ERROR;
                break;
            }
            Tcl_SetHashValue(hPtr, pPtr);
            Ns_Log(Notice, "prebind: tcp: [%s]:%d", addr, port);
        }

        /*
         * UDP
         */
        if ((STREQ(proto, "udp") || *proto == '*')  && port > 0) {
            if (Ns_GetSockAddr(saPtr, addr, port) != NS_OK) {
                Ns_Log(Error, "prebind: udp: invalid address: [%s]:%d",
                       addr, port);
                continue;
            }

            hPtr = PrebindCreateHashEntry(&preboundUdp, saPtr, &isNew);
            if (isNew == 0) {
                Ns_Log(Error, "prebind: udp: duplicate entry: [%s]:%d",
                       addr, port);
                continue;
            }
            pPtr = PrebindAlloc("udp", (size_t)reuses, saPtr);
            if (pPtr == NULL) {
                Tcl_DeleteHashEntry(hPtr);
                status = NS_ERROR;
                break;
            }
            Tcl_SetHashValue(hPtr, pPtr);
            Ns_Log(Notice, "prebind: udp: [%s]:%d", addr, port);
        }

        /*
         * ICMP
         *
         * Example:
         *   0/icmp[/count]
         */
        if (strncmp(proto, "icmp", 4u) == 0
            && (proto[4] == '\0' || proto[4] == '/')) {
            long        count = 1;
            const char *countStr;

            /*
             * Parse optional count.
             */
            countStr = strchr(proto, INTCHAR('/'));
            if (countStr != NULL) {
                char *countEnd;

                count = strtol(countStr + 1, &countEnd, 10);
                if (countEnd == countStr + 1 || *countEnd != '\0' || count < 1) {
                    Ns_Log(Warning, "prebind: ignore invalid icmp count: '%s'", countStr + 1);
                    count = 1;
                }
            }

            while (count-- > 0) {
                NS_SOCKET sock = Ns_SockBindRaw(IPPROTO_ICMP);

                if (sock == NS_INVALID_SOCKET) {
                    Ns_Log(Error, "prebind: bind error for icmp: %s", strerror(errno));
                    continue;
                }
                hPtr = Tcl_CreateHashEntry(&preboundRaw, NSSOCK2PTR(sock), &isNew);
                if (isNew == 0) {
                    Ns_Log(Error, "prebind: icmp: duplicate entry");
                    (void)ns_sockclose(sock);
                    continue;
                }
                Tcl_SetHashValue(hPtr, IPPROTO_ICMP);
                Ns_Log(Notice, "prebind: icmp: %d", sock);
            }
        }

        /*
         * Unix-domain socket
         * a line starting with a '/' means: path, which
         * implies a unix-domain socket.
         */
        if (STREQ(proto, "unix")) {
            if (Ns_PathIsAbsolute(line) == NS_TRUE) {
                unsigned short mode = 0u;
                NS_SOCKET      sock;

                Ns_Log(Debug, "prebind: Unix-domain socket <%s>\n", line);

                /*
                 * Parse mode
                 */
                str = strchr(line, INTCHAR('|'));
                if (str != NULL) {
                    long l;

                    *(str++) = '\0';
                    l = strtol(str, NULL, 8);
                    if (l > 0) {
                        mode = (unsigned short)l;
                    } else {
                        Ns_Log(Error, "prebind: unix: ignore invalid mode value: %s", line);
                    }
                }
                hPtr = Tcl_CreateHashEntry(&preboundUnix, (char *) line, &isNew);
                if (isNew == 0) {
                    Ns_Log(Error, "prebind: unix: duplicate entry: %s", line);
                    continue;
                }
                sock = Ns_SockBindUnix(line, SOCK_STREAM, mode);
                if (sock == NS_INVALID_SOCKET) {
                    Ns_Log(Error, "prebind: unix: %s: %s", proto, strerror(errno));
                    Tcl_DeleteHashEntry(hPtr);
                    continue;
                }
                Tcl_SetHashValue(hPtr, NSSOCK2PTR(sock));
                Ns_Log(Notice, "prebind: unix: %s = %d", line, sock);
            } else {
                Ns_Log(Warning, "prebind: invalid entry #%d: '%s'", specCount, spec);
            }
        }
    }
    ns_free(lines);

    return status;
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockBinderListen --
 *
 *      Create a new TCP/UDP/Unix socket bound to the specified port
 *      and listening for new connections.
 *
 *      The following types are defined:
 *      T - TCP socket
 *      U - UDP socket
 *      D - Unix domain socket
 *      R - raw socket
 *
 * Results:
 *      Socket descriptor or -1 on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockBinderListen(char type, const char *address, unsigned short port, int options)
{
    NS_SOCKET     sock = NS_INVALID_SOCKET;
#ifndef _WIN32
    ns_sockerrno_t err = 0;
    ssize_t       n;
    char          data[NS_IPADDR_SIZE];
    struct msghdr msg;
    struct iovec  iov[4];

    if (address == NULL) {
        address = NS_IP_UNSPECIFIED;
    }
    strncpy(data, address, sizeof(data)-1);

    /*
     * Build and send message.
     */
    ns_iov_set(&iov[0], &options, sizeof(options));
    ns_iov_set(&iov[1], &port,    sizeof(port));
    ns_iov_set(&iov[2], &type,    sizeof(type));
    ns_iov_set(&iov[3], &data,    sizeof(data));

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 4;
    n = sendmsg(binderRequest[1], &msg, 0);
    if (n != REQUEST_SIZE) {
        Ns_Log(Error, "Ns_SockBinderListen: sendmsg() failed: sent %" PRIdz " bytes, '%s'",
               n, strerror(errno));
        return -1;
    }

    /*
     * Receive reply.
     */
    ns_iov_set(&iov[0], &err, sizeof(err));
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
#ifdef HAVE_CMMSG
    msg.msg_control = (void *) data;
    msg.msg_controllen = sizeof(data);
#else
    msg.msg_accrights = (void*) &sock;
    msg.msg_accrightslen = sizeof(sock);
#endif
    n = recvmsg(binderResponse[0], &msg, 0);
    if (n != RESPONSE_SIZE) {
        Ns_Log(Error, "Ns_SockBinderListen: recvmsg() failed: recv %" PRIdz " bytes, '%s'",
               n, strerror(errno));
        return -1;
    }

#ifdef HAVE_CMMSG
    {
      struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
      if ((c != NULL) && c->cmsg_type == SCM_RIGHTS) {
          int *ptr;
          /*
           * Use memcpy to avoid alignment problems.
           */
          memcpy(&ptr, CMSG_DATA(c), sizeof(int*));
          sock = *ptr;
      }
    }
#endif

    /*
     * Close-on-exec, while set in the binder process by default
     * with Ns_SockBind, is not transmitted in the sendmsg and
     * must be set again.
     */

    if (sock != NS_INVALID_SOCKET && Ns_CloseOnExec(sock) != NS_OK) {
        (void)ns_sockclose(sock);
        sock = NS_INVALID_SOCKET;
    }
    if (err == 0) {
        Ns_Log(Notice, "Ns_SockBinderListen: listen(%s,%hu) = %d",
               address, port, sock);
    } else {
        Ns_SetSockErrno(err);
        sock = NS_INVALID_SOCKET;
        Ns_Log(Error, "Ns_SockBinderListen: listen(%s,%hu) failed: '%s'",
               address, port, ns_sockstrerror(ns_sockerrno));
    }
#endif /* _WIN32 */
    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * NsForkBinder --
 *
 *      Fork of the bind/listen process.  This routine is called
 *      by main() when the server starts as root.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *
 *      The binderRunning, binderRequest, binderResponse static
 *      variables are updated.
 *
 *----------------------------------------------------------------------
 */

void
NsForkBinder(void)
{
#ifndef _WIN32
    pid_t pid1;
    int status = 0;

    /*
     * Create two socket pipes, one for sending the request and one for
     * receiving the response.
     */

    if (ns_sockpair(binderRequest) != 0 || ns_sockpair(binderResponse) != 0) {
        Ns_Fatal("NsForkBinder: ns_sockpair() failed: '%s'", strerror(errno));
    }

    /*
     * Double-fork and run as a binder until the socket pairs are
     * closed.  The server double forks to avoid problems waiting for a
     * child root process after the parent does a setuid(), something
     * which appears to confuse the process-based Linux and SGI threads.
     */

    pid1 = ns_fork();
    if (pid1 < 0) {
        Ns_Fatal("NsForkBinder: fork() failed: '%s'", strerror(errno));

    } else if (pid1 == 0) {
        pid_t pid2;

        pid2 = ns_fork();
        if (pid2 < 0) {
            Ns_Fatal("NsForkBinder: fork() failed: '%s'", strerror(errno));
        } else if (pid2 == 0) {
            /*
             * Grandchild process.
             */
            (void)ns_sockclose(binderRequest[1]);
            (void)ns_sockclose(binderResponse[0]);
            Binder();
        } else {
            /*
             * Child process.
             */
        }
        exit(0);

    } else {
        /*
         * Parent process.
         */
        if (Ns_WaitForProcess(pid1, &status) != NS_OK) {
            Ns_Fatal("NsForkBinder: Ns_WaitForProcess(%d) failed: '%s'",
                     pid1, strerror(errno));
        } else if (status != 0) {
            Ns_Fatal("NsForkBinder: process %d exited with nonzero status: %d",
                     pid1, status);
        }
        binderRunning = NS_TRUE;
    }
#endif /* _WIN32 */
}


/*
 *----------------------------------------------------------------------
 *
 * NsStopBinder --
 *
 *      Close the socket to the binder after startup.  This is done
 *      to avoid a possible security risk of binding to privileged
 *      ports after startup.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Binder process will exit.
 *
 *----------------------------------------------------------------------
 */

void
NsStopBinder(void)
{
    if (binderRunning) {
        (void)ns_sockclose(binderRequest[1]);
        (void)ns_sockclose(binderResponse[0]);
        (void)ns_sockclose(binderRequest[0]);
        (void)ns_sockclose(binderResponse[1]);
        binderRunning = NS_FALSE;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Binder --
 *
 *      Child process bind/listen loop.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sockets are created and sent to the parent on request.
 *
 *----------------------------------------------------------------------
 */

#ifndef _WIN32
static void
Binder(void)
{
    int            options, err, sock;
    unsigned short port;
    ssize_t        n;
    char           type, address[NS_IPADDR_SIZE];
    struct msghdr  msg;
    struct iovec   iov[4];

#ifdef HAVE_CMMSG
    struct cmsghdr *c;
#endif

    Ns_Log(Notice, "binder: started");
    Ns_ThreadSetName("binder");

    /*
     * Endlessly listen for socket bind requests.
     */

    for (;;) {
        /*
         * Receive a message with the following contents.
         */
        ns_iov_set(&iov[0], &options, sizeof(options));
        ns_iov_set(&iov[1], &port,    sizeof(port));
        ns_iov_set(&iov[2], &type,    sizeof(type));
        ns_iov_set(&iov[3], &address, sizeof(address));

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = 4;
        options = 0;
        port = 0u;
        type = '\0';
        err = 0;
        do {
            n = recvmsg(binderRequest[0], &msg, 0);
        } while (n == -1 && errno == NS_EINTR);
        if (n == 0) {
            break;
        }
        if (n != REQUEST_SIZE) {
            Ns_Fatal("binder: recvmsg() failed: recv %" PRIdz " bytes, '%s'", n, strerror(errno));
        }

        /*
         * NB: Due to a bug in Solaris the child process must
         * call both bind() and listen() before returning the
         * socket.  All other Unix versions would actually allow
         * just performing the bind() in the child and allowing
         * the parent to perform the listen().
         */
        switch (type) {
        case 'U':
            sock = Ns_SockListenUdp(address, port, NS_FALSE);
            break;
        case 'D':
            sock = Ns_SockListenUnix(address, options, port);
            break;
        case 'R':
            sock = Ns_SockListenRaw(options);
            break;
        case 'T':
        default:
            sock = Ns_SockListenEx(address, port, options, NS_FALSE);
        }
        Ns_Log(Notice, "bind type %c addr %s port %d options %d to socket %d",
               type, address, port, options, sock);

        if (sock < 0) {
            err = errno;
        }

        ns_iov_set(&iov[0], &err, sizeof(err));
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = 1;

        if (sock != -1) {
#ifdef HAVE_CMMSG
            int *pfd;

            msg.msg_control = address;
            msg.msg_controllen = sizeof(address);
            c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type  = SCM_RIGHTS;
            /*
             * Use memcpy to avoid alignment problems.
             */
            memcpy(&pfd, CMSG_DATA(c), sizeof(int*));

            *pfd = sock;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            msg.msg_controllen = c->cmsg_len;
#else
            msg.msg_accrights = (void*) &sock;
            msg.msg_accrightslen = sizeof(sock);
#endif
        }

        do {
            n = sendmsg(binderResponse[1], &msg, 0);
        } while (n == -1 && errno == NS_EINTR);
        if (n != RESPONSE_SIZE) {
            Ns_Fatal("binder: sendmsg() failed: sent %" PRIdz " bytes, '%s'", n, strerror(errno));
        }
        if (sock >= -1) {
            /*
             * Close the socket as it won't be needed in the child.
             */
            (void)ns_sockclose(sock);
        }
    }
    Ns_Log(Notice, "binder: stopped");
}
#endif /* _WIN32 */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 72
 * indent-tabs-mode: nil
 * End:
 */
