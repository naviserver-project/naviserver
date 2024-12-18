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
 * sockcallback.c --
 *
 *      Support for the socket callback thread.
 */

#include "nsd.h"

/*
 * The following defines a socket being monitored.
 */

typedef struct Callback {
    struct Callback     *nextPtr;
    NS_SOCKET            sock;
    NS_POLL_NFDS_TYPE    idx;
    unsigned int         when;
    Ns_Time              timeout;
    Ns_Time              expires;
    Ns_SockProc         *proc;
    void                *arg;
} Callback;

/*
 * Local functions defined in this file
 */

static Ns_ThreadProc SockCallbackThread;
static Ns_ReturnCode Queue(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when,
                           const Ns_Time *timeout, const char **threadNamePtr);
static void CallbackTrigger(void);

/*
 * Static variables defined in this file
 */

static Callback     *firstQueuePtr = NULL, *lastQueuePtr = NULL;
static bool          shutdownPending = NS_FALSE;
static bool          running = NS_FALSE;
static Ns_Thread     sockThread;
static Ns_Mutex      lock = NULL;
static Ns_Cond       cond = NULL;
static NS_SOCKET     trigPipe[2];
static Tcl_HashTable activeCallbacks;


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockCallback --
 *
 *      Register a callback to be run when a socket reaches a certain
 *      state.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will wake up the callback thread.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_SockCallback(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when)
{
    return Queue(sock, proc, arg, when, NULL, NULL);
}

