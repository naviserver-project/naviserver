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

#define TASK_INIT     0x0001u
#define TASK_CANCEL   0x0002u
#define TASK_WAIT     0x0004u
#define TASK_TIMEOUT  0x0008u
#define TASK_DONE     0x0010u
#define TASK_PENDING  0x0020u
#define TASK_EXPIRE   0x0040u
#define TASK_TIMEDOUT 0x0080u
#define TASK_EXPIRED  0x0100u

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

static void TriggerQueue(TaskQueue *queuePtr)
    NS_GNUC_NONNULL(1);
static void JoinQueue(TaskQueue *queuePtr)
    NS_GNUC_NONNULL(1);
static void StopQueue(TaskQueue *queuePtr)
    NS_GNUC_NONNULL(1);
static bool SignalQueue(TaskQueue *queuePtr, Task *taskPtr, unsigned int signal)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static void FreeTask(Task *taskPtr)
    NS_GNUC_NONNULL(1);
static void RunTask(Task *taskPtr, short revents, const Ns_Time *nowPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);
static void ReleaseTask(Task *taskPtr)
    NS_GNUC_NONNULL(1);
static void ReserveTask(Task *taskPtr)
    NS_GNUC_NONNULL(1);

static Ns_ThreadProc TaskThread;

#define Call(tp, w) ((*((tp)->proc))((Ns_Task *)(tp), (tp)->sock, (tp)->arg, (w)))

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

