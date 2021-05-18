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
 * sched.c --
 *
 *  Support for the background task and scheduled procedure interfaces.  The
 *  implementation of the priority queue based on a binary heap. A binary heap
 *  has the following characteristics:
 *
 *   - Cost of insertion:                O(log N)
 *   - Cost of deletion:                 O(log N)
 *   - Cost of change of key value:      O(log N) (not used here)
 *   - Cost of smallest (largest) value: O(1)
 *
 *  The binary heap code is based on:
 *
 *      "Chapter 9. Priority Queues and Heapsort", Sedgewick "Algorithms
 *       in C, 3rd Edition", Addison-Wesley, 1998.
 *
 *       https://algs4.cs.princeton.edu/24pq/
 */

#include "nsd.h"

/*
 * The following two defines can be used to turn on consistency checking and
 * intense tracing of the scheduling/unscheduling of the commands.
 *
 * #define NS_SCHED_CONSISTENCY_CHECK
 * #define NS_SCHED_TRACE_EVENTS
 */

/*
 * The following structure defines a scheduled event.
 */

typedef struct Event {
    struct Event   *nextPtr;
    Tcl_HashEntry  *hPtr;       /* Entry in event hash or NULL if deleted. */
    int             id;         /* Unique event id. */
    int             qid;        /* Current priority queue id. */
    Ns_Time         nextqueue;  /* Next time to queue for run. */
    Ns_Time         lastqueue;  /* Last time queued for run. */
    Ns_Time         laststart;  /* Last time run started. */
    Ns_Time         lastend;    /* Last time run finished. */
    Ns_Time         interval;   /* Interval specification. */
    Ns_Time         scheduled;  /* The scheduled time. */
    Ns_SchedProc   *proc;       /* Procedure to execute. */
    void           *arg;        /* Client data for procedure. */
    Ns_SchedProc   *deleteProc; /* Procedure to cleanup when done (if any). */
    unsigned int    flags;      /* One or more of NS_SCHED_ONCE, NS_SCHED_THREAD,
                                 * NS_SCHED_DAILY, or NS_SCHED_WEEKLY. */
} Event;

/*
 * Local functions defined in this file.
 */

static Ns_ThreadProc SchedThread;       /* Detached event firing thread. */
static Ns_ThreadProc EventThread;       /* Proc for NS_SCHED_THREAD events. */
static Event *DeQueueEvent(int k);      /* Remove event from heap. */
static void FreeEvent(Event *ePtr)      /* Free completed or cancelled event. */
    NS_GNUC_NONNULL(1);
static void QueueEvent(Event *ePtr)     /* Queue event on heap. */
    NS_GNUC_NONNULL(1);
static void Exchange(int i, int j);     /* Exchange elements in the global queue */
static bool Larger(int j, int k);       /* Function defining the sorting
                                           criterium of the binary heap */


/*
 * Static variables defined in this file.
 */

static Tcl_HashTable eventsTable;   /* Hash table of events. */
static Ns_Mutex lock;               /* Lock around heap and hash table. */
static Ns_Cond schedcond;           /* Condition to wakeup SchedThread. */
static Ns_Cond eventcond;           /* Condition to wakeup EventThread(s). */
static Event **queue = NULL;        /* Heap priority queue (dynamically re-sized). */
static Event *firstEventPtr = NULL; /* Pointer to the first event */
static int nqueue = 0;              /* Number of events in queue. */
static int maxqueue = 0;            /* Max queue events (dynamically re-sized). */

static int nThreads = 0;            /* Total number of running threads */
static int nIdleThreads = 0;        /* Number of idle threads */

static bool running = NS_FALSE;
static bool shutdownPending = NS_FALSE;
static Ns_Thread schedThread;


/*
 *----------------------------------------------------------------------
 *
 * Exchange --
 *
 *     Helper function to exchange two events in the global queue,
 *     used in QueueEvent() and DeQueueEvent().
 *
 * Results:
 *     None.
 *
 * Side effects:
 *     Queue elements flipped.
 *
 *----------------------------------------------------------------------
 */

