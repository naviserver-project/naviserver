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
 * task.c --
 *
 *      Support socket for I/O tasks.
 */

#include "nsd.h"

/*
 * The following defines a task queue.
 */

typedef struct TaskQueue {
    struct TaskQueue  *nextPtr;           /* Next in list of all queues. */
    struct Task       *firstSignalPtr;    /* First in list of task signals. */
    Ns_Thread          tid;               /* Thread id. */
    Ns_Mutex           lock;              /* Queue list and signal lock. */
    Ns_Cond            cond;              /* Task and queue signal condition. */
    bool               shutdown;          /* Shutdown flag. */
    bool               stopped;           /* Stop flag. */
    NS_SOCKET          trigger[2];        /* Trigger pipe. */
    char               name[1];           /* String name. */
} TaskQueue;

/*
 * The following bits are used to send signals to a task queue
 * and manage the state tasks.
 */

#define TASK_INIT           0x01u
#define TASK_CANCEL         0x02u
#define TASK_WAIT           0x04u
#define TASK_TIMEOUT        0x08u
#define TASK_DONE           0x10u
#define TASK_PENDING        0x20u

/*
 * The following defines a task.
 */

typedef struct Task {
    struct TaskQueue  *queuePtr;      /* Monitoring queue. */
    struct Task       *nextWaitPtr;   /* Next on wait queue. */
    struct Task       *nextSignalPtr; /* Next on signal queue. */
    NS_SOCKET          sock;          /* Underlying socket. */
    Ns_TaskProc       *proc;          /* Task callback. */
    void              *arg;           /* Callback data. */
    NS_POLL_NFDS_TYPE  idx;           /* Poll index. */
    short              events;        /* Poll events. */
    Ns_Time            timeout;       /* Non-null timeout data. */
    unsigned int       signalFlags;   /* Signal bits sent to/from queue thread. */
    unsigned int       flags;         /* Flags private to queue. */
} Task;

/*
 * Local functions defined in this file
 */

