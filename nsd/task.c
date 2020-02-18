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
 *      Handles socket I/O tasks.
 */

#include "nsd.h"

/*
 * The following defines a task queue.
 */

typedef struct TaskQueue {
    struct TaskQueue  *nextPtr;           /* Next in list of all queues */
    struct Task       *firstSignalPtr;    /* First in list of task signals */
    Ns_Thread          tid;               /* Service thread ID */
    Ns_Mutex           lock;              /* Queue list and signal lock */
    Ns_Cond            cond;              /* Task and queue signal condition */
    bool               shutdown;          /* Shutdown flag */
    bool               stopped;           /* Stop flag */
    NS_SOCKET          trigger[2];        /* Trigger pipes */
    char               name[1];           /* Name of the queue */
} TaskQueue;

/*
 * The following bits are used to send signals to tasks
 * and manage the task state.
 */

#define TASK_INIT      0x01u
#define TASK_CANCEL    0x02u
#define TASK_WAIT      0x04u
#define TASK_TIMEOUT   0x08u
#define TASK_DONE      0x10u
#define TASK_PENDING   0x20u
#define TASK_EXPIRE    0x40u

/*
 * The following defines a task.
 */

typedef struct Task {
    struct TaskQueue  *queuePtr;      /* Monitoring queue */
    struct Task       *nextWaitPtr;   /* Next on wait queue */
    struct Task       *nextSignalPtr; /* Next on signal queue */
    NS_SOCKET          sock;          /* Task socket for I/O */
    Ns_TaskProc       *proc;          /* Task callback */
    void              *arg;           /* Callback private data */
    NS_POLL_NFDS_TYPE  idx;           /* Poll index */
    short              events;        /* Poll events */
    Ns_Time            timeout;       /* Read/write timeout (wall-clock time) */
    Ns_Time            expire;        /* Task wall-clock time) */
    int                refCount;      /* For reserve/release purposes */
    unsigned int       signalFlags;   /* Signal flags sent to queue thread */
    unsigned int       flags;         /* Flags private to the task */
} Task;

/*
 * Local functions defined in this file
 */

static void TriggerQueue(TaskQueue *)         NS_GNUC_NONNULL(1);
static void JoinQueue(TaskQueue *)            NS_GNUC_NONNULL(1);
static void StopQueue(TaskQueue *)            NS_GNUC_NONNULL(1);
static bool SignalQueue(Task *, unsigned)     NS_GNUC_NONNULL(1);
static void FreeTask(Task *)                  NS_GNUC_NONNULL(1);
static void RunTask(Task *, short, Ns_Time *) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

static Ns_ThreadProc TaskThread;

#define Call(tp,w) ((*((tp)->proc))((Ns_Task *)(tp),(tp)->sock,(tp)->arg,(w)))
#define Reserve(tp) (tp)->refCount++
#define Release(tp) if (--(tp)->refCount <= 0) FreeTask(tp)

/*
 * Static variables defined in this file
 */

static TaskQueue *firstQueuePtr; /* List of all known task queues */
static Ns_Mutex   lock;          /* Lock for the queue list */

/*
 * The following maps Ns_SockState bits to poll event bits.
 * The order is significant and determines the order of callbacks
 * when multiple events are ready.
 */

