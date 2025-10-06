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
 * event.c --
 *
 *      State machine for event driven socket I/O.
 */

#include "nsd.h"

/*
 * The following structure manages an I/O event callback
 * and its state.
 */

typedef struct Event {
    struct Event      *nextPtr;       /* Next in list of events. */
    NS_SOCKET          sock;          /* Underlying socket. */
    Ns_EventProc      *proc;          /* Event callback. */
    void              *arg;           /* Callback data. */
    NS_POLL_NFDS_TYPE  idx;           /* Poll index. */
    short              events;        /* Poll events. */
    Ns_Time            timeout;       /* Non-null timeout data. */
    unsigned int       status;        /* Manipulated by Ns_EventCallback(). */
} Event;

#define NS_EVENT_WAIT 1u  /* Event callback has requested a wait. */
#define NS_EVENT_DONE 2u  /* Event callback has signaled Event done. */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#  define FLEX /* flexible array */
#else
#  define FLEX 1 /* struct hack fallback */
#endif

/*
 * The following defines an event queue of sockets waiting for
 * I/O events or timeout.
 */

typedef struct EventQueue {
    Event             *firstInitPtr;  /* New events to be initialized. */
    Event             *firstWaitPtr;  /* Sockets waiting for events or timeout. */
    Event             *firstFreePtr;  /* Free, unused Event structs. */
    struct pollfd     *pfds;          /* Array of pollfd structs. */
    NS_SOCKET          trigger[2];    /* Trigger pipe to wake a polling queue. */
    Event              events[FLEX];  /* Array of maxevents Event structs. */
} EventQueue;


/*
 * Local functions defined in this file
 */

#define Call(ep, t, w) ((*((ep)->proc))((Ns_Event *)(ep), (ep)->sock, (ep)->arg, (t), (w)))
#define Push(x, xs) ((x)->nextPtr = (xs), (xs) = (x))


/*
 * Static variables defined in this file.
 */

static const struct {
    Ns_SockState when;  /* Event when bit. */
    const short  event; /* Poll event bit. */
} map[] = {
    {NS_SOCK_EXCEPTION, POLLPRI},
    {NS_SOCK_WRITE,     POLLOUT},
    {NS_SOCK_READ,      POLLIN}
};



/*
 *----------------------------------------------------------------------
 *
 * Ns_CreateEventQueue --
 *
 *      Create a new I/O event queue.
 *
 * Results:
 *      Handle to event queue.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_EventQueue *
Ns_CreateEventQueue(int maxevents)
{
    EventQueue *queuePtr;
    int         i;

    assert(maxevents > 0);

    queuePtr = ns_calloc(1u, sizeof(EventQueue) + (sizeof(Event) * ((size_t)maxevents + 1u)));
    queuePtr->pfds = ns_calloc((size_t)maxevents + 1u, sizeof(struct pollfd));

    if (ns_sockpair(queuePtr->trigger) != 0) {
        Ns_Fatal("taskqueue: ns_sockpair() failed: %s",
                 ns_sockstrerror(ns_sockerrno));
    }
    for (i = 0; i < maxevents; i++) {
        Event  *evPtr = &queuePtr->events[i];
        evPtr->nextPtr = &queuePtr->events[i+1];
    }
    queuePtr->events[maxevents].nextPtr = NULL;
    queuePtr->firstFreePtr = &queuePtr->events[0];

    return (Ns_EventQueue *) queuePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_EventEnqueue --
 *
 *      Add a socket to an event queue.
 *
 * Results:
 *      NS_TRUE if event queued, or NS_FALSE if queue full.
 *
 * Side effects:
 *      Given Ns_EventProc callback will be run later with any of the
 *      following why conditions:
 *
 *      NS_SOCK_INIT       Always called first.
 *      NS_SOCK_READ
 *      NS_SOCK_WRITE
 *      NS_SOCK_EXCEPTION
 *      NS_SOCK_TIMEOUT
 *      NS_SOCK_EXIT       Always called last when queue is shut down.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_EventEnqueue(Ns_EventQueue *queue, NS_SOCKET sock, Ns_EventProc *proc, void *arg)
{
    EventQueue *queuePtr;
    Event      *evPtr;

    NS_NONNULL_ASSERT(queue != NULL);
    NS_NONNULL_ASSERT(proc != NULL);
    NS_NONNULL_ASSERT(arg != NULL);

    queuePtr = (EventQueue *) queue;
    evPtr = queuePtr->firstFreePtr;
    if (evPtr != NULL) {
        queuePtr->firstFreePtr = evPtr->nextPtr;
        evPtr->sock = sock;
        evPtr->proc = proc;
        evPtr->arg = arg;
        Push(evPtr, queuePtr->firstInitPtr);
    }
    return (evPtr != NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_EventCallback --
 *
 *      Update pending conditions and timeout for an event.  This
 *      routine  is expected to be called from within the event
 *      callback proc including to set the initial wait conditions
 *      from within the NS_SOCK_INIT callback.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Event callback will be invoked when ready or on timeout.
 *
 *----------------------------------------------------------------------
 */

