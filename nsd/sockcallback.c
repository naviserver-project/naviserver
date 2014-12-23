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
 * sockcallback.c --
 *
 *	Support for the socket callback thread.
 */

#include "nsd.h"

/*
 * The following defines a socket being monitored.
 */

typedef struct Callback {
    struct Callback     *nextPtr;
    NS_SOCKET            sock;
    int			 idx;
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
static int Queue(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when, const Ns_Time *timeout, char const**threadNamePtr);
static void CallbackTrigger(void);

/*
 * Static variables defined in this file
 */

static Callback	    *firstQueuePtr = NULL, *lastQueuePtr = NULL;
static bool	     shutdownPending = NS_FALSE;
static bool	     running = NS_FALSE;
static Ns_Thread     sockThread;
static Ns_Mutex      lock;
static Ns_Cond	     cond;
static NS_SOCKET     trigPipe[2];
static Tcl_HashTable table;


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockCallback --
 *
 *	Register a callback to be run when a socket reaches a certain
 *	state.
 *
 * Results:
 *	NS_OK/NS_ERROR
 *
 * Side effects:
 *	Will wake up the callback thread.
 *
 *----------------------------------------------------------------------
 */

int
Ns_SockCallback(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when)
{
    return Queue(sock, proc, arg, when, NULL, NULL);
}

int
Ns_SockCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when, Ns_Time *timeout, char const**threadNamePtr)
{
    return Queue(sock, proc, arg, when, timeout, threadNamePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SockCancelCallback, Ns_SockCancelCallbackEx --
 *
 *	Remove a callback registered on a socket.  Optionally execute
 *	a callback from the SockCallbackThread.
 *
 * Results:
 *	NS_OK/NS_ERROR
 *
 * Side effects:
 *	Will wake up the callback thread.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SockCancelCallback(NS_SOCKET sock)
{
    (void) Ns_SockCancelCallbackEx(sock, NULL, NULL, NULL);
}

int
Ns_SockCancelCallbackEx(NS_SOCKET sock, Ns_SockProc *proc, void *arg, char const**threadNamePtr)
{
    return Queue(sock, proc, arg, (unsigned int)NS_SOCK_CANCEL, NULL, threadNamePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartSockShutdown, NsWaitSockShutdown --
 *
 *	Initiate and then wait for socket callbacks shutdown.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	May timeout waiting for shutdown.
 *
 *----------------------------------------------------------------------
 */

void
NsStartSockShutdown(void)
{
    Ns_MutexLock(&lock);
    if (running == NS_TRUE) {
	shutdownPending = NS_TRUE;
	CallbackTrigger();
    }
    Ns_MutexUnlock(&lock);
}

void
NsWaitSockShutdown(const Ns_Time *toPtr)
{
    int status;

    status = NS_OK;
    Ns_MutexLock(&lock);
    while (status == NS_OK && running == NS_TRUE) {
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
 *	Wakeup the callback thread if it's in poll().
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
CallbackTrigger(void)
{
    if (ns_send(trigPipe[1], "", 1, 0) != 1) {
	Ns_Fatal("trigger send() failed: %s", ns_sockstrerror(ns_sockerrno));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Queue --
 *
 *	Queue a callback for socket.
 *
 * Results:
 *	NS_OK or NS_ERROR on shutdown pending.
 *
 * Side effects:
 *	Socket thread may be created or signalled.
 *
 *----------------------------------------------------------------------
 */

static int
Queue(NS_SOCKET sock, Ns_SockProc *proc, void *arg, unsigned int when,
      const Ns_Time *timeout, char const**threadNamePtr)
{
    Callback   *cbPtr;
    int         status;
    bool        trigger, create;

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
    if (shutdownPending == NS_TRUE) {
	ns_free(cbPtr);
    	status = NS_ERROR;
    } else {
	if (running == NS_FALSE) {
    	    Tcl_InitHashTable(&table, TCL_ONE_WORD_KEYS);
	    Ns_MutexSetName(&lock, "ns:sockcallbacks");
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
         * threadName is currently just a constant, but when implementing
         * multiple socks threads, threadNamePtr should return the associated
         * queue. This way, we can keep the interface constant.
         */
        *threadNamePtr = "-socks-";
    }
    
    if (trigger == NS_TRUE) {
	CallbackTrigger();
    } else if (create == NS_TRUE) {
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
 *	Run callbacks registered with Ns_SockCallback.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Depends on callbacks.
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
    size_t         max;
    Callback      *cbPtr, *nextPtr;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    struct pollfd *pfds;

    Ns_ThreadSetName("-socks-");
    (void)Ns_WaitForStartup();
    Ns_Log(Notice, "socks: starting");

    events[0] = POLLIN;
    events[1] = POLLOUT;
    events[2] = POLLPRI;
    when[0] = (unsigned int)NS_SOCK_READ;
    when[1] = (unsigned int)NS_SOCK_WRITE;
    when[2] = (unsigned int)NS_SOCK_EXCEPTION | (unsigned int)NS_SOCK_DONE;
    max = 100u;
    pfds = ns_malloc(sizeof(struct pollfd) * max);
    pfds[0].fd = trigPipe[0];
    pfds[0].events = POLLIN;

    while (1) {
	int nfds, pollto;
        bool stop;
	Ns_Time now, diff;

	/*
	 * Grab the list of any queue updates and the shutdown
	 * flag.
	 */

    	Ns_MutexLock(&lock);
	cbPtr = firstQueuePtr;
	firstQueuePtr = NULL;
        lastQueuePtr = NULL;
	stop = shutdownPending;
	Ns_MutexUnlock(&lock);

	/*
    	 * Move any queued callbacks to the active table.
	 */

        while (cbPtr != NULL) {
            nextPtr = cbPtr->nextPtr;
            if ((cbPtr->when & (unsigned int)NS_SOCK_CANCEL) != 0u) {
		hPtr = Tcl_FindHashEntry(&table, NSSOCK2PTR(cbPtr->sock));
                if (hPtr != NULL) {
                    ns_free(Tcl_GetHashValue(hPtr));
                    Tcl_DeleteHashEntry(hPtr);
                }
                if (cbPtr->proc != NULL) {
                    /*
                     * Call Ns_SockProc to notify about cancel. For the
                     * time being, ignore boolean result.
                     */
                    (void) (*cbPtr->proc)(cbPtr->sock, cbPtr->arg, (unsigned int)NS_SOCK_CANCEL);
                }
                ns_free(cbPtr);
            } else {
                hPtr = Tcl_CreateHashEntry(&table, NSSOCK2PTR(cbPtr->sock), &isNew);
                if (isNew == 0) {
                    ns_free(Tcl_GetHashValue(hPtr));
                }
                Tcl_SetHashValue(hPtr, cbPtr);
            }
            cbPtr = nextPtr;
        }

	/*
	 * Verify and set the poll bits for all active callbacks.
	 */

	if (max <= (size_t)table.numEntries) {
	    max  = (size_t)table.numEntries + 100u;
	    pfds = ns_realloc(pfds, sizeof(struct pollfd) * max);
	}

        /*
         * Wake up every 30 seconds to process expired sockets
         */

        pollto = 30000;
        Ns_GetTime(&now);

	nfds = 1;
        for (hPtr = Tcl_FirstHashEntry(&table, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
	    cbPtr = Tcl_GetHashValue(hPtr);
            if ((cbPtr->timeout.sec > 0 || cbPtr->timeout.usec > 0)) {

                if (Ns_DiffTime(&now, &cbPtr->expires, &diff) > 0) {
                    /*
                     * Call Ns_SockProc to notify about timeout. For the
                     * time being, ignore boolean result.
                     */
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
                    if ((cbPtr->when & when[i]) != 0u) {
			pfds[nfds].events |= events[i];
                    }
        	}
		++nfds;

                if (cbPtr->timeout.sec != 0 || cbPtr->timeout.usec != 0) {
                    int to = (int)diff.sec * -1000 + (int)diff.usec / 1000 + 1;
                    if (to < pollto)  {
                        /*
                         * Reduce poll timeout to smaller value.
                         */
                        pollto = to;
                    }
                }
	    }
        }

    	/*
	 * Select on the sockets and drain the trigger pipe if
	 * necessary.
	 */

	if (stop == NS_TRUE) {
	    break;
	}
	pfds[0].revents = 0;
        do {
            n = ns_poll(pfds, nfds, pollto);
        } while (n < 0  && errno == EINTR);

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
            for (hPtr = Tcl_FirstHashEntry(&table, &search); n > 0 && hPtr != NULL; 
                 hPtr = Tcl_NextHashEntry(&search)) {
                cbPtr = Tcl_GetHashValue(hPtr);
                for (i = 0; i < Ns_NrElements(when); ++i) {
                    if (((cbPtr->when & when[i]) != 0u)
                        && (pfds[cbPtr->idx].revents & events[i]) != 0) {
                        /* 
                         * Call the Sock_Proc with the SockState flag
                         * combination from when[i]. This is actually the
                         * only place, where a Ns_SockProc is called with a
                         * flag combination in the last argument. If this
                         * would not be the case, we could set the type of
                         * the last parameter of Ns_SockProc to
                         * Ns_SockState.
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
    for (hPtr = Tcl_FirstHashEntry(&table, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
	cbPtr = Tcl_GetHashValue(hPtr);
	if ((cbPtr->when & (unsigned int)NS_SOCK_EXIT) != 0u) {
	    (void) ((*cbPtr->proc)(cbPtr->sock, cbPtr->arg, (unsigned int)NS_SOCK_EXIT));
	}
    }
    /*
     * Clean up the registered callbacks.
     */
    for (hPtr = Tcl_FirstHashEntry(&table, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
	ns_free(Tcl_GetHashValue(hPtr));
    }
    Tcl_DeleteHashTable(&table);

    Ns_Log(Notice, "socks: shutdown complete");

    /*
     * Tell others tht shutdown is complete.
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
 *	Return all defined socket callbacks in form of a valid Tcl list
 *	in the provided Tcl_DString. The passed Tcl_DString has to be
 *	initialized by the caller.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	DString is updated
 *
 *----------------------------------------------------------------------
 */

void
NsGetSockCallbacks(Tcl_DString *dsPtr)
{
    Tcl_HashSearch  search;

    assert(dsPtr != NULL);
    
    Ns_MutexLock(&lock);
    if (running == NS_TRUE) {
        Tcl_HashEntry *hPtr; 

        for (hPtr = Tcl_FirstHashEntry(&table, &search); hPtr != NULL; hPtr = Tcl_NextHashEntry(&search)) {
	    Callback *cbPtr = Tcl_GetHashValue(hPtr);
	    char      buf[TCL_INTEGER_SPACE];

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
            Ns_GetProcInfo(dsPtr, (Ns_Callback *)cbPtr->proc, cbPtr->arg);
            snprintf(buf, sizeof(buf), "%ld:%06ld", cbPtr->timeout.sec, cbPtr->timeout.usec);
            Tcl_DStringAppendElement(dsPtr, buf);
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