static void Exchange(int i, int j) {
    Event *tmp = queue[i];

    queue[i] = queue[j];
    queue[j] = tmp;
    queue[i]->qid = i;
    queue[j]->qid = j;
}


/*
 *----------------------------------------------------------------------
 *
 * NsInitSched --
 *
 *  Initialize scheduler API.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

void
NsInitSched(void)
{
    Ns_MutexInit(&lock);
    Ns_MutexSetName(&lock, "ns:sched");
    Tcl_InitHashTable(&eventsTable, TCL_ONE_WORD_KEYS);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_After --
 *
 *  Schedule a one-shot event after the specified delay in seconds.
 *
 * Results:
 *  Event id or NS_ERROR if delay is out of range.
 *
 * Side effects:
 *  See Ns_ScheduleProcEx().
 *
 *----------------------------------------------------------------------
 */

int
Ns_After(const Ns_Time *interval, Ns_SchedProc *proc, void *arg, ns_funcptr_t deleteProc)
{
    int result;

    NS_NONNULL_ASSERT(proc != NULL);
    NS_NONNULL_ASSERT(interval != NULL);

    if (interval->sec < 0 || interval->usec < 0) {
        result = (int)NS_ERROR;
    } else {
        result = Ns_ScheduleProcEx(proc, arg, NS_SCHED_ONCE, interval, (Ns_SchedProc *)deleteProc);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ScheduleProc --
 *
 *  Schedule a proc to run at a given interval.
 *
 * Results:
 *  Event id or NS_ERROR if interval is invalid.
 *
 * Side effects:
 *  See Ns_ScheduleProcEx().
 *
 *----------------------------------------------------------------------
 */

int
Ns_ScheduleProc(Ns_SchedProc *proc, void *arg, int thread, int secs)
{
    Ns_Time interval;

    NS_NONNULL_ASSERT(proc != NULL);

    interval.sec = secs;
    interval.usec = 0;
    return Ns_ScheduleProcEx(proc, arg, (thread != 0) ? NS_SCHED_THREAD : 0u,
                             &interval, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ScheduleDaily --
 *
 *  Schedule a proc to run once a day.
 *
 * Results:
 *  Event id or NS_ERROR if hour and/or minute is out of range.
 *
 * Side effects:
 *  See Ns_ScheduleProcEx
 *
 *----------------------------------------------------------------------
 */

int
Ns_ScheduleDaily(Ns_SchedProc *proc, void *clientData, unsigned int flags,
                 int hour, int minute, Ns_SchedProc *cleanupProc)
{
    int result;

    NS_NONNULL_ASSERT(proc != NULL);

    if (hour > 23 || hour < 0 || minute > 59 || minute < 0) {
        result = (int)NS_ERROR;
    } else {
        Ns_Time interval;

        interval.sec = (hour * 3600) + (minute * 60);
        interval.usec = 0;
        result = Ns_ScheduleProcEx(proc, clientData, flags | NS_SCHED_DAILY,
                                   &interval, cleanupProc);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ScheduleWeekly --
 *
 *  Schedule a proc to run once a week.
 *
 * Results:
 *  Event id or NS_ERROR if day, hour, and/or minute is out of range.
 *
 * Side effects:
 *  See Ns_ScheduleProcEx
 *
 *----------------------------------------------------------------------
 */

int
Ns_ScheduleWeekly(Ns_SchedProc *proc, void *clientData, unsigned int flags,
    int day, int hour, int minute, Ns_SchedProc *cleanupProc)
{
    int result;

    NS_NONNULL_ASSERT(proc != NULL);

    if (day < 0 || day > 6 || hour > 23 || hour < 0 || minute > 59 || minute < 0) {
        result = (int)NS_ERROR;
    } else {
        Ns_Time interval;

        interval.sec = (((day * 24) + hour) * 3600) + (minute * 60);
        interval.usec = 0;
        result = Ns_ScheduleProcEx(proc, clientData, flags | NS_SCHED_WEEKLY,
                                   &interval, cleanupProc);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ScheduleProcEx --
 *
 *  Schedule a proc to run at a given interval.  The interpretation
 *  of interval (whether iterative, daily, or weekly) is handled
 *  by QueueEvent.
 *
 * Results:
 *  Event ID or NS_ERROR when interval is out of range.
 *
 * Side effects:
 *  Event is allocated, hashed, and queued.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ScheduleProcEx(Ns_SchedProc *proc, void *clientData, unsigned int flags,
                  const Ns_Time *interval, Ns_SchedProc *cleanupProc)
{
    int id;

    NS_NONNULL_ASSERT(proc != NULL);
    NS_NONNULL_ASSERT(interval != NULL);

    if (unlikely(interval->sec < 0 || interval->usec < 0)) {
        id = (int)NS_ERROR;

    } else {
        Event    *ePtr;
        int       isNew;
        Ns_Time   now;

        Ns_GetTime(&now);
        ePtr = ns_malloc(sizeof(Event));
        ePtr->flags = flags;
        ePtr->nextqueue.sec = 0;
        ePtr->nextqueue.usec = 0;
        ePtr->lastqueue.sec = ePtr->laststart.sec = ePtr->lastend.sec = -1;
        ePtr->lastqueue.usec = ePtr->laststart.usec = ePtr->lastend.usec = 0;
        ePtr->interval = *interval;
        ePtr->proc = proc;
        ePtr->deleteProc = cleanupProc;
        ePtr->arg = clientData;

        Ns_MutexLock(&lock);
        if (shutdownPending) {
            id = (int)NS_ERROR;
            ns_free(ePtr);
        } else {
            do {
                static int nextId = 0;

                id = nextId++;
                if (nextId < 0) {
                    nextId = 0;
                }
                ePtr->hPtr = Tcl_CreateHashEntry(&eventsTable, INT2PTR(id), &isNew);
            } while (isNew == 0);
            Tcl_SetHashValue(ePtr->hPtr, ePtr);
            ePtr->id = id;
            ePtr->scheduled = now;
            QueueEvent(ePtr);
        }
        Ns_MutexUnlock(&lock);
    }

    return id;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Cancel, Ns_UnscheduleProc --
 *
 *      Cancel a previously scheduled event.
 *
 * Results:
 *      Ns_UnscheduleProc:  None.
 *      Ns_Cancel:          NS_TRUE if cancelled, NS_FALSE otherwise.
 *
 * Side effects:
 *      See FreeEvent().
 *
 *----------------------------------------------------------------------
 */

void
Ns_UnscheduleProc(int id)
{
    (void) Ns_Cancel(id);
}

bool
Ns_Cancel(int id)
{
    Event *ePtr = NULL;
    bool   cancelled = NS_FALSE;

    Ns_MutexLock(&lock);
    if (!shutdownPending) {
        Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&eventsTable, INT2PTR(id));

        if (hPtr != NULL) {
            ePtr = Tcl_GetHashValue(hPtr);
            Tcl_DeleteHashEntry(hPtr);
            ePtr->hPtr = NULL;
            if (ePtr->qid > 0) {
                (void) DeQueueEvent(ePtr->qid);
                cancelled = NS_TRUE;
            }
        }
    }
    Ns_MutexUnlock(&lock);
    if (cancelled) {
        FreeEvent(ePtr);
    }
    return cancelled;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Pause --
 *
 *  Pause a schedule procedure.
 *
 * Results:
 *  NS_TRUE if proc paused, NS_FALSE otherwise.
 *
 * Side effects:
 *  Proc will not run at the next scheduled time.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_Pause(int id)
{
    bool paused = NS_FALSE;

    Ns_MutexLock(&lock);
    if (!shutdownPending) {
        const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&eventsTable, INT2PTR(id));

        if (hPtr != NULL) {
            Event *ePtr;

            ePtr = Tcl_GetHashValue(hPtr);
            if ((ePtr->flags & NS_SCHED_PAUSED) == 0u) {
                ePtr->flags |= NS_SCHED_PAUSED;
                if (ePtr->qid > 0) {
                    (void) DeQueueEvent(ePtr->qid);
                }
                paused = NS_TRUE;
            }
        }
    }
    Ns_MutexUnlock(&lock);
    return paused;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Resume --
 *
 *  Resume a scheduled proc.
 *
 * Results:
 *  NS_TRUE if proc resumed, NS_FALSE otherwise.
 *
 * Side effects:
 *  Proc will be rescheduled.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_Resume(int id)
{
    bool resumed = NS_FALSE;

    Ns_MutexLock(&lock);
    if (!shutdownPending) {
        const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&eventsTable, INT2PTR(id));

        if (hPtr != NULL) {
            Event *ePtr;

            ePtr = Tcl_GetHashValue(hPtr);
            if ((ePtr->flags & NS_SCHED_PAUSED) != 0u) {
                Ns_Time now;

                ePtr->flags &= ~NS_SCHED_PAUSED;
                Ns_GetTime(&now);
                ePtr->scheduled = now;
                QueueEvent(ePtr);
                resumed = NS_TRUE;
            }
        }
    }
    Ns_MutexUnlock(&lock);

    return resumed;
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartSchedShutdown, NsWaitSchedShutdown --
 *
 *  Inititiate and then wait for sched shutdown.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  May timeout waiting for sched shutdown.
 *
 *----------------------------------------------------------------------
 */

void
NsStartSchedShutdown(void)
{
    Ns_MutexLock(&lock);
    if (running) {
        Ns_Log(Notice, "sched: shutdown pending");
        shutdownPending = NS_TRUE;
        Ns_CondSignal(&schedcond);
    }
    Ns_MutexUnlock(&lock);
}

void
NsWaitSchedShutdown(const Ns_Time *toPtr)
{
    Ns_ReturnCode status;

    Ns_MutexLock(&lock);
    status = NS_OK;
    while (status == NS_OK && running) {
        status = Ns_CondTimedWait(&schedcond, &lock, toPtr);
    }
    Ns_MutexUnlock(&lock);
    if (status != NS_OK) {
        Ns_Log(Warning, "sched: timeout waiting for sched exit");
    } else if (schedThread != NULL) {
        Ns_ThreadJoin(&schedThread, NULL);
    }
}

static bool
Larger(int j, int k)
{
    return (Ns_DiffTime(&queue[j]->nextqueue, &queue[k]->nextqueue, NULL) == 1);
}


#ifndef NS_SCHED_CONSISTENCY_CHECK
static void QueueConsistencyCheck(const char *UNUSED(startMsg), int UNUSED(n), bool UNUSED(runAsserts)) {
}
#else


static void
QueueConsistencyCheck(const char *startMsg, int n, bool runAsserts)
{
    int          k;

    Ns_Log(Notice, "=== %s (%d) ", startMsg, n);

#ifdef NS_SCHED_TRACE_EVENTS
    Event      *ePtr;
    Tcl_DString ds;
    time_t      s;

    Tcl_DStringInit(&ds);

    Ns_DStringPrintf(&ds, "=== %s (%d) ", startMsg, n);
    if (n > 1) {
        s = queue[1]->nextqueue.sec;
    }
    for (k = 1; k <= n; k++) {
        ePtr = queue[k];
        Ns_DStringPrintf(&ds, "[%d] (%p id %d qid %d " NS_TIME_FMT ")  ",
                         k, (void*)ePtr, ePtr->id, ePtr->qid,
                         (int64_t)ePtr->nextqueue.sec, ePtr->nextqueue.usec);
    }
    Ns_Log(Notice, "%s", ds.string);
    Tcl_DStringFree(&ds);
#endif

    /*
     * Check if all parent nodes (k/2) are earlier then the child nodes.
     */
    for (k = 2; k <= n; k++) {
        int  j  = k/2;
        bool ok = !Larger(j, k);

        if (!ok) {
            Ns_Log(Error, "=== %s: parent node [%d] (id %d " NS_TIME_FMT
                   ") is later than child [%d] (id %d " NS_TIME_FMT ")",
                   startMsg,
                   j, queue[j]->id, (int64_t)queue[j]->nextqueue.sec, queue[j]->nextqueue.usec,
                   k, queue[k]->id, (int64_t)queue[k]->nextqueue.sec, queue[k]->nextqueue.usec);
            if (runAsserts) {
                assert(ok);
            }
        }
    }

    /*
     * Check whether all qids correspond to the array position.
     */
    for (k = 1; k <= n; k++) {
        if (queue[k]->qid != k) {
            Ns_Log(Error, "=== %s inconsistent qid on pos %d (id %d): is %d, should be %d",
                   startMsg, k, queue[k]->id, queue[k]->qid, k);
            if (runAsserts) {
                assert(queue[k]->qid == k);
            }
        }
    }
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * QueueEvent --
 *
 *  Add an event to the priority queue heap.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  SchedThread() may be created and/or signaled.
 *
 *----------------------------------------------------------------------
 */

static void
QueueEvent(Event *ePtr)
{
    long d;

    if ((ePtr->flags & NS_SCHED_PAUSED) == 0u) {
        /*
         * Calculate the time from now in seconds this event should run.
         */
        if ((ePtr->flags & (NS_SCHED_DAILY | NS_SCHED_WEEKLY)) != 0u) {
            struct tm  *tp;
            time_t      secs = ePtr->scheduled.sec;

            tp = ns_localtime(&secs);
            tp->tm_sec = (int)ePtr->interval.sec;
            tp->tm_hour = 0;
            tp->tm_min = 0;
            if ((ePtr->flags & NS_SCHED_WEEKLY) != 0u) {
                tp->tm_mday -= tp->tm_wday;
            }
            ePtr->nextqueue.sec = mktime(tp);
            ePtr->nextqueue.usec = 0;
            d = Ns_DiffTime(&ePtr->nextqueue, &ePtr->scheduled, NULL);
            Ns_Log(Debug, "SCHED_DAILY: scheduled " NS_TIME_FMT " next " NS_TIME_FMT
                   " diff %ld secdiff %ld",
                   (int64_t)ePtr->scheduled.sec, ePtr->scheduled.usec,
                   (int64_t)ePtr->nextqueue.sec, ePtr->nextqueue.usec,
                   d, (long)ePtr->nextqueue.sec-(long)ePtr->scheduled.sec);

            if (d <= 0) {
                tp->tm_mday += ((ePtr->flags & NS_SCHED_WEEKLY) != 0u) ? 7 : 1;
                ePtr->nextqueue.sec = mktime(tp);
                ePtr->nextqueue.usec = 0;
                Ns_Log(Debug, "SCHED_DAILY: final next " NS_TIME_FMT ,
                       (int64_t)ePtr->nextqueue.sec, ePtr->nextqueue.usec);
            }
            ePtr->scheduled = ePtr->nextqueue;
        } else {
            Ns_Time diff, now;

            ePtr->nextqueue = ePtr->scheduled;
            Ns_IncrTime(&ePtr->nextqueue, ePtr->interval.sec, ePtr->interval.usec);
            /*
             * The update time is the next scheduled time.
             */
            ePtr->scheduled = ePtr->nextqueue;

            Ns_GetTime(&now);
            d = Ns_DiffTime(&ePtr->nextqueue, &now, &diff);
            Ns_Log(Debug, "sched: compute next run time based on: scheduled " NS_TIME_FMT
                   " diff %ld",
                   (int64_t)ePtr->scheduled.sec, ePtr->scheduled.usec, d);

            if (d == -1) {
                /*
                 * The last execution took longer than the schedule
                 * interval. Re-schedule after 10ms.
                 */
                ePtr->nextqueue = now;
                Ns_IncrTime(&ePtr->nextqueue, 0, 10000);
                Ns_Log(Warning, "sched id %d: last execution overlaps with scheduled exection; "
                       "running late", ePtr->id);
            }
        }

        ePtr->qid = ++nqueue;
        /*
         * The queue array is extended if necessary.
         */
        if (maxqueue <= nqueue) {
            maxqueue += 25;
            queue = ns_realloc(queue, sizeof(Event *) * ((size_t)maxqueue + 1u));
        }
        /*
         * Place the new event at the end of the queue array.
         */
        queue[nqueue] = ePtr;

        if (nqueue > 1) {
            int j, k;

            QueueConsistencyCheck("Queue event", nqueue - 1, NS_FALSE);

            /*
             * Bottom-up reheapify: swim up" in the heap.  When a node is
             * larger than its parent, then the nodes have to swapped.
             *
             * In the implementation below, "j" is always k/2 and represents
             * the parent node in the binary tree.
             */
            k = nqueue;
            j = k / 2;
            while (k > 1 && Larger(j, k)) {
                Exchange(j, k);
                k = j;
                j = k / 2;
            }
            QueueConsistencyCheck("Queue event end", nqueue, NS_TRUE);
        }
        Ns_Log(Debug, "QueueEvent (id %d qid %d " NS_TIME_FMT ")",
               ePtr->id, ePtr->qid,
               (int64_t)ePtr->nextqueue.sec, ePtr->nextqueue.usec);

        /*
         * Signal or create the SchedThread if necessary.
         */

        if (running) {
            Ns_CondSignal(&schedcond);
        } else {
            running = NS_TRUE;
            Ns_ThreadCreate(SchedThread, NULL, 0, &schedThread);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * DeQueueEvent --
 *
 *  Remove an event from the priority queue heap.
 *
 * Results:
 *  Pointer to removed event.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static Event *
DeQueueEvent(int k)
{
    Event *ePtr;

    Ns_Log(Debug, "DeQueueEvent (id %d qid %d " NS_TIME_FMT ")",
           queue[k]->id, k,
           (int64_t)queue[k]->nextqueue.sec, queue[k]->nextqueue.usec);

    QueueConsistencyCheck("Dequeue event start", nqueue, NS_TRUE);

    /*
     * Remove an element qid (named k in Sedgewick) from the priority queue.
     *
     * 1) Exchange element to be deleted with the node at the end. Now, the
     *    element will violate in most cases the heap order.
     * 2) Sink down the element.
     */

    Exchange(k, nqueue);
    ePtr = queue[nqueue--];
    ePtr->qid = 0;

    for (;;) {
        int j =  2 * k;

        if (j > nqueue) {
            break;
        }

        if (j < nqueue && Larger(j, j+1)) {
            ++j;
        }

        if (!Larger(k, j)) {
            break;
        }
        Exchange(k, j);
        k = j;
    }
    QueueConsistencyCheck("Dequeue event end", nqueue, NS_TRUE);

    return ePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * EventThread --
 *
 *  Run detached thread events.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  See FinishEvent().
 *
 *----------------------------------------------------------------------
 */

static void
EventThread(void *arg)
{
    Ns_Time   now;
    Event    *ePtr;
    int       jpt, njobs;
    uintptr_t jobId;

    jpt = njobs = nsconf.sched.jobsperthread;
    jobId = 0u;

    Ns_ThreadSetName("-sched:idle%" PRIuPTR "-", (uintptr_t)arg);
    Ns_Log(Notice, "starting");

    Ns_MutexLock(&lock);
    while (jpt == 0 || njobs > 0) {
        while (firstEventPtr == NULL && !shutdownPending) {
            Ns_CondWait(&eventcond, &lock);
        }
        if (firstEventPtr == NULL) {
            break;
        }
        ePtr = firstEventPtr;
        firstEventPtr = ePtr->nextPtr;
        if (firstEventPtr != NULL) {
            Ns_CondSignal(&eventcond);
        }
        --nIdleThreads;
        Ns_MutexUnlock(&lock);

        Ns_ThreadSetName("-sched:%" PRIuPTR ":%" PRIuPTR ":%d-",
                         (uintptr_t)arg, ++jobId, ePtr->id);
        (*ePtr->proc) (ePtr->arg, ePtr->id);
        Ns_ThreadSetName("-sched:idle%" PRIuPTR "-", (uintptr_t)arg);
        Ns_GetTime(&now);

        Ns_MutexLock(&lock);
        ++nIdleThreads;
        if (ePtr->hPtr == NULL) {
            Ns_MutexUnlock(&lock);
            FreeEvent(ePtr);
            Ns_MutexLock(&lock);
        } else {
            ePtr->flags &= ~NS_SCHED_RUNNING;
            ePtr->lastend = now;
            /*
             * EventThread triggers QueueEvent() based on lastqueue.
             */
            Ns_Log(Debug, "QueueEvent (%d) based on lastqueue "NS_TIME_FMT" or nextqueue "NS_TIME_FMT,
                   ePtr->id,
                   (int64_t)ePtr->lastqueue.sec, ePtr->lastqueue.usec,
                   (int64_t)ePtr->nextqueue.sec, ePtr->nextqueue.usec
                   );
            QueueEvent(ePtr);
        }
        /* Served given # of jobs in this thread */
        if (jpt != 0 && --njobs <= 0) {
            break;
        }
    }
    --nThreads;
    --nIdleThreads;
    Ns_Log(Notice, "exiting, %d threads, %d idle", nThreads, nIdleThreads);

    Ns_CondSignal(&schedcond);
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * FreeEvent --
 *
 *  Free and event after run.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Event is freed or re-queued.
 *
 *----------------------------------------------------------------------
 */

static void
FreeEvent(Event *ePtr)
{
    NS_NONNULL_ASSERT(ePtr != NULL);

    if (ePtr->deleteProc != NULL) {
        (*ePtr->deleteProc) (ePtr->arg, ePtr->id);
    }
    ns_free(ePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * SchedThread --
 *
 *  Detached thread to fire events on time.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Depends on event procedures.
 *
 *----------------------------------------------------------------------
 */

static void
SchedThread(void *UNUSED(arg))
{
    Ns_Time         now;
    Ns_Time         timeout = {0, 0};
    Event          *ePtr, *readyPtr = NULL;

    (void) Ns_WaitForStartup();

    Ns_ThreadSetName("-sched-");
    Ns_Log(Notice, "sched: starting");

    Ns_MutexLock(&lock);
    while (!shutdownPending) {

        /*
         * For events ready to run, either create a thread for
         * detached events or add to a list of synchronous events.
         */

        Ns_GetTime(&now);
        while (nqueue > 0 && Ns_DiffTime(&queue[1]->nextqueue, &now, NULL) <= 0) {
            ePtr = DeQueueEvent(1);

#ifdef NS_SCHED_TRACE_EVENTS
            Ns_Log(Notice, "... dequeue event (id %d) " NS_TIME_FMT,
                   ePtr->id,
                   (int64_t)ePtr->nextqueue.sec, ePtr->nextqueue.usec);
#endif
            if ((ePtr->flags & NS_SCHED_ONCE) != 0u) {
                Tcl_DeleteHashEntry(ePtr->hPtr);
                ePtr->hPtr = NULL;
            }
            ePtr->lastqueue = now;
            if ((ePtr->flags & NS_SCHED_THREAD) != 0u) {
                ePtr->flags |= NS_SCHED_RUNNING;
                ePtr->laststart = now;
                ePtr->nextPtr = firstEventPtr;
                firstEventPtr = ePtr;
            } else {
                ePtr->nextPtr = readyPtr;
                readyPtr = ePtr;
            }
        }

#ifdef NS_SCHED_TRACE_EVENTS
        if (readyPtr != NULL || firstEventPtr != NULL) {
            Ns_Log(Notice, "... dequeuing done ready %p ready-nextPtr %p first %p",
                   (void*)readyPtr, (void*)(readyPtr ? readyPtr->nextPtr : NULL),
                   (void*)firstEventPtr);
        }
#endif

        /*
         * Dispatch any threaded events.
         */

        if (firstEventPtr != NULL) {
            if (nIdleThreads == 0) {
                Ns_ThreadCreate(EventThread, INT2PTR(nThreads), 0, NULL);
                ++nIdleThreads;
                ++nThreads;
            }
            Ns_CondSignal(&eventcond);
        }

        /*
         * Run and re-queue or free synchronous events.
         */

        while ((ePtr = readyPtr) != NULL) {
            Ns_Time diff;

            readyPtr = ePtr->nextPtr;
            ePtr->laststart = now;
            ePtr->flags |= NS_SCHED_RUNNING;
            Ns_MutexUnlock(&lock);
            (*ePtr->proc) (ePtr->arg, ePtr->id);
            Ns_GetTime(&now);

            (void)Ns_DiffTime(&ePtr->laststart, &now, &diff);
            if (Ns_DiffTime(&diff, &nsconf.sched.maxelapsed, NULL) == 1) {
                Ns_Log(Warning, "sched: excessive time taken by proc %d (" NS_TIME_FMT " seconds)",
                       ePtr->id, (int64_t)diff.sec, diff.usec);
            }
            if (ePtr->hPtr == NULL) {
                FreeEvent(ePtr);
                ePtr = NULL;
            }
            Ns_MutexLock(&lock);
            if (ePtr != NULL) {
                ePtr->flags &= ~NS_SCHED_RUNNING;
                ePtr->lastend = now;
                /*
                 * Base repeating thread on the last queue time, and not on
                 * the last endtime to avoid a growing timeshift for events
                 * that should run at fixed intervals.
                 *
                 * SchedThread triggers QueueEvent() based on lastqueue.
                 */
                Ns_Log(Debug, "QueueEvent (%d) based on lastqueue", ePtr->id);
                QueueEvent(ePtr);
            }
        }

        /*
         * Wait for the next ready event.
         */
        if (nqueue == 0) {
            Ns_CondWait(&schedcond, &lock);
        } else if (!shutdownPending) {
            timeout = queue[1]->nextqueue;
            (void) Ns_CondTimedWait(&schedcond, &lock, &timeout);
        }

    }

    /*
     * Wait for any detached event threads to exit
     * and then cleanup the scheduler and signal
     * shutdown complete.
     */

    Ns_Log(Notice, "sched: shutdown started");
    if (nThreads > 0) {
        Ns_Log(Notice, "sched: waiting for %d/%d event threads...",
               nThreads, nIdleThreads);
        Ns_CondBroadcast(&eventcond);
        while (nThreads > 0) {
            (void) Ns_CondTimedWait(&schedcond, &lock, &timeout);
        }
    }
    Ns_MutexUnlock(&lock);
    while (nqueue > 0) {
        FreeEvent(queue[nqueue--]);
    }
    ns_free(queue);
    Tcl_DeleteHashTable(&eventsTable);
    Ns_Log(Notice, "sched: shutdown complete");

    Ns_MutexLock(&lock);
    running = NS_FALSE;
    Ns_CondBroadcast(&schedcond);
    Ns_MutexUnlock(&lock);
}


void
NsGetScheduled(Tcl_DString *dsPtr)
{
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MutexLock(&lock);
    hPtr = Tcl_FirstHashEntry(&eventsTable, &search);
    while (hPtr != NULL) {
        const Event *ePtr = Tcl_GetHashValue(hPtr);

        Tcl_DStringStartSublist(dsPtr);
        Ns_DStringPrintf(dsPtr, "%d %d ", ePtr->id, ePtr->flags);
        Ns_DStringAppendTime(dsPtr, &ePtr->interval);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &ePtr->nextqueue);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &ePtr->lastqueue);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &ePtr->laststart);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_DStringAppendTime(dsPtr, &ePtr->lastend);
        Tcl_DStringAppend(dsPtr, " ", 1);
        Ns_GetProcInfo(dsPtr, (ns_funcptr_t)ePtr->proc, ePtr->arg);
        Tcl_DStringEndSublist(dsPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
    Ns_MutexUnlock(&lock);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
