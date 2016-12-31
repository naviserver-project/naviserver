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
 * binder.c --
 *
 * Support for pre-bound privileged ports for Unix
 */

#include "nsd.h"

#ifndef _WIN32
# include <sys/un.h>
# include <sys/uio.h>

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
static Ns_ReturnCode PrebindSockets(const char *line)
    NS_GNUC_NONNULL(1);

static void Binder(void);

static struct Prebind* PrebindAlloc(const char *proto, size_t reuses, struct sockaddr *saPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static bool PrebindGet(const char *proto, struct sockaddr *saPtr, NS_SOCKET *sockPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void PrebindCloseSockets(const char *proto, struct sockaddr *saPtr, struct Prebind *pPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
#endif


#ifndef _WIN32

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
    bool            reuseport;
    size_t          i;

    NS_NONNULL_ASSERT(proto != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);

    pPtr = ns_malloc(sizeof(Prebind) + sizeof(NS_SOCKET)*reuses-1);
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
    static Tcl_HashTable *tablePtr;
    Tcl_HashEntry        *hPtr;
    bool                  foundEntry = NS_FALSE;

    NS_NONNULL_ASSERT(proto != NULL);
    NS_NONNULL_ASSERT(saPtr != NULL);
    NS_NONNULL_ASSERT(sockPtr != NULL);

    if (*proto == 't') {
        tablePtr = &preboundTcp;
    } else {
        tablePtr = &preboundUdp;
    }

    Ns_MutexLock(&lock);
    hPtr = Tcl_FindHashEntry(tablePtr, (char *)saPtr);
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
            ns_sockclose(sock);
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
        } else {
            //fprintf(stderr, "listen on port %hd already prebound\n", port);
        }

        if (sock != NS_INVALID_SOCKET && listen(sock, backlog) == -1) {
            /*
             * Can't listen; close the opened socket
             */
            int err = errno;

            ns_sockclose(sock);
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
    if (sock == NS_INVALID_SOCKET && binderRunning && saPtr != NULL) {
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
    NS_SOCKET        sock = NS_INVALID_SOCKET;
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr *saPtr = (struct sockaddr *)&sa;

    if (Ns_GetSockAddr(saPtr, address, port) == NS_OK) {
        bool           found;

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
    }

    /*
     * If forked binder is running and we could not allocate socket
     * directly, try to do it through the binder
     */

    if (sock == NS_INVALID_SOCKET && binderRunning) {
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
        int err = errno;

        ns_sockclose(sock);
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
    int       n = 1;

    NS_NONNULL_ASSERT(saPtr != NULL);

    sock = socket(saPtr->sa_family, SOCK_DGRAM, 0);

    if (sock == NS_INVALID_SOCKET
        || setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&n, sizeof(n)) == -1
        || setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&n, sizeof(n)) == -1
        || bind(sock, saPtr, Ns_SockaddrGetSockLen(saPtr)) == -1) {
        int err = errno;

        ns_sockclose(sock);
        sock = NS_INVALID_SOCKET;
        Ns_SetSockErrno(err);
    } else {
#if defined(SO_REUSEPORT)
        if (reusePort) {
            int optval = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
        }
#endif
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
    NS_SOCKET sock;
    struct sockaddr_un addr;
    size_t pathLength;

    NS_NONNULL_ASSERT(path != NULL);
    pathLength = strlen(path);

    if (pathLength >= sizeof(addr.sun_path)) {
        Ns_Log(Error, "provided path exeeds maximum length: %s\n", path);
        return NS_INVALID_SOCKET;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, pathLength + 1);
    unlink(path);

    sock = socket(AF_UNIX, socktype > 0 ? socktype : SOCK_STREAM, 0);

    if (sock == NS_INVALID_SOCKET
        || bind(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1
        || (mode && chmod(path, mode) == -1)) {
        int err = errno;

        ns_sockclose(sock);
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

    sock = socket(AF_INET, SOCK_RAW, proto);

    if (sock == NS_INVALID_SOCKET) {
        int err = errno;

        ns_sockclose(sock);
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
 * NsPreBind --
 *
 *      Pre-bind any requested ports (called from Ns_Main at startup).
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May pre-bind to one or more ports.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
NsPreBind(const char *args, const char *file)
{
    Ns_ReturnCode status = NS_OK;

#ifndef _WIN32

    if (args != NULL) {
        status = PrebindSockets(args);
    }

    /*
     * Check, if the bind options were provided via file. If so, parse
     * and interprete it.
     */
    if (status == NS_OK && file != NULL) {
        Tcl_Channel chan = Tcl_OpenFileChannel(NULL, file, "r", 0);

        if (chan == NULL) {
            Ns_Log(Error, "NsPreBind: can't open file '%s': '%s'", file,
                   strerror(Tcl_GetErrno()));
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
        ns_sockclose(sock);
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
        ns_sockclose(sock);
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
 * Results:
 *      None.
 *
 * Side effects:
 *      Sockets are left in bound state for later listen
 *      in Ns_SockListenXXX.
 *
 *----------------------------------------------------------------------
 */
#ifndef _WIN32

static Ns_ReturnCode
PrebindSockets(const char *spec)
{
    Tcl_HashEntry         *hPtr;
    int                    isNew, sock;
    char                  *next, *str, *line, *lines;
    long                   l;
    Ns_ReturnCode          status = NS_OK;
    struct NS_SOCKADDR_STORAGE  sa;
    struct sockaddr       *saPtr = (struct sockaddr *)&sa;

    NS_NONNULL_ASSERT(spec != NULL);

    line = lines = ns_strdup(spec);
    Ns_Log(Notice, "trying to prebind <%s>", line);

    for (; line != NULL; line = next) {
        const char     *proto;
        char           *addr, *p;
        unsigned short  port;
        long            reuses;
        struct Prebind *pPtr;

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
        proto = "tcp";
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
	 * Parse port
	 */
        Ns_HttpParseHost(line, &addr, &str);
        if (str != NULL) {
            *str++ = '\0';
            l = strtol(str, NULL, 10);
            line = str;
        } else {
            l = strtol(addr, NULL, 10);
            addr = NS_IP_UNSPECIFIED;
        }
        port = (l >= 0) ? (unsigned short)l : 0u;

        /*
	 * Parse protocol; a line starting with a '/' means: path, which
	 * implies a unix-domain socket.
	 */
        if (*line != '/' && (str = strchr(line, INTCHAR('/')))) {
            *str++ = '\0';
            proto = str;
        }

	/*
	 * TCP
	 */
        Ns_Log(Notice, "prebind: proto %s addr %s port %d reuses %ld", proto, addr, port, reuses);

        if (STREQ(proto, "tcp") && port > 0) {
            if (Ns_GetSockAddr(saPtr, addr, port) != NS_OK) {
                Ns_Log(Error, "prebind: tcp: invalid address: [%s]:%d", addr, port);
                continue;
            }
            hPtr = Tcl_CreateHashEntry(&preboundTcp, (char *) &sa, &isNew);
            if (isNew == 0) {
                Ns_Log(Error, "prebind: tcp: duplicate entry: [%s]:%d",
                       addr, port);
                continue;
            }

            Ns_LogSockaddr(Notice, "prebind adds", (const struct sockaddr *)saPtr);

            pPtr = PrebindAlloc(proto, (size_t)reuses, saPtr);
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
        if (STREQ(proto, "udp") && port > 0) {
            if (Ns_GetSockAddr(saPtr, addr, port) != NS_OK) {
                Ns_Log(Error, "prebind: udp: invalid address: [%s]:%d",
                       addr, port);
                continue;
            }
            hPtr = Tcl_CreateHashEntry(&preboundUdp, (char *)saPtr, &isNew);
            if (isNew == 0) {
                Ns_Log(Error, "prebind: udp: duplicate entry: [%s]:%d",
                       addr, port);
                continue;
            }
            pPtr = PrebindAlloc(proto, (size_t)reuses, saPtr);
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
	 */
        if (strncmp(proto, "icmp", 4u) == 0) {
            long count = 1;
            /* Parse count */

            str = strchr(str, INTCHAR('/'));
            if (str != NULL) {
                *(str++) = '\0';
                count = strtol(str, NULL, 10);
            }
            while (count--) {
                sock = Ns_SockBindRaw(IPPROTO_ICMP);
                if (sock == NS_INVALID_SOCKET) {
                    Ns_Log(Error, "prebind: bind error for icmp: %s",strerror(errno));
                    continue;
                }
                hPtr = Tcl_CreateHashEntry(&preboundRaw, NSSOCK2PTR(sock), &isNew);
                if (isNew == 0) {
                    Ns_Log(Error, "prebind: icmp: duplicate entry");
                    ns_sockclose(sock);
                    continue;
                }
                Tcl_SetHashValue(hPtr, IPPROTO_ICMP);
                Ns_Log(Notice, "prebind: icmp: %d", sock);
            }
        }

	/*
	 * Unix-domain socket
	 */
        if (Ns_PathIsAbsolute(line) == NS_TRUE) {
            unsigned short mode = 0u;
            /* Parse mode */

            str = strchr(str, INTCHAR('|'));
            if (str != NULL) {
                *(str++) = '\0';
                l = strtol(str, NULL, 10);
                if (l > 0) {
                    mode = (unsigned short)l;
                }
            }
            hPtr = Tcl_CreateHashEntry(&preboundUnix, (char *) line, &isNew);
            if (isNew == 0) {
                Ns_Log(Error, "prebind: unix: duplicate entry: %s",line);
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
    int           err;
    ssize_t       n;
    char          data[NS_IPADDR_SIZE];
    struct msghdr msg;
    struct iovec  iov[4];

    if (address == NULL) {
        address = NS_IP_UNSPECIFIED;
    }

    /*
     * Build and send message.
     */
    iov[0].iov_base = (caddr_t) &options;
    iov[0].iov_len = sizeof(options);
    iov[1].iov_base = (caddr_t) &port;
    iov[1].iov_len = sizeof(port);
    iov[2].iov_base = (caddr_t) &type;
    iov[2].iov_len = sizeof(type);
    iov[3].iov_base = (caddr_t) data;
    iov[3].iov_len = sizeof(data);

    strncpy(data, address, sizeof(data)-1);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 4;
    n = sendmsg(binderRequest[1], (struct msghdr *) &msg, 0);
    if (n != REQUEST_SIZE) {
        Ns_Log(Error, "Ns_SockBinderListen: sendmsg() failed: sent %" PRIdz " bytes, '%s'",
               n, strerror(errno));
        return -1;
    }

    /*
     * Reveive reply.
     */
    iov[0].iov_base = (caddr_t) &err;
    iov[0].iov_len = sizeof(int);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
#ifdef HAVE_CMMSG
    msg.msg_control = (void *) data;
    msg.msg_controllen = sizeof(data);
#else
    msg.msg_accrights = (caddr_t) &sock;
    msg.msg_accrightslen = sizeof(sock);
#endif
    n = recvmsg(binderResponse[0], (struct msghdr *) &msg, 0);
    if (n != RESPONSE_SIZE) {
        Ns_Log(Error, "Ns_SockBinderListen: recvmsg() failed: recv %" PRIdz " bytes, '%s'",
               n, strerror(errno));
        return -1;
    }

#ifdef HAVE_CMMSG
    {
      struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
      if ((c != NULL) && c->cmsg_type == SCM_RIGHTS) {
	  int *ptr = (int*)CMSG_DATA(c);
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
        ns_sockclose(sock);
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
 *      Fork of the slave bind/listen process.  This routine is called
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
    pid_t pid;
    int status;

    /*
     * Create two socket pipes, one for sending the request and one
     * for receiving the response.
     */

    if (ns_sockpair(binderRequest) != 0 || ns_sockpair(binderResponse) != 0) {
        Ns_Fatal("NsForkBinder: ns_sockpair() failed: '%s'", strerror(errno));
    }

    /*
     * Double-fork and run as a binder until the socket pairs are
     * closed.  The server double forks to avoid problems
     * waiting for a child root process after the parent does a
     * setuid(), something which appears to confuse the
     * process-based Linux and SGI threads.
     */

    pid = ns_fork();
    if (pid < 0) {
        Ns_Fatal("NsForkBinder: fork() failed: '%s'", strerror(errno));
    } else if (pid == 0) {
        pid = ns_fork();
        if (pid < 0) {
            Ns_Fatal("NsForkBinder: fork() failed: '%s'", strerror(errno));
        } else if (pid == 0) {
            ns_sockclose(binderRequest[1]);
            ns_sockclose(binderResponse[0]);
            Binder();
        }
        exit(0);
    }
    if (Ns_WaitForProcess(pid, &status) != NS_OK) {
        Ns_Fatal("NsForkBinder: Ns_WaitForProcess(%d) failed: '%s'",
                 pid, strerror(errno));
    } else if (status != 0) {
        Ns_Fatal("NsForkBinder: process %d exited with non-zero status: %d",
                 pid, status);
    }
    binderRunning = NS_TRUE;
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
        ns_sockclose(binderRequest[1]);
        ns_sockclose(binderResponse[0]);
        ns_sockclose(binderRequest[0]);
        ns_sockclose(binderResponse[1]);
        binderRunning = NS_FALSE;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Binder --
 *
 *      Slave process bind/listen loop.
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
        iov[0].iov_base = (caddr_t) &options;
        iov[0].iov_len = sizeof(options);
        iov[1].iov_base = (caddr_t) &port;
        iov[1].iov_len = sizeof(port);
        iov[2].iov_base = (caddr_t) &type;
        iov[2].iov_len = sizeof(type);
        iov[3].iov_base = (caddr_t) address;
        iov[3].iov_len = sizeof(address);
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = iov;
        msg.msg_iovlen = 4;
        type = '\0';
        err = 0;
        do {
            n = recvmsg(binderRequest[0], (struct msghdr *) &msg, 0);
        } while (n == -1 && errno == EINTR);
        if (n == 0) {
            break;
        }
        if (n != REQUEST_SIZE) {
            Ns_Fatal("binder: recvmsg() failed: recv %" PRIdz " bytes, '%s'", n, strerror(errno));
        }

        /*
         * NB: Due to a bug in Solaris the slave process must
         * call both bind() and listen() before returning the
         * socket.  All other Unix versions would actually allow
         * just performing the bind() in the slave and allowing
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

        iov[0].iov_base = (caddr_t) &err;
        iov[0].iov_len = sizeof(err);
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
            pfd = (int*)CMSG_DATA(c);
            *pfd = sock;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            msg.msg_controllen = c->cmsg_len;
#else
            msg.msg_accrights = (caddr_t) &sock;
            msg.msg_accrightslen = sizeof(sock);
#endif
        }

        do {
            n = sendmsg(binderResponse[1], (struct msghdr *) &msg, 0);
        } while (n == -1 && errno == EINTR);
        if (n != RESPONSE_SIZE) {
            Ns_Fatal("binder: sendmsg() failed: sent %" PRIdz " bytes, '%s'", n, strerror(errno));
        }
        if (sock != -1) {
            /*
             * Close the socket as it won't be needed in the slave.
             */
            ns_sockclose(sock);
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