Ns_ReturnCode
Ns_SockCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when,
                  const Ns_Time *timeout, const char **threadNamePtr)
{
    return Queue(sock, proc, arg, when, timeout, threadNamePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockCancelCallback, Ns_SockCancelCallbackEx --
 *
 *      Remove a callback registered on a socket.  Optionally execute
 *      a callback from the SockCallbackThread.
 *
 * Results:
 *      NS_OK/NS_ERROR
 *
 * Side effects:
 *      Will wake up the callback thread.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SockCancelCallback(NS_SOCKET sock)
{
    (void) Ns_SockCancelCallbackEx(sock, NULL, NULL, NULL);
}

Ns_ReturnCode
Ns_SockCancelCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, const char **threadNamePtr)
{
    return Queue(sock, proc, arg, (unsigned int)NS_SOCK_CANCEL, NULL, threadNamePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitSockCallback --
 *
 *      Global initialization routine for sockcallbacks.
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
NsInitSockCallback(void)
{
    static bool initialized = NS_FALSE;

    if (!initialized) {
        Tcl_InitHashTable(&activeCallbacks, TCL_ONE_WORD_KEYS);
        Ns_MutexInit(&lock);
        Ns_MutexSetName(&lock, "ns:sockcallbacks");
        Ns_CondInit(&cond);
        initialized = NS_TRUE;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartSockShutdown, NsWaitSockShutdown --
 *
 *      Initiate and then wait for socket callbacks shutdown.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May timeout waiting for shutdown.
 *
 *----------------------------------------------------------------------
 */

void
NsStartSockShutdown(void)
{
    Ns_MutexLock(&lock);
    if (running) {
        shutdownPending = NS_TRUE;
        CallbackTrigger();
    }
    Ns_MutexUnlock(&lock);
}

void
NsWaitSockShutdown(const Ns_Time *toPtr)
{
    Ns_ReturnCode status = NS_OK;

    Ns_MutexLock(&lock);
    while (status == NS_OK && running) {
        status = Ns_CondTimedWait(&cond, &lock, toPtr);
    }
    Ns_MutexUnlock(&lock);
    if (status != NS_OK) {
        Ns_Log(Warning, "socks: timeout waiting for callback shutdown");
    } else if (sockThread != NULL) {
        Ns_ThreadJoin(&sockThread, NULL);
        sockThread = NULL;
        ns_sockclose(trigPipe[0]);
        ns_sockclose(trigPipe[1]);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CallbackTrigger --
 *
 *      Wakeup the callback thread when it is waiting for input in a
 *      poll() call.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
CallbackTrigger(void)
{
    if (ns_send(trigPipe[1], NS_EMPTY_STRING, 1u, 0) != 1) {
        Ns_Fatal("trigger send() failed: %s", ns_sockstrerror(ns_sockerrno));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Queue --
 *
 *      Queue a callback for socket.
 *
 * Results:
 *      NS_OK or NS_ERROR on shutdown pending.
 *
 * Side effects:
 *      Socket thread may be created or signaled.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
Queue(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when,
      const Ns_Time *timeout, const char **threadNamePtr)
{
    Callback     *cbPtr;
    Ns_ReturnCode status;
    bool          trigger, create;

    cbPtr = ns_calloc(1u, sizeof(Callback));
    cbPtr->sock = sock;
    cbPtr->proc = proc;
    cbPtr->arg = arg;
    cbPtr->when = when;
    trigger = create = NS_FALSE;

    if (timeout != NULL) {
        cbPtr->timeout = *timeout;
        Ns_GetTime(&cbPtr->expires);
        Ns_IncrTime(&cbPtr->expires, cbPtr->timeout.sec, cbPtr->timeout.usec);
    } else {
        cbPtr->timeout.sec = 0;
        cbPtr->timeout.usec = 0;
    }

    Ns_MutexLock(&lock);
    if (shutdownPending) {
        ns_free(cbPtr);
        status = NS_ERROR;
    } else {
        if (!running) {
            create = NS_TRUE;
            running = NS_TRUE;
        } else if (firstQueuePtr == NULL) {
            trigger = NS_TRUE;
        }
        if (firstQueuePtr == NULL) {
            firstQueuePtr = cbPtr;
        } else {
            lastQueuePtr->nextPtr = cbPtr;
        }
        cbPtr->nextPtr = NULL;
        lastQueuePtr = cbPtr;
        status = NS_OK;
    }
    Ns_MutexUnlock(&lock);

    if (threadNamePtr != NULL) {
        /*
         * The thread name is currently just a constant, but when
         * implementing multiple "-socks-" threads, threadNamePtr should
         * return the associated queue. This way, we can keep the
         * interface constant.
         */
        *threadNamePtr = "-socks-";
    }

    if (trigger) {
        CallbackTrigger();
    } else if (create) {
        if (ns_sockpair(trigPipe) != 0) {
            Ns_Fatal("ns_sockpair() failed: %s", ns_sockstrerror(ns_sockerrno));
        }
        Ns_ThreadCreate(SockCallbackThread, NULL, 0, &sockThread);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * SockCallbackThread --
 *
 *      Run callbacks registered with Ns_SockCallback.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on callbacks.
 *
 *----------------------------------------------------------------------
 */

static void
SockCallbackThread(void *UNUSED(arg))
{
    char           c;
    unsigned int   when[3];
    short          events[3];
    int            n, i, isNew;
    size_t         maxPollfds = 100u;
    Callback      *cbPtr, *nextPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    struct pollfd *pfds;

    Ns_ThreadSetName("-socks-");
    (void)Ns_WaitForStartup();
    Ns_Log(Notice, "socks: starting");

    /*
     * The array events[] is used
     *   1) for the requested poll mask, and
     *   2) for associating the revents received by the poll call to the
     *      NS_SOCK* states reported back
     *
     * The NS_SOCK* states are kept in the corresponding when[]
     * elements.  The positions in this array are for 'r', 'w' and 'e'
     * callback types in this order.
     */
    events[0] = (short)POLLIN;
    events[1] = (short)POLLOUT;
    events[2] = (short)POLLERR;
    when[0] = (unsigned int)NS_SOCK_READ;
    when[1] = (unsigned int)NS_SOCK_WRITE;
    when[2] = (unsigned int)NS_SOCK_EXCEPTION | (unsigned int)NS_SOCK_DONE;

    pfds = (struct pollfd *)ns_malloc(sizeof(struct pollfd) * maxPollfds);
    pfds[0].fd = trigPipe[0];
    pfds[0].events = (short)POLLIN;

    for (;;) {
        long              pollTimeout;
        NS_POLL_NFDS_TYPE nfds;
        bool              stop;
        Ns_Time           now, diff = {0, 0};

        /*
         * Grab the list of any queue updates and the shutdown flag.
         */

        Ns_MutexLock(&lock);
        cbPtr = firstQueuePtr;
        firstQueuePtr = NULL;
        lastQueuePtr = NULL;
        stop = shutdownPending;
        Ns_MutexUnlock(&lock);

        /*
         * Move any queued callbacks to the activeCallbacks table.
         */

        while (cbPtr != NULL) {
            nextPtr = cbPtr->nextPtr;
            if ((cbPtr->when & (unsigned int)NS_SOCK_CANCEL) != 0u) {
                /*
                 * We have a cancel callback. Find active callback in
                 * hash table and remove it.
                 */
                hPtr = Tcl_FindHashEntry(&activeCallbacks, NSSOCK2PTR(cbPtr->sock));
                if (hPtr != NULL) {
                    ns_free(Tcl_GetHashValue(hPtr));
                    Tcl_DeleteHashEntry(hPtr);
                }
                /*
                 * If there is a callback proc, execute it.
                 */
                if (cbPtr->proc != NULL) {
                    /*
                     * For the time being, ignore boolean result.
                     */
                    (void) (*cbPtr->proc)(cbPtr->sock, cbPtr->arg, (unsigned int)NS_SOCK_CANCEL);
                }
                ns_free(cbPtr);
            } else {
                hPtr = Tcl_CreateHashEntry(&activeCallbacks, NSSOCK2PTR(cbPtr->sock), &isNew);
                if (isNew == 0) {
                    ns_free(Tcl_GetHashValue(hPtr));
                }
                Tcl_SetHashValue(hPtr, cbPtr);
            }
            cbPtr = nextPtr;
        }

        /*
         * Check, if we have to extend maxPollfds and realloc memory if
         * necessary.
         */
        if (maxPollfds <= (size_t)activeCallbacks.numEntries) {
            maxPollfds  = (size_t)activeCallbacks.numEntries + 100u;
            pfds = (struct pollfd *)ns_realloc(pfds, sizeof(struct pollfd) * maxPollfds);
        }

        /*
         * Wake up every 30 seconds to process expired sockets
         */

        pollTimeout = 30000;
        Ns_GetTime(&now);

        /*
         * Verify and set the poll bits for all active callbacks.
         */

        nfds = 1;
        for (hPtr = Tcl_FirstHashEntry(&activeCallbacks, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
            cbPtr = Tcl_GetHashValue(hPtr);
            if ((cbPtr->timeout.sec > 0 || cbPtr->timeout.usec > 0)) {

                if (Ns_DiffTime(&now, &cbPtr->expires, &diff) > 0) {
                    /*
                     * Call Ns_SockProc to notify about timeout. For the
                     * time being, ignore boolean result.
                     */
                    Ns_Log(Notice, "sockcallback: fd %d timeout " NS_TIME_FMT " exceeded by " NS_TIME_FMT,
                           cbPtr->sock, (int64_t) cbPtr->timeout.sec, cbPtr->timeout.usec,
                           (int64_t) diff.sec, diff.usec
                           );
                    (void) (*cbPtr->proc)(cbPtr->sock, cbPtr->arg, (unsigned int)NS_SOCK_TIMEOUT);
                    cbPtr->when = 0u;
                }
            }
            if ((cbPtr->when & NS_SOCK_ANY) == 0u) {
                Tcl_DeleteHashEntry(hPtr);
                ns_free(cbPtr);
            } else {
                cbPtr->idx = nfds;
                pfds[nfds].fd = cbPtr->sock;
                pfds[nfds].events = pfds[nfds].revents = 0;
                for (i = 0; i < Ns_NrElements(when); ++i) {
                    //Ns_Log(Notice, "SockCallback check when[%d]: fd %d %.4x", i, pfds[i].fd, when[i]);
                    if ((cbPtr->when & when[i]) != 0u) {
                        pfds[nfds].events |= events[i];
                    }
                }
                ++nfds;

                if (cbPtr->timeout.sec != 0 || cbPtr->timeout.usec != 0) {
                    time_t to = diff.sec * -1000 + diff.usec / 1000 + 1;

                    if (to < pollTimeout)  {
                        /*
                         * Reduce poll timeout to smaller value.
                         */
                        pollTimeout = (long)to;
                    }
                }
            }
        }

        /*for (i=0; i<(int)nfds; i++) {
            Ns_Log(Notice, "SockCallback pollbits [%d]: fd %d %.4x", i, pfds[i].fd, pfds[i].events);
            }*/

        /*
         * Call poll() on the sockets and drain the trigger pipe if
         * necessary.
         */

        if (stop) {
            break;
        }

        pfds[0].revents = 0;
        do {
            Ns_Log(Debug, "SockCallback before poll nfds %ld timeout %zd", (long)nfds, pollTimeout);
            n = ns_poll(pfds, nfds, pollTimeout);
            Ns_Log(Debug, "SockCallback poll returned %d", n);
        } while (n < 0  && errno == NS_EINTR);

        if (n < 0) {
            Ns_Fatal("sockcallback: ns_poll() failed: %s",
                     ns_sockstrerror(ns_sockerrno));
        }
        if (((pfds[0].revents & POLLIN) != 0)
            && recv(trigPipe[0], &c, 1, 0) != 1) {
            Ns_Fatal("trigger ns_read() failed: %s", strerror(errno));
        }

        if (n > 0) {
            /*
             * Execute any ready callbacks.
             */
            for (hPtr = Tcl_FirstHashEntry(&activeCallbacks, &search); hPtr != NULL;
                 hPtr = Tcl_NextHashEntry(&search)) {
                cbPtr = Tcl_GetHashValue(hPtr);
                for (i = 0; i < Ns_NrElements(when); ++i) {
                    if (((cbPtr->when & when[i]) != 0u)
                        && (pfds[cbPtr->idx].revents & events[i]) != 0) {
                        /*
                         * Call the Sock_Proc with the SockState flag
                         * combination from when[i]. This is actually
                         * the only place, where an Ns_SockProc is called
                         * with a flag combination in the last
                         * argument. If this would not be the case, we
                         * could set the type of the last parameter of
                         * Ns_SockProc to Ns_SockState.
                         */
                        if ((*cbPtr->proc)(cbPtr->sock, cbPtr->arg, when[i]) == NS_FALSE) {
                            cbPtr->when = 0u;
                        } else {
                            if (cbPtr->timeout.sec != 0 || cbPtr->timeout.usec != 0) {
                                Ns_GetTime(&cbPtr->expires);
                                Ns_IncrTime(&cbPtr->expires, cbPtr->timeout.sec, cbPtr->timeout.usec);
                            }
                        }
                    }
                }
            }
        }
    }
    /*
     * Fire socket exit callbacks.
     */

    Ns_Log(Notice, "socks: shutdown pending");
    for (hPtr = Tcl_FirstHashEntry(&activeCallbacks, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
        cbPtr = Tcl_GetHashValue(hPtr);
        if ((cbPtr->when & (unsigned int)NS_SOCK_EXIT) != 0u) {
            (void) ((*cbPtr->proc)(cbPtr->sock, cbPtr->arg, (unsigned int)NS_SOCK_EXIT));
        }
    }
    /*
     * Clean up the registered callbacks.
     */
    for (hPtr = Tcl_FirstHashEntry(&activeCallbacks, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
        ns_free(Tcl_GetHashValue(hPtr));
    }
    Tcl_DeleteHashTable(&activeCallbacks);
    ns_free(pfds);

    Ns_Log(Notice, "socks: shutdown complete");

    /*
     * Tell others that shutdown is complete.
     */
    Ns_MutexLock(&lock);
    running = NS_FALSE;
    Ns_CondBroadcast(&cond);
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetSockCallbacks --
 *
 *      Return all defined socket callbacks in form of a valid Tcl list
 *      in the provided Tcl_DString. The passed Tcl_DString has to be
 *      initialized by the caller.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      DString is updated
 *
 *----------------------------------------------------------------------
 */

void
NsGetSockCallbacks(Tcl_DString *dsPtr)
{
    Tcl_HashSearch  search;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MutexLock(&lock);
    if (running) {
        const Tcl_HashEntry *hPtr;

        for (hPtr = Tcl_FirstHashEntry(&activeCallbacks, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
            const Callback *cbPtr = Tcl_GetHashValue(hPtr);
            char            buf[TCL_INTEGER_SPACE];

            /*
             * The "when" conditions are ORed together. Return these
             * as a sublist of conditions.
             */
            Tcl_DStringStartSublist(dsPtr);
            snprintf(buf, sizeof(buf), "%d", (int) cbPtr->sock);
            Tcl_DStringAppendElement(dsPtr, buf);
            Tcl_DStringStartSublist(dsPtr);
            if ((cbPtr->when & (unsigned int)NS_SOCK_READ) != 0u) {
                Tcl_DStringAppendElement(dsPtr, "read");
            }
            if ((cbPtr->when & (unsigned int)NS_SOCK_WRITE) != 0u) {
                Tcl_DStringAppendElement(dsPtr, "write");
            }
            if ((cbPtr->when & (unsigned int)NS_SOCK_EXCEPTION) != 0u) {
                Tcl_DStringAppendElement(dsPtr, "exception");
            }
            if ((cbPtr->when & (unsigned int)NS_SOCK_EXIT) != 0u) {
                Tcl_DStringAppendElement(dsPtr, "exit");
            }
            Tcl_DStringEndSublist(dsPtr);
            Ns_GetProcInfo(dsPtr, (ns_funcptr_t)cbPtr->proc, cbPtr->arg);
            Ns_DStringNAppend(dsPtr, " ", 1);
            Ns_DStringAppendTime(dsPtr, &cbPtr->timeout);
            Tcl_DStringEndSublist(dsPtr);
        }
    }
    Ns_MutexUnlock(&lock);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 72
 * indent-tabs-mode: nil
 * End:
 */