static void TriggerQueue(const TaskQueue *queuePtr)      NS_GNUC_NONNULL(1);
static void JoinQueue(TaskQueue *queuePtr)               NS_GNUC_NONNULL(1);
static void StopQueue(TaskQueue *queuePtr)               NS_GNUC_NONNULL(1);
static bool SignalQueue(Task *taskPtr, unsigned int bit) NS_GNUC_NONNULL(1);
static void RunTask(Task *taskPtr, short revents, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static Ns_ThreadProc TaskThread;

#define Call(tp,w) ((*((tp)->proc))((Ns_Task *)(tp),(tp)->sock,(tp)->arg,(w)))

/*
 * Static variables defined in this file
 */

static TaskQueue *firstQueuePtr; /* List of all queues.  */
static Ns_Mutex   lock;          /* Lock for queue list. */

/*
 * The following maps sock "when" bits to poll event bits.
 * The order is significant and determines the order of callbacks
 * when multiple events are ready.
 */

static const struct {
    Ns_SockState when;           /* SOCK when bit. */
    short        event;          /* Poll event bit. */
} map[] = {
    {NS_SOCK_EXCEPTION, POLLPRI},
    {NS_SOCK_WRITE,     POLLOUT},
    {NS_SOCK_READ,      POLLIN}
};



/*
 *----------------------------------------------------------------------
 *
 * NsInitTask --
 *
 *      Global initialization for tasks.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_EXTERN void
NsInitTask(void)
{
    static bool initialized = NS_FALSE;

    if (!initialized) {
        Ns_MutexInit(&lock);
        Ns_MutexSetName(&lock, "ns:task");
        initialized = NS_TRUE;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_CreateTaskQueue --
 *
 *      Create a new task queue.
 *
 * Results:
 *      Handle to task queue.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_TaskQueue *
Ns_CreateTaskQueue(const char *name)
{
    TaskQueue *queuePtr;
    size_t     nameLength;

    NS_NONNULL_ASSERT(name != NULL);

    nameLength = strlen(name);
    queuePtr = ns_calloc(1u, sizeof(TaskQueue) + nameLength);
    memcpy(queuePtr->name, name, nameLength + 1u);
    Ns_MutexInit(&queuePtr->lock);
    Ns_MutexSetName2(&queuePtr->lock, "ns:taskqueue", name);

    if (ns_sockpair(queuePtr->trigger) != 0) {
        Ns_Fatal("taskqueue: ns_sockpair() failed: %s",
                 ns_sockstrerror(ns_sockerrno));
    }
    Ns_MutexLock(&lock);
    queuePtr->nextPtr = firstQueuePtr;
    firstQueuePtr = queuePtr;
    Ns_ThreadCreate(TaskThread, queuePtr, 0, &queuePtr->tid);
    Ns_MutexUnlock(&lock);

    return (Ns_TaskQueue *) queuePtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DestroyTaskQueue --
 *
 *      Stop and join a task queue.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Pending tasks callbacks, if any, are cancelled.
 *
 *----------------------------------------------------------------------
 */

void
Ns_DestroyTaskQueue(Ns_TaskQueue *queue)
{
    TaskQueue  *queuePtr;
    TaskQueue **nextPtrPtr;

    NS_NONNULL_ASSERT(queue != NULL);

    queuePtr = (TaskQueue *) queue;

    /*
     * Remove queue from list of all queues.
     */

    Ns_MutexLock(&lock);
    nextPtrPtr = &firstQueuePtr;
    while (*nextPtrPtr != queuePtr) {
        nextPtrPtr = &(*nextPtrPtr)->nextPtr;
    }
    *nextPtrPtr = queuePtr->nextPtr;
    Ns_MutexUnlock(&lock);

    /*
     * Signal stop and wait for join.
     */

    StopQueue(queuePtr);
    JoinQueue(queuePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCreate --
 *
 *      Create a new task.
 *
 * Results:
 *      Handle to task.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Task *
Ns_TaskCreate(NS_SOCKET sock, Ns_TaskProc *proc, void *arg)
{
    Task *taskPtr;

    NS_NONNULL_ASSERT(proc != NULL);

    taskPtr = ns_calloc(1u, sizeof(Task));
    taskPtr->sock = sock;
    taskPtr->proc = proc;
    taskPtr->arg = arg;

    return (Ns_Task *) taskPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskEnqueue --
 *
 *      Add a task to a queue.
 *
 * Results:
 *      NS_OK if task sent, NS_ERROR otherwise.
 *
 * Side effects:
 *      Queue will begin running the task.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TaskEnqueue(Ns_Task *task, Ns_TaskQueue *queue)
{
    Ns_ReturnCode status;
    Task         *taskPtr;

    NS_NONNULL_ASSERT(task != NULL);
    NS_NONNULL_ASSERT(queue != NULL);

    taskPtr = (Task *) task;

    taskPtr->queuePtr = (TaskQueue *) queue;
    if (unlikely(SignalQueue(taskPtr, TASK_INIT) == NS_FALSE)) {
        status = NS_ERROR;
    } else {
        status = NS_OK;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskRun --
 *
 *      Run a task directly, waiting for completion.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on task callback.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TaskRun(Ns_Task *task)
{
    Task          *taskPtr;
    const Ns_Time *timeoutPtr;
    Ns_Time        now;
    struct pollfd  pfd;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *) task;
    pfd.fd = taskPtr->sock;
    Call(taskPtr, NS_SOCK_INIT);
    Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %d: NS_SOCK_INIT done", taskPtr->sock);

    while ((taskPtr->flags & TASK_DONE) == 0u) {
        if ((taskPtr->flags & TASK_TIMEOUT) != 0u) {
            timeoutPtr = &taskPtr->timeout;
        } else {
            timeoutPtr = NULL;
        }
        pfd.revents = 0;
        pfd.events = taskPtr->events;
        if (NsPoll(&pfd, 1, timeoutPtr) != 1) {

            /*
             * A timeout occurred, notify the task
             */
            Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %d: timeout", taskPtr->sock);
            Call(taskPtr, NS_SOCK_TIMEOUT);
            status = NS_TIMEOUT; /* Prevent calling NS_SOCK_DONE below */
            break;
        }
        Ns_GetTime(&now);
        Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %d: run task with revents %.2x",
               taskPtr->sock, pfd.revents);

        RunTask(taskPtr, pfd.revents, &now);
    }

    if (status == NS_OK) {

        /*
         * If everything went well above, tell the task that we are done.
         */
        Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %d: NS_SOCK_DONE done", taskPtr->sock);
        Call(taskPtr, NS_SOCK_DONE);
        taskPtr->signalFlags |= TASK_DONE;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCancel --
 *
 *      Signal a task queue to stop running a task.
 *
 * Results:
 *      NS_OK if cancel sent, NS_ERROR otherwise.
 *
 * Side effects:
 *      Task callback will be invoke with NS_SOCK_CANCEL and is
 *      expected to call Ns_TaskDone to indicate completion.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TaskCancel(Ns_Task *task)
{
    Ns_ReturnCode status = NS_OK;
    Task         *taskPtr;

    NS_NONNULL_ASSERT(task != NULL);

    Ns_Log(Ns_LogTaskDebug, "task cancel");

    taskPtr = (Task *) task;
    if (taskPtr->queuePtr == NULL) {
        taskPtr->signalFlags |= TASK_CANCEL;
    } else if (SignalQueue(taskPtr, TASK_CANCEL) == NS_FALSE) {
        status = NS_ERROR;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskWait --
 *
 *      Wait for a task to complete.  Infinite wait is indicated
 *      by a NULL timeoutPtr.
 *
 * Results:
 *      NS_TIMEOUT if task did not complete by absolute time,
 *      NS_OK otherwise.
 *
 * Side effects:
 *      May wait up to specified timeout.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TaskWait(Ns_Task *task, Ns_Time *timeoutPtr)
{
    Task          *taskPtr;
    TaskQueue     *queuePtr;
    Ns_ReturnCode  status = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskWait %p timeout %p",
           (void*)task, (void*)timeoutPtr);

    taskPtr = (Task *) task;
    queuePtr = taskPtr->queuePtr;

    if (queuePtr == NULL) {
        if ((taskPtr->signalFlags & TASK_DONE) == 0u) {
            status = NS_TIMEOUT;
        }
    } else {
        if (timeoutPtr != NULL) {
            Ns_Time atime;

            timeoutPtr = Ns_AbsoluteTime(&atime, timeoutPtr);
        }

        Ns_MutexLock(&queuePtr->lock);
        while (status == NS_OK && (taskPtr->signalFlags & TASK_DONE) == 0u) {
            status = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock,
                                      timeoutPtr);
        }
        Ns_MutexUnlock(&queuePtr->lock);
        if (status == NS_OK) {
            taskPtr->queuePtr = NULL;
        }
    }

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskWait %p timeout %p returns %d",
           (void*)task, (void*)timeoutPtr, status);

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCompleted --
 *
 *      Checks if given task is completed
 *
 * Results:
 *      0 if task did not complete yet or timed out
 *      1 otherwise.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */

bool
Ns_TaskCompleted(const Ns_Task *task)
{
    const Task *taskPtr;
    TaskQueue  *queuePtr;
    bool        status;

    NS_NONNULL_ASSERT(task != NULL);
    taskPtr = (const Task *) task;

    queuePtr = taskPtr->queuePtr;
    if (queuePtr == NULL) {
        status = ((taskPtr->signalFlags & TASK_DONE) != 0u) ? NS_TRUE : NS_FALSE;
    } else {
        Ns_MutexLock(&queuePtr->lock);
        status = ((taskPtr->signalFlags & TASK_DONE) != 0u) ? NS_TRUE : NS_FALSE;
        Ns_MutexUnlock(&queuePtr->lock);
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCallback --
 *
 *  Update pending conditions and timeout for a task.  This
 *  routine  is expected to be called from within the task
 *  callback proc including to set the initial wait conditions
 *  from within the NS_SOCK_INIT callback.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Task callback will be invoked when ready or on timeout.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TaskCallback(Ns_Task *task, Ns_SockState when, const Ns_Time *timeoutPtr)
{
    Task *taskPtr;
    int   i;

    NS_NONNULL_ASSERT(task != NULL);

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskCallback task %p", (void*)task);
    taskPtr = (Task *) task;

    /*
     * Map from sock when bits to poll event bits.
     */

    taskPtr->events = 0;
    for (i = 0; i < Ns_NrElements(map); ++i) {
        if (when == map[i].when) {
            taskPtr->events |= map[i].event;
        }
    }

    /*
     * Copy timeout, if any.
     */

    if (timeoutPtr == NULL) {
        taskPtr->flags &= ~TASK_TIMEOUT;
    } else {
        taskPtr->flags |= TASK_TIMEOUT;
        taskPtr->timeout = *timeoutPtr;
    }

    /*
     * Mark as waiting if there are events or a timeout.
     */

    if ((taskPtr->events) != 0 || (timeoutPtr != NULL)) {
        taskPtr->flags |= TASK_WAIT;
    } else {
        taskPtr->flags &= ~TASK_WAIT;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskDone --
 *
 *      Mark a task as done. This routine should be called from
 *      within the task callback.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Task queue will signal this task is done on next spin.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TaskDone(Ns_Task *task)
{
    NS_NONNULL_ASSERT(task != NULL);

    ((Task *) task)->flags |= TASK_DONE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskFree --
 *
 *      Free task structure.  The caller is responsible for
 *      ensuring the task is no longer being run or monitored
 *      by a task queue.
 *
 * Results:
 *      The NS_SOCKET which the caller is responsible
 *      for closing or reusing.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_TaskFree(Ns_Task *task)
{
    NS_SOCKET sock;

    NS_NONNULL_ASSERT(task != NULL);

    sock = ((Task *) task)->sock;
    ns_free(task);

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartTaskQueueShutdown --
 *
 *      Trigger all task queues to begin shutdown.
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
NsStartTaskQueueShutdown(void)
{
    TaskQueue *queuePtr;

    /*
     * Trigger all queues to shutdown.
     */

    Ns_MutexLock(&lock);
    queuePtr = firstQueuePtr;
    while (queuePtr != NULL) {
        StopQueue(queuePtr);
        queuePtr = queuePtr->nextPtr;
    }
    Ns_MutexUnlock(&lock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsWaitTaskQueueShutdown --
 *
 *      Wait for all task queues to shutdown.
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
NsWaitTaskQueueShutdown(const Ns_Time *toPtr)
{
    TaskQueue     *queuePtr, *nextPtr;
    Ns_ReturnCode  status;

    /*
     * Clear out list of any remaining task queues.
     */

    Ns_MutexLock(&lock);
    queuePtr = firstQueuePtr;
    firstQueuePtr = NULL;
    Ns_MutexUnlock(&lock);

    /*
     * Join all queues possible within total allowed time.
     */

    status = NS_OK;
    while (status == NS_OK && queuePtr != NULL) {
        nextPtr = queuePtr->nextPtr;
        Ns_MutexLock(&queuePtr->lock);
        while (status == NS_OK && !queuePtr->stopped) {
            status = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, toPtr);
        }
        Ns_MutexUnlock(&queuePtr->lock);
        if (status == NS_OK) {
            JoinQueue(queuePtr);
        }
        queuePtr = nextPtr;
    }
    if (status != NS_OK) {
        Ns_Log(Warning, "timeout waiting for task queue shutdown");
    }
}


/*
 *----------------------------------------------------------------------
 *
 * RunTask --
 *
 *      Run a single task from either a task queue
 *      or a directly via Ns_TaskRun().
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on callbacks of given task.
 *
 *----------------------------------------------------------------------
 */

static void
RunTask(Task *taskPtr, short revents, const Ns_Time *nowPtr)
{
    NS_NONNULL_ASSERT(taskPtr != NULL);
    NS_NONNULL_ASSERT(nowPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "RunTask: revents 0: %d, task flags %.4x",
           revents, taskPtr->flags);

    /*
     * NB: Treat POLLHUP as POLLIN on systems which return it.
     */
    if ((revents & POLLHUP) != 0) {
        revents |= (short)POLLIN;
        Ns_Log(Ns_LogTaskDebug, "RunTask: got POLLHUP: new revents %d", revents);
    }
    if (revents != 0) {
        int i;

        for (i = 0; i < Ns_NrElements(map); ++i) {
            if ((revents & map[i].event) != 0) {
                Call(taskPtr, map[i].when);
            }
        }
    } else if (((taskPtr->flags & TASK_TIMEOUT) != 0u)
               && (Ns_DiffTime(&taskPtr->timeout, nowPtr, NULL) < 0)) {
        taskPtr->flags &= ~ TASK_WAIT;
        Ns_Log(Ns_LogTaskDebug, "RunTask: Call NS_SOCK_TIMEOUT for flags %.4x",
               taskPtr->flags);
        Call(taskPtr, NS_SOCK_TIMEOUT);
    }

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * SignalQueue --
 *
 *      Send a signal for a task to a task queue.
 *
 * Results:
 *      boolean value.
 *
 * Side effects:
 *      Task queue will process signal on next spin.
 *
 *----------------------------------------------------------------------
 */

static bool
SignalQueue(Task *taskPtr, unsigned int bit)
{
    TaskQueue *queuePtr;
    bool       pending = NS_FALSE, queueShutdown = NS_FALSE, result = NS_TRUE;

    NS_NONNULL_ASSERT(taskPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "signal queue");

    queuePtr = taskPtr->queuePtr;

    Ns_MutexLock(&queuePtr->lock);
    queueShutdown = queuePtr->shutdown;
    if (queueShutdown == NS_FALSE) {

        /*
         * Mark the signal and add event to signal list if not
         * already there.
         */

        taskPtr->signalFlags |= bit;
        pending = ((taskPtr->signalFlags & TASK_PENDING) != 0u);
        if (!pending) {
            taskPtr->signalFlags |= TASK_PENDING;
            taskPtr->nextSignalPtr = queuePtr->firstSignalPtr;
            queuePtr->firstSignalPtr = taskPtr;
        }
    }
    Ns_MutexUnlock(&queuePtr->lock);

    if (queueShutdown == NS_TRUE) {
        result = NS_FALSE;
    } else if (pending == NS_FALSE) {
        TriggerQueue(queuePtr);
        result = NS_TRUE;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * TriggerQueue --
 *
 *      Wakeup a task queue.
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
TriggerQueue(const TaskQueue *queuePtr)
{
    NS_NONNULL_ASSERT(queuePtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "trigger queue");

    if (ns_send(queuePtr->trigger[1], NS_EMPTY_STRING, 1, 0) != 1) {
        Ns_Fatal("task queue: trigger send() failed: %s",
                 ns_sockstrerror(ns_sockerrno));
    }
}


/*
 *----------------------------------------------------------------------
 *
 * StopQueue --
 *
 *      Signal a task queue to shutdown.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Queue will exit on next spin and call remaining tasks
 *      with NS_SOCK_EXIT.
 *
 *----------------------------------------------------------------------
 */

static void
StopQueue(TaskQueue *queuePtr)
{
    NS_NONNULL_ASSERT(queuePtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "stop queue");

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->shutdown = NS_TRUE;
    Ns_MutexUnlock(&queuePtr->lock);
    TriggerQueue(queuePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * JoinQueue --
 *
 *      Cleanup resources of a task queue.
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
JoinQueue(TaskQueue *queuePtr)
{
    NS_NONNULL_ASSERT(queuePtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "join queue");

    Ns_ThreadJoin(&queuePtr->tid, NULL);
    ns_sockclose(queuePtr->trigger[0]);
    ns_sockclose(queuePtr->trigger[1]);
    Ns_MutexDestroy(&queuePtr->lock);
    ns_free(queuePtr);
}


/*
 *----------------------------------------------------------------------
 *
 * TaskThread --
 *
 *      Run a task queue.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on callbacks of given tasks.
 *
 *----------------------------------------------------------------------
 */

static void
TaskThread(void *arg)
{
    TaskQueue     *queuePtr = arg;
    size_t         maxFds = 100u /* To start with */;
    Task          *taskPtr, *nextPtr, *firstWaitPtr;
    struct pollfd *pFds;

    Ns_ThreadSetName("task:%s", queuePtr->name);
    Ns_Log(Notice, "starting");

    pFds = (struct pollfd *)ns_malloc(sizeof(struct pollfd) * maxFds);
    firstWaitPtr = NULL;

    for (;;) {
        int               nFdsReady;
        NS_POLL_NFDS_TYPE nFds;
        bool              queueShutdown = NS_FALSE, broadcast = NS_FALSE;
        Ns_Time           now;
        const Ns_Time    *timeoutPtr;

        Ns_MutexLock(&queuePtr->lock);

        /*
         * Record queue shutting down, now that we hold
         * the queue mutex.
         */
        queueShutdown = queuePtr->shutdown;

        /*
         * Collect all signalled tasks in the waiting list
         */

        while ((taskPtr = queuePtr->firstSignalPtr) != NULL) {

            Ns_Log(Ns_LogTaskDebug, "TaskThread: signal handling task %p"
                   " signal flags %.4x flags %.4x",
                   (void*)taskPtr, taskPtr->signalFlags, taskPtr->flags);

            if ((taskPtr->flags & TASK_WAIT) == 0u) {
                taskPtr->flags |= TASK_WAIT;

                /*
                 * Only enqueue the taskPtr as nextWaitPtr, when this differs
                 * from the current task. Otherwise, we can get an infinite
                 * loop (can happen e.g. when timeouts fire).
                 */
                if (taskPtr != firstWaitPtr) {
                    taskPtr->nextWaitPtr = firstWaitPtr;
                    firstWaitPtr = taskPtr;
                }
            }
            if ((taskPtr->signalFlags & TASK_INIT) != 0u) {
                taskPtr->signalFlags &= ~TASK_INIT;
                taskPtr->flags       |= TASK_INIT;
            }
            if ((taskPtr->signalFlags & TASK_CANCEL) != 0u) {
                taskPtr->signalFlags &= ~TASK_CANCEL;
                taskPtr->flags       |= TASK_CANCEL;
            }
            taskPtr->signalFlags &= ~TASK_PENDING;

            queuePtr->firstSignalPtr = taskPtr->nextSignalPtr;
            taskPtr->nextSignalPtr = NULL;
        }

        Ns_MutexUnlock(&queuePtr->lock);

        /*
         * Include the trigger pipe in the list of descriptors
         * to poll on. This is used by TriggerPipe() to wake us
         * up and expedite work.
         */
        pFds[0].fd = queuePtr->trigger[0];
        pFds[0].events = (short)POLLIN;
        pFds[0].revents = 0;

        nFds = 1; /* Counts for the trigger pipe */
        broadcast = 0; /* Singnal any waiting threads about completed tasks */
        timeoutPtr = NULL; /* Will contain minimum time for NsPoll() */

        /*
         * Invoke pre-poll callbacks (TASK_INIT, TASK_CANCEL, TASK_DONE),
         * determine minimum poll timeout and set the pollfd structs
         * for all tasks located in the waiting list.
         *
         * Note that a task can go from TASK_INIT to TASK_DONE immediately
         * so all required callbacks are invoked before determining if a
         * poll is necessary.
         */

        taskPtr = firstWaitPtr;
        firstWaitPtr = NULL;

        while (taskPtr != NULL) {

            assert(taskPtr != taskPtr->nextWaitPtr);
            nextPtr = taskPtr->nextWaitPtr;

            Ns_Log(Ns_LogTaskDebug, "TaskThread: task %p next %p flags %.6x",
                   (void*)taskPtr, (void*)nextPtr, taskPtr->flags);

            if ((taskPtr->flags & TASK_INIT) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TaskThread: TASK_INIT task %p"
                       "  flags %.6x", (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~TASK_INIT;
                Call(taskPtr, NS_SOCK_INIT);

                Ns_Log(Ns_LogTaskDebug, "TaskThread: TASK_INIT task %p"
                       " flags %.6x DONE", (void*)taskPtr, taskPtr->flags);
            }

            if ((taskPtr->flags & TASK_CANCEL) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TaskThread: TASK_CANCEL task %p"
                       "  flags %.6x", (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~(TASK_CANCEL|TASK_WAIT);
                taskPtr->flags |= TASK_DONE;
                Call(taskPtr, NS_SOCK_CANCEL);

                Ns_Log(Ns_LogTaskDebug, "TaskThread: TASK_CANCEL task %p"
                       " flags %.6x DONE", (void*)taskPtr, taskPtr->flags);
            }

            if ((taskPtr->flags & TASK_DONE) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TaskThread: TASK_DONE task %p"
                       " flags %.6x", (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~(TASK_DONE|TASK_WAIT);
                Call(taskPtr, NS_SOCK_DONE);

                Ns_MutexLock(&queuePtr->lock);
                taskPtr->signalFlags |= TASK_DONE;
                Ns_MutexUnlock(&queuePtr->lock);
                broadcast = NS_TRUE;

                Ns_Log(Ns_LogTaskDebug, "TaskThread: TASK_DONE task %p"
                       " flags %.6x DONE", (void*)taskPtr, taskPtr->flags);
            }

            if ((taskPtr->flags & TASK_WAIT) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TaskThread: TASK_WAIT task %p"
                       " flags %.6x", (void*)taskPtr, taskPtr->flags);

                /*
                 * Arrange poll descriptor for this task
                 */
                if (maxFds <= (size_t)nFds) {
                    maxFds  = (size_t)nFds + 100u;
                    pFds = (struct pollfd *)ns_realloc(pFds, maxFds);
                }
                taskPtr->idx = nFds;
                pFds[nFds].fd = taskPtr->sock;
                pFds[nFds].events = taskPtr->events;
                pFds[nFds].revents = 0;

                /*
                 * Figure out minimum timeout for NsPoll()
                 */
                if ((taskPtr->flags & TASK_TIMEOUT) != 0u) {
                    if (timeoutPtr == NULL ||
                        Ns_DiffTime(&taskPtr->timeout, timeoutPtr, NULL) < 0) {
                        timeoutPtr = &taskPtr->timeout;
                    }
                }

                /*
                 * Push the task back to the waiting list again
                 */
                taskPtr->nextWaitPtr = firstWaitPtr;
                firstWaitPtr = taskPtr;
                nFds++;
            }

            taskPtr = nextPtr;
        }

        /*
         * Signal threads which may be waiting on tasks to complete,
         * as some of the task above may have been completed.
         */
        if (broadcast == NS_TRUE) {
            Ns_CondBroadcast(&queuePtr->cond);
        }

        /*
         * Check queue shutdown, now that all signals have been processed.
         */
        if (queueShutdown == NS_TRUE) {
            break;
        }

        /*
         * Poll on task sockets. This where we spend most of the time.
         * Result is just logged but otherwise ignored.
         *
         * FIXME: what happens on error (nFdsReady == -1?)
         */
        nFdsReady = NsPoll(pFds, nFds, timeoutPtr);

        Ns_Log(Ns_LogTaskDebug, "TaskThread: poll for %u fds returned %d",
               (unsigned)nFds, nFdsReady);

        /*
         * Drain the trigger pipe. This has no other meaning
         * but to kick us out of the NsPoll for attending
         * some expedited work.
         */
        if ((pFds[0].revents & POLLIN) != 0) {
            char emptyChar;

            Ns_Log(Ns_LogTaskDebug, "TaskThread: signal from trigger pipe");

            if (ns_recv(pFds[0].fd, &emptyChar, 1, 0) != 1) {
                Ns_Fatal("queue: trigger ns_read() failed: %s",
                         ns_sockstrerror(ns_sockerrno));
            }
        }

        /*
         * Execute events/timeouts for waiting tasks.
         */
        Ns_GetTime(&now);
        taskPtr = firstWaitPtr;
        while (taskPtr != NULL) {

            Ns_Log(Ns_LogTaskDebug, "runtask %p idx %u: revents %.2x",
                   (void*)taskPtr,
                   (unsigned)taskPtr->idx,
                   (int)pFds[taskPtr->idx].revents);

            RunTask(taskPtr, pFds[taskPtr->idx].revents, &now);
            taskPtr = taskPtr->nextWaitPtr;
        }
    }

    Ns_Log(Notice, "shutdown pending");

    /*
     * Call exit for all remaining tasks.
     */

    taskPtr = firstWaitPtr;
    while (taskPtr != NULL) {
        Call(taskPtr, NS_SOCK_EXIT);
        taskPtr = taskPtr->nextWaitPtr;
    }

    /*
     * Signal all tasks done and shutdown complete.
     */

    Ns_MutexLock(&queuePtr->lock);
    taskPtr = firstWaitPtr;
    while (taskPtr != NULL) {
        taskPtr->signalFlags |= TASK_DONE;
        taskPtr = taskPtr->nextWaitPtr;
    }
    queuePtr->stopped = NS_TRUE;
    Ns_MutexUnlock(&queuePtr->lock);
    Ns_CondBroadcast(&queuePtr->cond);

    ns_free(pFds);
    Ns_Log(Notice, "shutdown complete");
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
