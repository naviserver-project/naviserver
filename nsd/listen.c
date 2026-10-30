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
    Ns_Callback *freeProc;
} ListenData;

typedef struct ListenWait {
    Ns_Mutex lock;
    Ns_Cond cond;
    bool done;
} ListenWait;

typedef struct ListenPort {
    Tcl_HashTable addresses;
    NS_SOCKET sock;
    unsigned short port;
    bool closing;
    ListenWait *waitPtr;
} ListenPort;

/*
 * Local functions defined in this file
 */

static Ns_SockProc  ListenCallback;
static Ns_SockProc  ListenClose;

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
    return NsSockListenCallback(addr, port, proc, bind, arg, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * NsSockListenCallback --
 *
 *      Register a listener with optional ownership of its callback argument.
 *      The argument is released on successful listener teardown only.
 *
 * Results:
 *      A listening socket, or NS_INVALID_SOCKET. On failure the caller
 *      retains ownership of arg. Existing registrations are left intact.
 *
 * Side effects:
 *      Opens a socket and registers it with the socket callback thread.
 *
 *----------------------------------------------------------------------
 */
NS_SOCKET
NsSockListenCallback(const char *addr, unsigned short port, Ns_SockProc *proc,
                     bool doBind, void *arg, Ns_Callback *freeProc)
{
    struct NS_SOCKADDR_STORAGE sa;
    struct sockaddr *saPtr = (struct sockaddr *)&sa;
    NS_SOCKET sock = NS_INVALID_SOCKET;

    NS_NONNULL_ASSERT(proc != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    if (Ns_GetSockAddr(saPtr, addr, port) == NS_OK) {
        NS_SOCKET bound = Ns_SockBind(saPtr, NS_FALSE);

        if (bound != NS_INVALID_SOCKET) {
            socklen_t len = sizeof(sa);

            if (getsockname(bound, saPtr, &len) == 0) {
                Tcl_HashEntry *hPtr;
                int isNew;

                port = Ns_SockaddrGetPort(saPtr);
                if (!doBind) {
                    ns_sockclose(bound);
                    bound = NS_INVALID_SOCKET;
                }
                Ns_MutexLock(&lock);
                hPtr = Tcl_CreateHashEntry(&portsTable, INT2PTR(port), &isNew);
                if (isNew != 0) {
                    ListenPort *portPtr = ns_calloc(1u, sizeof(ListenPort));

                    sock = bound != NS_INVALID_SOCKET ? bound : Ns_SockListen(NULL, port);
                    bound = NS_INVALID_SOCKET;
                    portPtr->sock = sock;
                    portPtr->port = port;
                    Tcl_InitHashTable(&portPtr->addresses, TCL_STRING_KEYS);
                    if (sock != NS_INVALID_SOCKET && (!doBind || listen(sock, 5) == 0)
                        && Ns_SockSetNonBlocking(sock) == NS_OK) {
                        char address[NS_IPADDR_SIZE];
                        ListenData *dataPtr = ns_malloc(sizeof(ListenData));
                        Tcl_HashEntry *addressPtr;

                        dataPtr->proc = proc;
                        dataPtr->arg = arg;
                        dataPtr->freeProc = freeProc;
                        addressPtr = Tcl_CreateHashEntry(&portPtr->addresses,
                                                         ns_inet_ntop(saPtr, address, sizeof(address)), &isNew);
                        Tcl_SetHashValue(addressPtr, dataPtr);
                        Tcl_SetHashValue(hPtr, portPtr);
                        if (Ns_SockCallback(sock, ListenCallback, portPtr,
                                            NS_SOCK_READ | NS_SOCK_EXIT) != NS_OK) {
                            ns_free(dataPtr);
                            ns_sockclose(sock);
                            sock = NS_INVALID_SOCKET;
                        }
                    } else {
                        if (sock != NS_INVALID_SOCKET) {
                            ns_sockclose(sock);
                        }
                        sock = NS_INVALID_SOCKET;
                    }
                    if (sock == NS_INVALID_SOCKET) {
                        Tcl_DeleteHashEntry(hPtr);
                        Tcl_DeleteHashTable(&portPtr->addresses);
                        ns_free(portPtr);
                    }
                }
                Ns_MutexUnlock(&lock);
            }
            if (bound != NS_INVALID_SOCKET) {
                ns_sockclose(bound);
            }
        }
    }
    return sock;
}

/*
 *----------------------------------------------------------------------
 *
 * NsSockCloseListenCallback --
 *
 *      Cancel a listener on its owning callback thread, then close it.
 *      Wait for completion unless called by that same callback thread.
 *      Closing from an accept script is deferred until the script returns.
 *      Before startup, defer cleanup rather than wait for the callback
 *      thread, which is itself waiting for startup to complete.
 *
 * Results:
 *      None. An already closed listener requires no further work.
 *
 * Side effects:
 *      Releases the port registration, socket and owned callback argument.
 *      During shutdown, the exit callback completes the same teardown.
 *
 *----------------------------------------------------------------------
 */
void
NsSockCloseListenCallback(NS_SOCKET sock)
{
    ListenWait     wait = {0};
    ListenPort    *portPtr = NULL;
    Tcl_HashSearch search;
    Tcl_HashEntry *hPtr;
    bool           waitForClose = Ns_InfoStarted() && !NsSockCallbackIsCurrentThread();

    Ns_MutexInit(&wait.lock);
    Ns_CondInit(&wait.cond);
    Ns_MutexLock(&lock);
    for (hPtr = Tcl_FirstHashEntry(&portsTable, &search); hPtr != NULL;
         hPtr = Tcl_NextHashEntry(&search)) {
        ListenPort *candidate = Tcl_GetHashValue(hPtr);

        if (candidate->sock == sock && !candidate->closing) {
            portPtr = candidate;
            portPtr->closing = NS_TRUE;
            portPtr->waitPtr = waitForClose ? &wait : NULL;
            break;
        }
    }
    Ns_MutexUnlock(&lock);
    if (portPtr != NULL) {
        /*
         * Shutdown rejection leaves completion to ListenCallback's exit event.
         */
        (void)Ns_SockCancelCallbackEx(sock, ListenClose, portPtr, NULL);
        if (waitForClose) {
            Ns_MutexLock(&wait.lock);
            while (!wait.done) {
                Ns_CondWait(&wait.cond, &wait.lock);
            }
            Ns_MutexUnlock(&wait.lock);
        }
    }
    Ns_CondDestroy(&wait.cond);
    Ns_MutexDestroy(&wait.lock);
}

/*
 *----------------------------------------------------------------------
 *
 * ListenClose --
 *
 *      Finalize a listener after callback cancellation or at shutdown.
 *      Runs on the callback thread, after any accept callback has returned.
 *
 * Results:
 *      NS_FALSE, since this listener must no longer be polled.
 *
 * Side effects:
 *      Closes the socket, releases registry state and signals its closer.
 *
 *----------------------------------------------------------------------
 */
static bool
ListenClose(NS_SOCKET sock, void *arg, unsigned int UNUSED(why))
{
    ListenPort     *portPtr = arg;
    ListenWait     *waitPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    Ns_MutexLock(&lock);
    hPtr = Tcl_FindHashEntry(&portsTable, INT2PTR(portPtr->port));
    assert(hPtr != NULL && Tcl_GetHashValue(hPtr) == portPtr);
    ns_sockclose(sock);
    Tcl_DeleteHashEntry(hPtr);
    waitPtr = portPtr->waitPtr;
    Ns_MutexUnlock(&lock);
    for (hPtr = Tcl_FirstHashEntry(&portPtr->addresses, &search); hPtr != NULL;
         hPtr = Tcl_NextHashEntry(&search)) {
        ListenData *dataPtr = Tcl_GetHashValue(hPtr);

        if (dataPtr->freeProc != NULL) {
            (*dataPtr->freeProc)(dataPtr->arg);
        }
        ns_free(dataPtr);
    }
    Tcl_DeleteHashTable(&portPtr->addresses);
    ns_free(portPtr);
    if (waitPtr != NULL) {
        Ns_MutexLock(&waitPtr->lock);
        waitPtr->done = NS_TRUE;
        Ns_CondSignal(&waitPtr->cond);
        Ns_MutexUnlock(&waitPtr->lock);
    }
    return NS_FALSE;
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
    ListenPort         *portPtr;
    NS_SOCKET           newSock;
    bool                success, closing;

    portPtr = arg;

    if (why == (unsigned int)NS_SOCK_EXIT) {
        return ListenClose(sock, portPtr, why);
    }

    Ns_MutexLock(&lock);
    closing = portPtr->closing;
    Ns_MutexUnlock(&lock);
    if (closing) {
        return NS_TRUE;
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
            (void) ns_sockclose(newSock);
            return NS_TRUE;
        }
        ldPtr = NULL;

        (void)ns_inet_ntop((struct sockaddr *)&sa, ipString, NS_IPADDR_SIZE);
        Ns_Log(Debug, "ListenCallback: ipstring <%s>", ipString);

        Ns_MutexLock(&lock);
        hPtr = Tcl_FindHashEntry(&portPtr->addresses, ipString);
        if (hPtr == NULL) {
            hPtr = Tcl_FindHashEntry(&portPtr->addresses, NS_IP_UNSPECIFIED);
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
            success = (*ldPtr->proc)(newSock, ldPtr->arg, why);
        }
    } else {
        success = NS_TRUE;
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