static const struct {
    Ns_SockState when;
    short        event;
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
 *      Global initialization for tasks subsystem.
 *      Must be called with the global lock held.
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
 *      Create a new (named) task queue.
 *
 * Results:
 *      Handle to the task queue.
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

    queuePtr->stopped = NS_FALSE;

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

    StopQueue(queuePtr);
    JoinQueue(queuePtr);

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCreate --
 *
 *      Create new task.
 *
 * Results:
 *      Handle to the task.
 *
 * Side effects:
 *      Reserves the task initially.
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

    Reserve(taskPtr);

    return (Ns_Task *) taskPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskTimedCreate --
 *
 *      Create a new timed task.
 *      A timed task may live up to the given expiration time.
 *      After expiry time is reached, it is set as timed-out.
 *
 * Results:
 *      Handle to the task.
 *
 * Side effects:
 *      If no expiration time given, defaults to Ns_TaskCreate.
 *
 *----------------------------------------------------------------------
 */

Ns_Task *
Ns_TaskTimedCreate(NS_SOCKET sock, Ns_TaskProc *proc, void *arg, Ns_Time *expPtr)
{
    Task *taskPtr;

    taskPtr = (Task *)Ns_TaskCreate(sock, proc, arg);

    if (expPtr != NULL) {
        Ns_Time atime, *expire = NULL;

        expire = Ns_AbsoluteTime(&atime, expPtr);
        taskPtr->flags |= TASK_EXPIRE;
        taskPtr->expire = *expire;
    }

    return (Ns_Task *) taskPtr;
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
 *      Task will be released and might be eventually freed.
 *
 *----------------------------------------------------------------------
 */

NS_SOCKET
Ns_TaskFree(Ns_Task *task)
{
    Task      *taskPtr;
    NS_SOCKET  sock;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;
    sock = taskPtr->sock;
    Release(taskPtr);

    return sock;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskEnqueue --
 *
 *      Add a task to a queue.
 *
 * Results:
 *      NS_OK if task added, NS_ERROR otherwise.
 *
 * Side effects:
 *      Queue will begin running the task.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TaskEnqueue(Ns_Task *task, Ns_TaskQueue *queue)
{
    Task         *taskPtr;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);
    NS_NONNULL_ASSERT(queue != NULL);

    taskPtr = (Task *)task;
    taskPtr->queuePtr = (TaskQueue *) queue;

    if (unlikely(SignalQueue(taskPtr, TASK_INIT) == NS_FALSE)) {
        status = NS_ERROR;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskRun --
 *
 *      Run a task directly (in the same thread as the caller)
 *      until completion or expiry of the task timeout.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on the task callback.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TaskRun(Ns_Task *task)
{
    Task          *taskPtr;
    struct pollfd pfd;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;
    taskPtr->signalFlags &= ~TASK_DONE;
    taskPtr->flags &= ~TASK_DONE;
    taskPtr->flags |= TASK_WAIT;
    pfd.fd = taskPtr->sock;

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %p NS_SOCK_INIT", (void*)task);
    Call(taskPtr, NS_SOCK_INIT);

    while (status == NS_OK && (taskPtr->flags & TASK_DONE) == 0u) {
        if ((taskPtr->flags & TASK_WAIT) == 0u) {
            status = NS_TIMEOUT;
        } else {
            Ns_Time *timeoutPtr = NULL;

            if ((taskPtr->flags & TASK_TIMEOUT) != 0u) {
                timeoutPtr = &taskPtr->timeout;
            }
            if (timeoutPtr != NULL && (taskPtr->flags & TASK_EXPIRE) != 0u) {
                if (Ns_DiffTime(&taskPtr->expire, timeoutPtr, NULL) < 0u) {
                    timeoutPtr = &taskPtr->expire;
                }
            }
            pfd.events = taskPtr->events;
            if (NsPoll(&pfd, (NS_POLL_NFDS_TYPE)1, timeoutPtr) != 1) {
                status = NS_TIMEOUT;
            } else {
                Ns_Time now;
                Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %p run with revents:%.2x",
                       (void*)task, (int)pfd.revents);
                Ns_GetTime(&now);
                RunTask(taskPtr, pfd.revents, &now);
            }
        }
    }

    if (status == NS_TIMEOUT) {
        Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %p NS_SOCK_TIMEOUT", (void*)task);
        Call(taskPtr, NS_SOCK_TIMEOUT);
        taskPtr->signalFlags |= TASK_TIMEOUT;
    } else {
        Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun %p NS_SOCK_DONE", (void*)task);
        Call(taskPtr, NS_SOCK_DONE);
        taskPtr->signalFlags |= TASK_DONE;
    }

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCancel --
 *
 *      Signal a task queue to stop running a task.
 *
 * Results:
 *      NS_OK if cancel signal sent
 *      NS_ERROR otherwise.
 *
 * Side effects:
 *      Task callback will be invoked with NS_SOCK_CANCEL
 *      and is expected to call Ns_TaskDone to indicate completion.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TaskCancel(Ns_Task *task)
{
    Task         *taskPtr;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(tas != NULL);

    taskPtr = (Task *)task;
    Ns_Log(Ns_LogTaskDebug, "Ns_TaskCancel %p", (void*)task);

    if (taskPtr->queuePtr == NULL) {
        taskPtr->signalFlags |= TASK_CANCEL;
    } else if (unlikely(SignalQueue(taskPtr, TASK_CANCEL) == NS_FALSE)) {
        status = NS_ERROR;
    }

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskCancel %p status:%d", (void*)task, status);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskWait --
 *
 *      Wait for a task to complete.
 *      Infinite wait is indicated by a NULL timeoutPtr.
 *
 * Results:
 *      NS_TIMEOUT if task expired or in comm timeout
 *      NS_OK otherwise.
 *
 * Side effects:
 *      May exit with timeout if the task had expiry-time set
 *      even if no explicit timeout passed (timeoutPtr is NULL).
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TaskWait(Ns_Task *task, Ns_Time *timeoutPtr)
{
    Task          *taskPtr;
    TaskQueue     *queuePtr;
    Ns_ReturnCode  result = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *) task;
    Ns_Log(Ns_LogTaskDebug, "Ns_TaskWait %p", (void*)task);

    queuePtr = taskPtr->queuePtr;
    if (queuePtr == NULL) {
        if ((taskPtr->signalFlags & TASK_DONE) == 0u) {
            result = NS_TIMEOUT;
        }
    } else {
        Ns_Time atime, *toPtr = NULL;

        if (timeoutPtr != NULL) {
            toPtr = Ns_AbsoluteTime(&atime, timeoutPtr);
        }
        Ns_MutexLock(&queuePtr->lock);
        while (result == NS_OK
               && (taskPtr->signalFlags & (TASK_DONE|TASK_TIMEOUT)) == 0u) {

            result = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, toPtr);
        }
        if ((taskPtr->signalFlags & TASK_TIMEOUT) != 0u) {
            result = NS_TIMEOUT;
        }
        Ns_MutexUnlock(&queuePtr->lock);
        if (result == NS_OK) {
            taskPtr->queuePtr = NULL;
        }
    }

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskWait %p status:%d", (void*)task, result);

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCompleted --
 *
 *      Checks if given task has completed
 *
 * Results:
 *      NS_TRUE if task completed
 *      NS_FALSE otherwise
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
    int         done;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *) task;
    if (taskPtr->queuePtr == NULL) {
        done = taskPtr->signalFlags & TASK_DONE;
    } else {
        Ns_MutexLock(&taskPtr->queuePtr->lock);
        done = taskPtr->signalFlags & TASK_DONE;
        Ns_MutexUnlock(&taskPtr->queuePtr->lock);
    }

    return (done == 0u) ? NS_FALSE : NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskCallback --
 *
 *      Update pending conditions and timeout of a task.
 *      Should be called from the task callback proc.
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
Ns_TaskCallback(Ns_Task *task, Ns_SockState when, const Ns_Time *timeoutPtr)
{
    Task *taskPtr;
    int   i;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *) task;
    Ns_Log(Ns_LogTaskDebug, "Ns_TaskCallback task %p  when:%.2x",
           (void*)task, (int)when);

    /*
     * Map Ns_SockState bits to poll bits.
     */
    taskPtr->events = 0;
    for (i = 0; i < Ns_NrElements(map); i++) {
        if (when == map[i].when) {
            taskPtr->events |= map[i].event;
        }
    }

    /*
     * Copy timeout. We may get either wall-clock or relative time
     * but we require the wall-clock time because of the internals
     * of the task handling.
     */
    if (timeoutPtr == NULL) {
        taskPtr->flags &= ~TASK_TIMEOUT;
    } else {
        Ns_Time atime, *timePtr = NULL;

        timePtr = Ns_AbsoluteTime(&atime, (Ns_Time *)timeoutPtr);
        taskPtr->timeout = *timePtr;
        taskPtr->flags |= TASK_TIMEOUT;
    }

    /*
     * Honour task expiry time since it may trigger
     * before the calculated timeout above.
     */
    if ((taskPtr->flags & TASK_EXPIRE) != 0u) {
        if ((taskPtr->flags & TASK_TIMEOUT) == 0u
            || Ns_DiffTime(&taskPtr->expire, &taskPtr->timeout, NULL) < 0u) {

            taskPtr->timeout = taskPtr->expire;
            taskPtr->flags |= TASK_TIMEOUT;
        }
    }

    /*
     * Mark as waiting if there are events or a timeout.
     */
    if (taskPtr->events != 0 || (taskPtr->flags & TASK_TIMEOUT) != 0u) {
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
 *      Task queue will signal this task on the next spin.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TaskDone(Ns_Task *task)
{
    NS_NONNULL_ASSERT(task != NULL);

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskDone %p", (void *)task);
    ((Task *)task)->flags |= TASK_DONE;
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

    Ns_MutexLock(&lock);
    queuePtr = firstQueuePtr;
    while (queuePtr != NULL) {
        StopQueue(queuePtr);
        queuePtr = queuePtr->nextPtr;
    }
    Ns_MutexUnlock(&lock);

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * NsWaitTaskQueueShutdown --
 *
 *      Wait for all task queues to shutdown
 *      within the given time interval.
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
    TaskQueue     *queuePtr = NULL, *nextPtr = NULL;
    Ns_ReturnCode  status = NS_OK;

    /*
     * Clear out list of known task queues.
     */
    Ns_MutexLock(&lock);
    queuePtr = firstQueuePtr;
    firstQueuePtr = NULL;
    Ns_MutexUnlock(&lock);

    /*
     * Join all queues, possibly within allowed time.
     */
    while (status == NS_OK && queuePtr != NULL) {
        nextPtr = queuePtr->nextPtr;
        Ns_MutexLock(&queuePtr->lock);
        while (status == NS_OK && queuePtr->stopped == NS_FALSE) {
            status = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, toPtr);
        }
        Ns_MutexUnlock(&queuePtr->lock);
        if (queuePtr->stopped == NS_TRUE) {
            JoinQueue(queuePtr);
            queuePtr = nextPtr;
        }
    }

    if (status != NS_OK) {
        Ns_Log(Warning, "timeout waiting for task queues shutdown");
    }

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * RunTask --
 *
 *      Run a single task from either a task queue
 *      or a directly via the Ns_TaskRun procedure.
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
RunTask(Task *taskPtr, short revents, Ns_Time *nowPtr)
{
    bool timeout = NS_FALSE;

    NS_NONNULL_ASSERT(taskPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "RunTask %p, flags:%.4x, revents:%.2x",
           (void*)taskPtr, (int)taskPtr->flags, (int)revents);

    if ((taskPtr->flags & TASK_EXPIRE) != 0u
        && Ns_DiffTime(&taskPtr->expire, nowPtr, NULL) <= 0) {

        taskPtr->flags &= ~TASK_WAIT;
        Ns_Log(Ns_LogTaskDebug, "RunTask %p expired, call NS_SOCK_TIMEOUT,"
               " flags:%.4x", (void*)taskPtr, (int)taskPtr->flags);
        Call(taskPtr, NS_SOCK_TIMEOUT);
        timeout = NS_TRUE;

    } else if (revents != 0) {
        int i;

        /*
         * NB: Treat POLLHUP as POLLIN on systems which return it.
         */
        if ((revents & POLLHUP) != 0) {
            revents |= (short)POLLIN;
            Ns_Log(Ns_LogTaskDebug, "RunTask %p, got POLLHUP: revents:%.2x",
                   (void*)taskPtr, (int)revents);
        }
        for (i = 0; i < Ns_NrElements(map); i++) {
            if ((revents & map[i].event) != 0) {
                Call(taskPtr, map[i].when);
            }
        }
    } else if ((taskPtr->flags & TASK_TIMEOUT) != 0
               && Ns_DiffTime(&taskPtr->timeout, nowPtr, NULL) <= 0) {

        taskPtr->flags &= ~TASK_WAIT;
        Ns_Log(Ns_LogTaskDebug, "RunTask %p timeout, call NS_SOCK_TIMEOUT,"
               " flags:%.4x", (void*)taskPtr, (int)taskPtr->flags);
        Call(taskPtr, NS_SOCK_TIMEOUT);
        timeout = NS_TRUE;
    }

    if (timeout == NS_TRUE) {
        Ns_MutexLock(&taskPtr->queuePtr->lock);
        taskPtr->signalFlags |= TASK_TIMEOUT;
        Ns_CondSignal(&taskPtr->queuePtr->cond);
        Ns_MutexUnlock(&taskPtr->queuePtr->lock);
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
    bool       queueShutdown, taskDone, pending = NS_FALSE, result = NS_TRUE;

    NS_NONNULL_ASSERT(taskPtr != NULL);

    queuePtr = taskPtr->queuePtr;

    Ns_Log(Ns_LogTaskDebug, "SignalQueue %s: bit:%d", queuePtr->name, bit);

    Ns_MutexLock(&queuePtr->lock);
    queueShutdown = queuePtr->shutdown;

    /*
     * If cancelling a task, make sure to clear timeout signal
     * as otherwise a following call to Ns_TaskWait() might
     * exit prematurely.
     */
    if (bit == TASK_CANCEL && (taskPtr->signalFlags & TASK_TIMEOUT) != 0u) {
        taskPtr->signalFlags &= ~TASK_TIMEOUT;
    }

    /*
     * Task which is already marked as completed
     * should not be touched any more.
     * An example is cancelling an already completed task.
     */
    taskDone = ((taskPtr->signalFlags & TASK_DONE) != 0u);

    if (queueShutdown == NS_FALSE && taskDone == NS_FALSE) {

        /*
         * Mark the signal and add event to signal list if not
         * already there.
         */
        taskPtr->signalFlags |=bit;
        pending = ((taskPtr->signalFlags & TASK_PENDING) != 0u);
        if (pending == NS_FALSE) {
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
    } else if (taskDone == NS_TRUE) {
        result = NS_FALSE;
    }

    Ns_Log(Ns_LogTaskDebug, "SignalQueue %s: bit:%d, result:%d",
           queuePtr->name, bit, result);

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
TriggerQueue(TaskQueue *queuePtr)
{
    NS_NONNULL_ASSERT(queuePtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "TriggerQueue %s", queuePtr->name);

    if (ns_send(queuePtr->trigger[1], NS_EMPTY_STRING, 1, 0) != 1) {
        Ns_Fatal("TriggerQueue ns_send() failed: %s",
                 ns_sockstrerror(ns_sockerrno));
    }

    return;
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

    Ns_Log(Ns_LogTaskDebug, "StopQueue %s", queuePtr->name);

    Ns_MutexLock(&queuePtr->lock);
    queuePtr->shutdown = NS_TRUE;
    Ns_MutexUnlock(&queuePtr->lock);

    TriggerQueue(queuePtr);

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * JoinQueue --
 *
 *      Cleanup resources of a task queue and free it.
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

    Ns_Log(Ns_LogTaskDebug, "JoinQueue %s", queuePtr->name);

    Ns_ThreadJoin(&queuePtr->tid, NULL);
    ns_sockclose(queuePtr->trigger[0]);
    ns_sockclose(queuePtr->trigger[1]);
    Ns_MutexDestroy(&queuePtr->lock);

    ns_free(queuePtr);

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeTask --
 *
 *      Free's task memory.
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
FreeTask(Task *taskPtr)
{
    NS_NONNULL_ASSERT(taskPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "FreeTask %p", (void *)taskPtr);
    ns_free(taskPtr);

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * TaskThread --
 *
 *      Service the task queue.
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
    TaskQueue     *queuePtr = (TaskQueue *)arg;
    Task          *taskPtr, *nextPtr, *firstWaitPtr;
    struct pollfd *pFds = NULL;
    size_t         maxFds = 100u; /* Initial count of pollfd's */

    Ns_ThreadSetName("task:%s", queuePtr->name);
    Ns_Log(Notice, "starting");

    pFds = (struct pollfd *)ns_calloc(maxFds, sizeof(struct pollfd));
    firstWaitPtr = NULL;

    for (;;) {
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
         * Collect all signaled tasks in the signal waiting list
         */

        while ((taskPtr = queuePtr->firstSignalPtr) != NULL) {

            Ns_Log(Ns_LogTaskDebug, "signal handling for task %p"
                   " signal flags %.4x flags %.4x",
                   (void*)taskPtr, taskPtr->signalFlags, taskPtr->flags);

            taskPtr->signalFlags &= ~TASK_PENDING;

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
                    Reserve(taskPtr); /* Task acquired for the waiting list */
                }
            }

            if ((taskPtr->signalFlags & TASK_INIT) != 0u) {
                taskPtr->signalFlags &= ~TASK_INIT;
                taskPtr->flags |= TASK_INIT;
            }

            if ((taskPtr->signalFlags & TASK_CANCEL) != 0u) {
                taskPtr->signalFlags &= ~TASK_CANCEL;
                taskPtr->flags |= TASK_CANCEL;
            }

            queuePtr->firstSignalPtr = taskPtr->nextSignalPtr;
            taskPtr->nextSignalPtr = NULL;
        }

        Ns_MutexUnlock(&queuePtr->lock);

        /*
         * Include the trigger pipe in the list of descriptors
         * to poll on. This pipe wakes us up and expedites work.
         */
        pFds[0].fd = queuePtr->trigger[0];
        pFds[0].events = (short)POLLIN;
        pFds[0].revents = 0;

        nFds = 1; /* Count of the pollable sockets (+ the trigger pipe) */
        broadcast = 0; /* Signal any waiting threads about completed tasks */
        timeoutPtr = NULL; /* Minimum time for NsPoll() */

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

            Ns_Log(Ns_LogTaskDebug, "waiting task %p next %p flags %.6x",
                   (void*)taskPtr, (void*)nextPtr, taskPtr->flags);

            if ((taskPtr->flags & TASK_INIT) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TASK_INIT task %p flags %.6x",
                       (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~TASK_INIT;
                Call(taskPtr, NS_SOCK_INIT);

                Ns_Log(Ns_LogTaskDebug, "TASK_INIT task %p flags %.6x DONE",
                       (void*)taskPtr, taskPtr->flags);
            }

            if ((taskPtr->flags & TASK_CANCEL) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TASK_CANCEL task %p flags %.6x",
                       (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~(TASK_CANCEL|TASK_WAIT);
                taskPtr->flags |= TASK_DONE;
                Call(taskPtr, NS_SOCK_CANCEL);

                Ns_Log(Ns_LogTaskDebug, "TASK_CANCEL task %p flags %.6x DONE",
                       (void*)taskPtr, taskPtr->flags);
            }

            if ((taskPtr->flags & TASK_DONE) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TASK_DONE task %p flags %.6x",
                       (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~(TASK_DONE|TASK_WAIT);
                Call(taskPtr, NS_SOCK_DONE);

                Ns_MutexLock(&queuePtr->lock);
                taskPtr->signalFlags |= TASK_DONE;
                Ns_MutexUnlock(&queuePtr->lock);

                broadcast = NS_TRUE;

                Ns_Log(Ns_LogTaskDebug, "TASK_DONE task %p flags %.6x DONE",
                       (void*)taskPtr, taskPtr->flags);
            }

            if ((taskPtr->flags & TASK_WAIT) != 0u) {

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

                nFds++;

                /*
                 * Figure out minimum timeout to wait for socket events
                 */
                if ((taskPtr->flags & TASK_TIMEOUT) != 0u) {
                    if (timeoutPtr == NULL
                        || Ns_DiffTime(&taskPtr->timeout, timeoutPtr, NULL) < 0) {

                        timeoutPtr = &taskPtr->timeout;
                    }
                }

                /*
                 * Push the task back to the waiting list again
                 */
                taskPtr->nextWaitPtr = firstWaitPtr;
                firstWaitPtr = taskPtr;
                Reserve(taskPtr); /* Task acquired for the waiting list */

                Ns_Log(Ns_LogTaskDebug, "TASK_WAIT task %p flags %.6x",
                       (void*)taskPtr, (int)taskPtr->flags);
            }

            Release(taskPtr); /* Task released from the waiting list */
            taskPtr = nextPtr;
        }

        /*
         * Signal threads which may be waiting on tasks to complete,
         * as some of the task above may have been completed already.
         */
        if (broadcast == NS_TRUE) {
            Ns_CondBroadcast(&queuePtr->cond);
        }

        /*
         * Check queue shutdown, now that all tasks have been processed.
         */
        if (queueShutdown == NS_TRUE) {
            break;
        }

        /*
         * Poll on task sockets. This where we spend most of the time.
         * Result is just logged but otherwise ignored.
         * Note that NsPoll() never returns negative. In case of some
         * error, it brings the whole house down.
         */

        {
            int nready;

            nready = NsPoll(pFds, nFds, timeoutPtr);
            Ns_Log(Ns_LogTaskDebug, "poll for %u fds returned %d",
                   (unsigned)nFds, nready);
        }

        /*
         * Drain the trigger pipe. This has no other reason
         * but to kick us out of the NsPoll() for attending
         * some expedited work.
         */
        if ((pFds[0].revents & POLLIN) != 0) {
            char emptyChar;

            Ns_Log(Ns_LogTaskDebug, "signal from trigger pipe");

            if (ns_recv(pFds[0].fd, &emptyChar, 1, 0) != 1) {
                Ns_Fatal("queue: signal from trigger pipe failed: %s",
                         ns_sockstrerror(ns_sockerrno));
            }
        }

        /*
         * Execute socket events for waiting tasks.
         */
        Ns_GetTime(&now);
        taskPtr = firstWaitPtr;
        while (taskPtr != NULL) {
            short revents = pFds[taskPtr->idx].revents;

            Ns_Log(Ns_LogTaskDebug, "run task:%p idx:%u: revents:%.2x",
                   (void*)taskPtr, (unsigned)taskPtr->idx, (int)revents);
            RunTask(taskPtr, revents, &now);
            taskPtr = taskPtr->nextWaitPtr;
        }
    }

    Ns_Log(Notice, "shutdown pending");

    /*
     * Call exit for all waiting tasks.
     */
    taskPtr = firstWaitPtr;
    while (taskPtr != NULL) {
        (void) Call(taskPtr, NS_SOCK_EXIT);
        taskPtr = taskPtr->nextWaitPtr;
    }

    /*
     * Release all tasks and complete shutdown.
     */
    Ns_MutexLock(&queuePtr->lock);
    taskPtr = firstWaitPtr;
    while (taskPtr != NULL) {
        Task *nextWaitPtr = taskPtr->nextWaitPtr;

        taskPtr->signalFlags |= TASK_DONE;
        Release(taskPtr); /* This might free the task */
        taskPtr = nextWaitPtr;
    }
    queuePtr->stopped = NS_TRUE;
    Ns_CondBroadcast(&queuePtr->cond);
    Ns_MutexUnlock(&queuePtr->lock);

    ns_free(pFds);

    Ns_Log(Notice, "shutdown complete");

    return;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
