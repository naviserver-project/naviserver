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
 * listen.c --
 *
 *      Listen on sockets and register callbacks for incoming
 *      connections.
 */

#include "nsd.h"

/*
 * This the context used by the socket callback.
 */

typedef struct ListenData {
    Ns_SockProc *proc;
    void        *arg;
} ListenData;

/*
 * Local functions defined in this file
 */

static Ns_SockProc  ListenCallback;

/*
 * Static variables defined in this file
 */

static Tcl_HashTable portsTable;      /* Table of per-port data. */
static Ns_Mutex      lock = NULL;     /* Lock around portsTable. */


/*
 *----------------------------------------------------------------------
 *
 * NsInitListen --
 *
 *      Initialize listen callback API.
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
NsInitListen(void)
{
    Ns_MutexInit(&lock);
    Ns_MutexSetName(&lock, "ns:listencallbacks");
    Tcl_InitHashTable(&portsTable, TCL_ONE_WORD_KEYS);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockListenCallback --
 *
 *      Listen on an address/port and register a callback to be run
 *      when connections come in on it.
 *
 * Results:
 *      A valid NS_SOCK or  NS_INVALID_SOCKET
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_SockListenCallback(const char *addr, unsigned short port, Ns_SockProc *proc, bool bind, void *arg)
{
    NS_SOCKET             sock = NS_INVALID_SOCKET;
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr      *saPtr = (struct sockaddr *)&sa;

    NS_NONNULL_ASSERT(proc != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    Ns_Log(Debug, "Ns_SockListenCallback: called with addr <%s> and port %hu", addr, port);

    if (Ns_GetSockAddr(saPtr, addr, port) == NS_OK) {
        NS_SOCKET     bindsock;
        Ns_ReturnCode status = NS_OK;

        bindsock = Ns_SockBind(saPtr, NS_FALSE);

        if (port == 0u) {
            /*
             * The specified port might be 0 to allocate a fresh, unused
             * port for listening. Ns_SockBind() will update the port in
             * the saPtr, update the local variable here to have a valid
             * entry for the hash table below.
             */
            socklen_t slen = sizeof(sa);
            if (getsockname(bindsock, (struct sockaddr *)&saPtr, &slen) == 0) {
                port = Ns_SockaddrGetPort(saPtr);
                Ns_Log(Debug, "Ns_SockListenCallback: Ns_GetSockAddr kernel assigned port %hu", port);
            } else {
                Ns_Log(Warning, "getsockname failed on ephemeral bind: %s",
                       ns_sockstrerror(ns_sockerrno));
                if (bindsock != NS_INVALID_SOCKET) {
                    ns_sockclose(bindsock);
                    bindsock = NS_INVALID_SOCKET;
                }
                status = NS_ERROR;
            }
        }

        if (!bind) {
            /*
             * Just make sure, we can bind to the specified interface.
             */
            if (likely(bindsock != NS_INVALID_SOCKET)) {
                ns_sockclose(bindsock);
                bindsock = NS_INVALID_SOCKET;
            } else {
                sock = NS_INVALID_SOCKET;
                status = NS_ERROR;
            }
        }

        if (status == NS_OK) {
            Tcl_HashTable        *tablePtr = NULL;
            Tcl_HashEntry        *hPtr;
            int                   isNew;

            Ns_Log(Debug, "Ns_SockListenCallback: registering port %hu", port);

            Ns_MutexLock(&lock);

            /*
             * Update the global hash table that keeps track of which ports
             * we're listening on.
             */
            hPtr = Tcl_CreateHashEntry(&portsTable, INT2PTR(port), &isNew);
            if (isNew == 0) {
                tablePtr = Tcl_GetHashValue(hPtr);
            } else {

                if (bindsock != NS_INVALID_SOCKET) {
                    listen(bindsock, 5);
                    sock = bindsock;
                } else {
                    sock = Ns_SockListen(NULL, port);
                }

                if (sock == NS_INVALID_SOCKET) {
                    Tcl_DeleteHashEntry(hPtr);
                    status = NS_ERROR;
                } else {
                    status = Ns_SockSetNonBlocking(sock);
                }
                if (status == NS_OK) {
                    tablePtr = ns_malloc(sizeof(Tcl_HashTable));
                    Tcl_InitHashTable(tablePtr, TCL_STRING_KEYS);
                    Tcl_SetHashValue(hPtr, tablePtr);
                    status = Ns_SockCallback(sock, ListenCallback, tablePtr,
                                             (unsigned int)NS_SOCK_READ | (unsigned int)NS_SOCK_EXIT);
                }
                if (status != NS_OK && sock != NS_INVALID_SOCKET) {
                    ns_sockclose(sock);
                    sock = NS_INVALID_SOCKET;
                }
            }
            if (sock != NS_INVALID_SOCKET) {
                char ipString[NS_IPADDR_SIZE] = {'\0'};

                assert(tablePtr != NULL);

                hPtr = Tcl_CreateHashEntry(tablePtr,
                                           ns_inet_ntop(saPtr, ipString, NS_IPADDR_SIZE),
                                           &isNew);
                Ns_Log(Debug, "Ns_SockListenCallback: registering IP addr %s isNew %d", ipString, isNew);
                Ns_LogSockaddr(Debug, "... register IP + PROTO", (const struct sockaddr *) saPtr);

                if (isNew == 0) {
                    Ns_Log(Error, "listen callback: there is already a listen callback registered");
                    ns_sockclose(sock);
                    sock = NS_INVALID_SOCKET;
                } else {
                    ListenData *ldPtr;

                    ldPtr = ns_malloc(sizeof(ListenData));
                    ldPtr->proc = proc;
                    ldPtr->arg = arg;
                    Tcl_SetHashValue(hPtr, ldPtr);
                }
            }
            Ns_MutexUnlock(&lock);
        }
    }

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockPortBound --
 *
 *      Determine if we're already listening on a given port on any
 *      address.
 *
 * Results:
 *      Boolean
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_SockPortBound(unsigned short port)
{
    const Tcl_HashEntry *hPtr;

    Ns_MutexLock(&lock);
    hPtr = Tcl_FindHashEntry(&portsTable, INT2PTR(port));
    Ns_MutexUnlock(&lock);

    return (hPtr != NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * ListenCallback --
 *
 *      This is a wrapper callback that runs the user's callback iff
 *      a valid socket exists.
 *
 * Results:
 *      NS_TRUE or NS_FALSE
 *
 * Side effects:
 *      May close the socket if no user context can be found.
 *
 *----------------------------------------------------------------------
 */

static bool
ListenCallback(NS_SOCKET sock, void *arg, unsigned int why)
{
    struct NS_SOCKADDR_STORAGE sa;
    socklen_t           len;
    Tcl_HashTable      *tablePtr;
    NS_SOCKET           newSock;
    bool                success;

    tablePtr = arg;

    if (why == (unsigned int)NS_SOCK_EXIT) {
        (void) ns_sockclose(sock);
        return NS_FALSE;
    }

    newSock = Ns_SockAccept(sock, NULL, NULL);

    if (likely(newSock != NS_INVALID_SOCKET)) {
        const Tcl_HashEntry *hPtr;
        const ListenData    *ldPtr;
        int                  retVal;
        char                 ipString[NS_IPADDR_SIZE] = {'\0'};

        (void) Ns_SockSetBlocking(newSock);
        len = (socklen_t)sizeof(sa);
        retVal = getsockname(newSock, (struct sockaddr *) &sa, &len);
        if (retVal == -1) {
            Ns_Log(Warning, "listencallback: can't obtain socket info %s", ns_sockstrerror(ns_sockerrno));
            (void) ns_sockclose(sock);
            return NS_FALSE;
        }
        ldPtr = NULL;

        (void)ns_inet_ntop((struct sockaddr *)&sa, ipString, NS_IPADDR_SIZE);
        Ns_Log(Debug, "ListenCallback: ipstring <%s>", ipString);

        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(tablePtr, ipString);
        if (hPtr == NULL) {
            hPtr = Tcl_FindHashEntry(tablePtr, NS_IP_UNSPECIFIED);
        }
        if (hPtr != NULL) {
            ldPtr = Tcl_GetHashValue(hPtr);
        }
        Ns_MutexUnlock(&lock);

        Ns_LogSockaddr(Notice, "... query IP + PROTO", (const struct sockaddr *) &sa);

        if (ldPtr == NULL) {
            /*
             * There was no hash entry for the listen callback
             */
            int result = ns_sockclose(newSock);
            success = (result == 0);
        } else {
            /*
             * The hash entry was found, so fire the callback (e.g. exec Tcl
             * code).
             */
            success = (*ldPtr->proc) (newSock, ldPtr->arg, why);
        }
    } else {
        success = NS_FALSE;
    }
    return success;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