void
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
    TaskQueue *queuePtr, **nextPtrPtr;

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

    ReserveTask(taskPtr);

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
        Ns_Time atime, *expirePtr;

        expirePtr = Ns_AbsoluteTime(&atime, expPtr);
        taskPtr->flags |= TASK_EXPIRE;
        taskPtr->expire = *expirePtr;
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

    ReleaseTask(taskPtr);

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
    TaskQueue    *queuePtr;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);
    NS_NONNULL_ASSERT(queue != NULL);

    taskPtr = (Task *)task;
    queuePtr = (TaskQueue *)queue;

    taskPtr->queuePtr = queuePtr;

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskEnqueue: task %p, queue:%p",
           (void*)taskPtr, (void*)queuePtr);
    if (unlikely(SignalQueue(queuePtr, taskPtr, TASK_INIT) != NS_TRUE)) {
        status = NS_ERROR;
    }
    Ns_Log(Ns_LogTaskDebug, "Ns_TaskEnqueue: task:%p status:%d",
           (void*)taskPtr, status);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskRun --
 *
 *      Run a task directly (in the same thread as the caller)
 *      until completion or expiry of the task timers.
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
    unsigned int  flags = 0u;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;

    taskPtr->flags &= ~(TASK_DONE);
    taskPtr->flags |= TASK_WAIT;

    pfd.fd = taskPtr->sock;

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun: task:%p init", (void*)taskPtr);
    Call(taskPtr, NS_SOCK_INIT);

    flags |= (TASK_TIMEDOUT|TASK_EXPIRED);

    while (status == NS_OK && (taskPtr->flags & (flags|TASK_DONE)) == 0u) {
        Ns_Time *timeoutPtr = NULL;

        if ((taskPtr->flags & TASK_TIMEOUT) != 0u) {
            timeoutPtr = &taskPtr->timeout;
        }
        if ((taskPtr->flags & TASK_EXPIRE) != 0u
            && (timeoutPtr == NULL
                || Ns_DiffTime(&taskPtr->expire, timeoutPtr, NULL) < 0)) {
            timeoutPtr = &taskPtr->expire;
        }
        pfd.events = taskPtr->events;
        if (NsPoll(&pfd, (NS_POLL_NFDS_TYPE)1, timeoutPtr) != 1) {
            Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun: task:%p timeout",
                   (void*)taskPtr);
            Call(taskPtr, NS_SOCK_TIMEOUT);

            status = NS_TIMEOUT;
        } else {
            Ns_Time now;
            Ns_GetTime(&now);
            RunTask(taskPtr, pfd.revents, &now);
        }
    }

    if (status == NS_OK && (taskPtr->flags & flags) == 0u) {
        Ns_Log(Ns_LogTaskDebug, "Ns_TaskRun: task:%p done",
               (void*)taskPtr);
        Call(taskPtr, NS_SOCK_DONE);
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
    TaskQueue    *queuePtr;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;
    queuePtr = taskPtr->queuePtr;

    NS_NONNULL_ASSERT(queuePtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskCancel: task:%p", (void*)taskPtr);
    if (unlikely(SignalQueue(queuePtr, taskPtr, TASK_CANCEL) != NS_TRUE)) {
        status = NS_ERROR;
    }
    Ns_Log(Ns_LogTaskDebug, "Ns_TaskCancel: task:%p status:%d",
           (void*)taskPtr, status);

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskWait --
 *
 *      Wait for a task to complete infinitely or for some time.
 *      Infinite wait is indicated by a NULL timeoutPtr.
 *      The passed timer (timeoutPtr) should contain time in
 *      the relative format and we will wait for the task to
 *      complete (or timeout) until the timer triggers.
 *
 * Results:
 *      NS_OK: task completed (done)
 *      NS_TIMEOUT: task expired, in comm timeout or timer expired
 *
 * Side effects:
 *      May exit with NS_TIMEOUT even if no explicit timer
 *      was passed (timeoutPtr is NULL).
 *
 *      For NS_OK returns, the task is dissociated from the
 *      task queue (if any used), otherwise the task is left
 *      associated with the queue.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_TaskWait(Ns_Task *task, Ns_Time *timeoutPtr)
{
    Task          *taskPtr;
    TaskQueue     *queuePtr;
    Ns_Time        atime, *toPtr = NULL;
    unsigned int   flags = 0u;
    Ns_ReturnCode  result = NS_OK;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;
    queuePtr = taskPtr->queuePtr;

    NS_NONNULL_ASSERT(queuePtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskWait %p", (void*)taskPtr);

    if (timeoutPtr != NULL) {
        toPtr = Ns_AbsoluteTime(&atime, timeoutPtr);
    }

    flags |= (TASK_TIMEDOUT|TASK_EXPIRED);

    Ns_MutexLock(&queuePtr->lock);
    while (result == NS_OK && (taskPtr->signalFlags & (flags|TASK_DONE)) == 0u) {
        result = Ns_CondTimedWait(&queuePtr->cond, &queuePtr->lock, toPtr);
    }
    if (result == NS_OK && (taskPtr->signalFlags & flags) != 0u) {
        result = NS_TIMEOUT;
    }
    taskPtr->signalFlags = 0;
    if (result == NS_OK) {
        taskPtr->queuePtr = NULL;
    }
    Ns_MutexUnlock(&queuePtr->lock);

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskWait %p status:%d", (void*)taskPtr, result);

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
    Task      *taskPtr;
    TaskQueue *queuePtr;
    bool       completed = NS_TRUE;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;
    queuePtr = taskPtr->queuePtr;

    if (queuePtr != NULL) {
        Ns_MutexLock(&queuePtr->lock);
        completed = ((taskPtr->signalFlags & TASK_DONE) != 0u);
        Ns_MutexUnlock(&queuePtr->lock);
    }

    return completed;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TaskWaitCompleted --
 *
 *      Wait until the task is completed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
void Ns_TaskWaitCompleted(Ns_Task *task)
{
    Task      *taskPtr;
    TaskQueue *queuePtr;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;
    queuePtr = taskPtr->queuePtr;

    NS_NONNULL_ASSERT(queuePtr != NULL);

    Ns_MutexLock(&queuePtr->lock);
    while ((taskPtr->signalFlags & TASK_DONE) == 0u) {
        Ns_CondWait(&queuePtr->cond, &queuePtr->lock);
    }
    Ns_MutexUnlock(&queuePtr->lock);

    return;
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
    Task         *taskPtr;
    unsigned int  index, flags = 0u;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *) task;

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskCallback: task:%p  when:%.2x, timeout:%p",
           (void*)taskPtr, (int)when, (void*)timeoutPtr);

    /*
     * Map Ns_SockState bits to poll bits.
     */
    taskPtr->events = 0;
    for (index = 0u; index < Ns_NrElements(map); index++) {
        if (when == map[index].when) {
            taskPtr->events |= map[index].event;
        }
    }

    /*
     * Copy timeout. We may get either wall-clock or relative time
     * but we require the wall-clock time because of the internals
     * of the task handling.
     */
    if (timeoutPtr == NULL) {
        taskPtr->flags &= ~(TASK_TIMEOUT);
    } else {
        Ns_Time atime, *timePtr;

        timePtr = Ns_AbsoluteTime(&atime, (Ns_Time *)timeoutPtr);
        taskPtr->timeout = *timePtr;
        taskPtr->flags |= TASK_TIMEOUT;
    }

    /*
     * Mark as waiting if there are events or timers.
     */
    flags |= (TASK_TIMEOUT|TASK_EXPIRE);
    if (taskPtr->events != 0 || (taskPtr->flags & flags) != 0u) {
        taskPtr->flags |= TASK_WAIT;
    } else {
        taskPtr->flags &= ~(TASK_WAIT);
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
    Task *taskPtr;

    NS_NONNULL_ASSERT(task != NULL);

    taskPtr = (Task *)task;

    Ns_Log(Ns_LogTaskDebug, "Ns_TaskDone: task:%p", (void *)taskPtr);
    taskPtr->flags |= TASK_DONE;
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
    TaskQueue     *queuePtr, *nextPtr = NULL;
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
 *      Will run task socket timeout callback for task timeout/expiry.
 *
 *----------------------------------------------------------------------
 */

static void
RunTask(Task *taskPtr, short revents, const Ns_Time *nowPtr)
{
    NS_NONNULL_ASSERT(taskPtr != NULL);

    Ns_Log(Ns_LogTaskDebug, "RunTask: task:%p, flags:%.6x, revents:%.2x",
           (void*)taskPtr, (int)taskPtr->flags, (int)revents);

    if ((taskPtr->flags & TASK_EXPIRE) != 0u
        && Ns_DiffTime(&taskPtr->expire, nowPtr, NULL) <= 0) {

        taskPtr->flags |= TASK_EXPIRED;
        Ns_Log(Ns_LogTaskDebug, "RunTask: task:%p expired, flags:%.6x",
               (void*)taskPtr, taskPtr->flags);
        Call(taskPtr, NS_SOCK_TIMEOUT);

    } else if (revents != 0) {
        unsigned int index;

        /*
         * NB: Treat POLLHUP as POLLIN on systems which return it.
         */
        if ((revents & POLLHUP) != 0) {
            revents |= (short)POLLIN;
        }
        for (index = 0u; index < Ns_NrElements(map); index++) {
            if ((revents & map[index].event) != 0) {
                Ns_Log(Ns_LogTaskDebug, "RunTask: task:%p events, event:%.2x",
                       (void*)taskPtr, map[index].when);
                Call(taskPtr, map[index].when);
            }
        }
    } else if ((taskPtr->flags & TASK_TIMEOUT) != 0u
               && Ns_DiffTime(&taskPtr->timeout, nowPtr, NULL) <= 0) {

        taskPtr->flags |= TASK_TIMEDOUT;
        Ns_Log(Ns_LogTaskDebug, "RunTask: task:%p timedout, flags:%.6x",
               (void*)taskPtr, taskPtr->flags);
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
SignalQueue(TaskQueue *queuePtr, Task *taskPtr, unsigned int signal)
{
    bool queueShutdown, taskDone, pending = NS_FALSE, result = NS_TRUE;

    Ns_Log(Ns_LogTaskDebug, "SignalQueue: name:%s: signal:%d",
           queuePtr->name, signal);

    Ns_MutexLock(&queuePtr->lock);
    queueShutdown = queuePtr->shutdown;

    /*
     * Task which is already marked as completed
     * should not be touched any more.
     * An example is cancelling an already completed task.
     */
    taskDone = ((taskPtr->signalFlags & TASK_DONE) != 0u);

    if (queueShutdown == NS_FALSE && taskDone == NS_FALSE) {

        /*
         * Mark the signal and add event to the signal list
         * if task not already listed there.
         */
        taskPtr->signalFlags |= signal;
        pending = ((taskPtr->signalFlags & TASK_PENDING) != 0u);

        if (pending == NS_FALSE) {
            taskPtr->signalFlags |= TASK_PENDING;
            taskPtr->nextSignalPtr = queuePtr->firstSignalPtr;
            queuePtr->firstSignalPtr = taskPtr;
            ReserveTask(taskPtr); /* Acquired for the signal list */
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

    Ns_Log(Ns_LogTaskDebug, "SignalQueue: name:%s: signal:%d, result:%d",
           queuePtr->name, signal, result);

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

    Ns_Log(Ns_LogTaskDebug, "TriggerQueue: name:%s", queuePtr->name);

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
    Ns_Log(Ns_LogTaskDebug, "StopQueue: name:%s", queuePtr->name);

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
    Ns_Log(Ns_LogTaskDebug, "JoinQueue: name:%s", queuePtr->name);

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
 * FreeTask, ReleaseTask --
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
    Ns_Log(Ns_LogTaskDebug, "FreeTask: task:%p", (void *)taskPtr);
    ns_free(taskPtr);

    return;
}

static void
ReleaseTask(Task *taskPtr)
{
    Ns_Log(Ns_LogTaskDebug, "ReleaseTask taskPtr %p refCount %d",
           (void*)taskPtr, taskPtr->refCount);

    if (--taskPtr->refCount == 0) {
        FreeTask(taskPtr);
    }

    return;
}

static void
ReserveTask(Task *taskPtr)
{
    taskPtr->refCount++;

    Ns_Log(Ns_LogTaskDebug, "ReserveTask taskPtr %p refCount %d",
           (void*)taskPtr, taskPtr->refCount);

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
    Task          *taskPtr, *nextPtr, *firstWaitPtr = NULL;
    struct pollfd *pFds;
    size_t         maxFds = 100u; /* Initial count of pollfd's */

    Ns_ThreadSetName("task:%s", queuePtr->name);
    Ns_Log(Notice, "starting");

    pFds = (struct pollfd *)ns_calloc(maxFds, sizeof(struct pollfd));

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
         * Handle all signaled tasks from the waiting list
         */

        while ((taskPtr = queuePtr->firstSignalPtr) != NULL) {

            Ns_Log(Ns_LogTaskDebug, "signal-list handling for task:%p"
                   " signalflags:%.6x flags:%.6x",
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
                    ReserveTask(taskPtr); /* Acquired for the waiting list */
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
            ReleaseTask(taskPtr); /* Released from the signal list */
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
            unsigned int signalFlags = 0u;

            assert(taskPtr != taskPtr->nextWaitPtr);
            nextPtr = taskPtr->nextWaitPtr;

            Ns_Log(Ns_LogTaskDebug, "wait-list handling for task:%p"
                   " next:%p flags:%.6x",
                   (void*)taskPtr, (void*)nextPtr, taskPtr->flags);

            if ((taskPtr->flags & TASK_INIT) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TASK_INIT task:%p flags:%.6x",
                       (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~(TASK_INIT);
                Call(taskPtr, NS_SOCK_INIT);

                Ns_Log(Ns_LogTaskDebug, "TASK_INIT task:%p flags:%.6x DONE",
                       (void*)taskPtr, taskPtr->flags);
            }
            if ((taskPtr->flags & TASK_CANCEL) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TASK_CANCEL task:%p flags:%.6x",
                       (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~(TASK_CANCEL|TASK_WAIT);
                taskPtr->flags |= TASK_DONE;
                Call(taskPtr, NS_SOCK_CANCEL);

                Ns_Log(Ns_LogTaskDebug, "TASK_CANCEL task:%p flags:%.6x DONE",
                       (void*)taskPtr, taskPtr->flags);

            }
            if ((taskPtr->flags & TASK_EXPIRED) != 0u) {

                taskPtr->flags &= ~(TASK_EXPIRED|TASK_WAIT);
                signalFlags |= TASK_EXPIRED;
                broadcast = NS_TRUE;

                Ns_Log(Ns_LogTaskDebug, "TASK_EXPIRED task:%p flags:%.6x",
                       (void*)taskPtr, taskPtr->flags);
            }
            if ((taskPtr->flags & TASK_TIMEDOUT) != 0u) {

                taskPtr->flags &= ~(TASK_TIMEDOUT|TASK_WAIT);
                signalFlags |= TASK_TIMEDOUT;
                broadcast = NS_TRUE;

                Ns_Log(Ns_LogTaskDebug, "TASK_TIMEDOUT task:%p flags:%.6x",
                       (void*)taskPtr, taskPtr->flags);
            }
            if ((taskPtr->flags & TASK_DONE) != 0u) {

                Ns_Log(Ns_LogTaskDebug, "TASK_DONE task:%p flags:%.6x",
                       (void*)taskPtr, taskPtr->flags);

                taskPtr->flags &= ~(TASK_DONE|TASK_WAIT);
                signalFlags |= TASK_DONE;
                broadcast = NS_TRUE;
                Call(taskPtr, NS_SOCK_DONE);

                Ns_Log(Ns_LogTaskDebug, "TASK_DONE task:%p flags:%.6x DONE",
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

                if ((taskPtr->flags & TASK_EXPIRE) != 0u) {
                    if (timeoutPtr == NULL
                        || Ns_DiffTime(&taskPtr->expire, timeoutPtr, NULL) < 0) {

                        timeoutPtr = &taskPtr->expire;
                    }
                }

                /*
                 * Push the task back to the waiting list again
                 */
                taskPtr->nextWaitPtr = firstWaitPtr;
                firstWaitPtr = taskPtr;
                ReserveTask(taskPtr); /* Acquired for the waiting list */

                Ns_Log(Ns_LogTaskDebug, "TASK_WAIT task:%p flags:%.6x",
                       (void*)taskPtr, taskPtr->flags);
            }

            /*
             * At this place, we might wake-up the thread
             * waiting in Ns_TaskWait() if the cond-var
             * receives a spurious wakeup.
             * Note we are blocking the thread here.
             * Perhaps there is a better way to pass
             * this signal back to the task?
             */
            if (signalFlags == 0u) {
                ReleaseTask(taskPtr); /* Released from the waiting list */
            } else {
                Ns_MutexLock(&queuePtr->lock);
                taskPtr->signalFlags |= signalFlags;
                ReleaseTask(taskPtr); /* Released from the waiting list */
                Ns_MutexUnlock(&queuePtr->lock);
            }

            taskPtr = nextPtr; /* Advance to the next task in the wait list */
        }

        /*
         * Signal threads which may be waiting on tasks to complete,
         * as some of the task above may have been completed already.
         * It is important to note that the condvar may have received
         * spurious wake-ups so some waiter threads may be already
         * handling the task even before we signal them explicitly.
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
            Ns_Log(Ns_LogTaskDebug, "poll for %u fds returned %d ready",
                   (unsigned)nFds, nready);
        }

        /*
         * Drain the trigger pipe. This has no other reason
         * but to kick us out of the NsPoll() for attending
         * some expedited work.
         */
        if ((pFds[0].revents & POLLIN) != 0) {
            char emptyChar;

            Ns_Log(Ns_LogTaskDebug, "received signal from trigger-pipe");

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
            nextPtr = taskPtr->nextWaitPtr;
            RunTask(taskPtr, pFds[taskPtr->idx].revents, &now);
            taskPtr = nextPtr;
        }
    }

    Ns_Log(Notice, "shutdown pending");

    /*
     * Call exit for all waiting tasks.
     */
    taskPtr = firstWaitPtr;
    while (taskPtr != NULL) {
        nextPtr = taskPtr->nextWaitPtr;
        Call(taskPtr, NS_SOCK_EXIT);
        taskPtr = nextPtr;
    }

    /*
     * Release all tasks and complete shutdown.
     */
    Ns_MutexLock(&queuePtr->lock);
    taskPtr = firstWaitPtr;
    while (taskPtr != NULL) {
        taskPtr->signalFlags |= TASK_DONE;
        nextPtr = taskPtr->nextWaitPtr;
        ReleaseTask(taskPtr); /* This might free the task */
        taskPtr = nextPtr;
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