void
Ns_EventCallback(Ns_Event *event, Ns_SockState when, const Ns_Time *timeoutPtr)
{
    Event *evPtr;
    int    i;

    NS_NONNULL_ASSERT(event != NULL);
    evPtr = (Event *) event;

    /*
     * Map from sock when bits to poll event bits.
     */

    evPtr->events = 0;
    for (i = 0; i < Ns_NrElements(map); ++i) {
        if (when == map[i].when) {
            evPtr->events |= map[i].event;
        }
    }

    /*
     * Copy timeout, if any.
     */

    if (timeoutPtr != NULL) {
        evPtr->timeout = *timeoutPtr;
    }

    /*
     * Add to the waiting list if there are events or a timeout.
     */

    if (evPtr->events != 0 || timeoutPtr != NULL) {
        evPtr->status = NS_EVENT_WAIT;
    } else {
        evPtr->status = NS_EVENT_DONE;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RunEventQueue --
 *
 *      Run one iteration of event queue callbacks.
 *
 * Results:
 *      NS_TRUE if there are events still on the queue, NS_FALSE
 *      otherwise.
 *
 * Side effects:
 *      Depends on event callbacks.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_RunEventQueue(Ns_EventQueue *queue)
{
    EventQueue       *queuePtr;
    Event            *evPtr, *nextPtr;
    Ns_Time           now;
    const Ns_Time    *timeoutPtr = NULL;
    int               i, n;
    NS_POLL_NFDS_TYPE nfds;
    char              c;

    NS_NONNULL_ASSERT(queue != NULL);
    queuePtr = (EventQueue *) queue;

    /*
     * Process any new events.
     */
    Ns_GetTime(&now);

    while ((evPtr = queuePtr->firstInitPtr) != NULL) {
        queuePtr->firstInitPtr = evPtr->nextPtr;
        Call(evPtr, &now, NS_SOCK_INIT);
        if (evPtr->status == 0u) {
            Ns_Log(Bug, "Ns_RunEventQueue: callback init failed");
            Push(evPtr, queuePtr->firstFreePtr);
        }
    }

    /*
     * Determine minimum timeout, and set the pollfd structs for
     * all waiting events.
     */

    queuePtr->pfds[0].fd = queuePtr->trigger[0];
    queuePtr->pfds[0].events = (short)POLLIN;
    queuePtr->pfds[0].revents = 0;
    nfds = 1;

    evPtr = queuePtr->firstWaitPtr;

    while (evPtr != NULL) {
        evPtr->idx = nfds;
        queuePtr->pfds[nfds].fd = evPtr->sock;
        queuePtr->pfds[nfds].events = evPtr->events;
        queuePtr->pfds[nfds].revents = 0;
        if (evPtr->timeout.sec > 0 || evPtr->timeout.usec > 0) {
            if (timeoutPtr == NULL
                || Ns_DiffTime(&evPtr->timeout, timeoutPtr, NULL) < 0) {
                timeoutPtr = &evPtr->timeout;
            }
        }
        nfds++;
        evPtr = evPtr->nextPtr;
    }

    /*
     * Poll sockets and drain the trigger pipe if necessary.
     */

    n = NsPoll(queuePtr->pfds, nfds, timeoutPtr);
    /*
     * n is currently not used; n is either number of ready descriptors, or 0
     * on timeout, or -1 on error
     */
    ((void)(n)); /* ignore n */

    if (((queuePtr->pfds[0].revents & POLLIN) != 0)
        && (recv(queuePtr->pfds[0].fd, &c, 1, 0) != 1)
        ) {
        Ns_Fatal("event queue: trigger ns_read() failed: %s",
                 ns_sockstrerror(ns_sockerrno));
    }

    /*
     * Execute any ready events or timeouts for waiting tasks.
     */

    Ns_GetTime(&now);
    evPtr = queuePtr->firstWaitPtr;
    queuePtr->firstWaitPtr = NULL;

    while (evPtr != NULL) {
        short revents;

        nextPtr = evPtr->nextPtr;

        /*
         * NB: Treat POLLHUP as POLLIN on systems which return it.
         */

        revents = queuePtr->pfds[evPtr->idx].revents;
        if ((revents & POLLHUP) != 0) {
            revents |= (short)POLLIN;
        }
        if (revents != 0) {
            for (i = 0; i < Ns_NrElements(map); ++i) {
                if ((revents & map[i].event) != 0) {
                    Call(evPtr, &now, map[i].when);
                }
            }
        } else if ((evPtr->timeout.sec > 0 || evPtr->timeout.usec > 0)
                   && Ns_DiffTime(&evPtr->timeout, &now, NULL) < 0) {
            Call(evPtr, &now, NS_SOCK_TIMEOUT);
        }

        if (evPtr->status == NS_EVENT_WAIT) {
            Push(evPtr, queuePtr->firstWaitPtr);
        } else {
            Push(evPtr, queuePtr->firstFreePtr);
        }
        evPtr = nextPtr;
    }

    return (queuePtr->firstWaitPtr != NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TriggerEventQueue --
 *
 *      Wake an event queue.
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
Ns_TriggerEventQueue(const Ns_EventQueue *queue)
{
    const EventQueue *queuePtr;

    NS_NONNULL_ASSERT(queue != NULL);
    queuePtr = (const EventQueue *) queue;

    if (send(queuePtr->trigger[1], NS_EMPTY_STRING, 1, 0) != 1) {
        Ns_Fatal("event queue: trigger send() failed: %s",
                 ns_sockstrerror(ns_sockerrno));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ExitEventQueue --
 *
 *      Call exit for all remaining events in a queue.
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
Ns_ExitEventQueue(Ns_EventQueue *queue)
{
    EventQueue *queuePtr;
    Event      *evPtr;
    Ns_Time     now;

    NS_NONNULL_ASSERT(queue != NULL);
    queuePtr = (EventQueue *) queue;

    Ns_GetTime(&now);
    evPtr = queuePtr->firstWaitPtr;
    queuePtr->firstWaitPtr = NULL;
    while (evPtr != NULL) {
        Call(evPtr, &now, NS_SOCK_EXIT);
        evPtr = evPtr->nextPtr;
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
