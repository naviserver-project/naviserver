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
 * tcljob.c --
 *
 *	Tcl job queueing routines.
 *
 * Lock rules:
 *
 *   o. lock the queuelock when modifing tp structure elements.
 *   o. lock the queue's lock when modifing queue structure elements.
 *   o  jobs are shared between tp and the queue but are owned by the queue,
 *      so use queue's lock is used to control access to the jobs.
 *   o. to avoid deadlock, when locking both the queuelock and queue's
 *      lock lock the queuelock first
 *   o. to avoid deadlock, the tp queuelock should be locked before the
 *      queue's lock.
 *
 *
 * Notes:
 *
 *   The threadpool's max number of thread is the sum of all the current
 *   queue's max threads.
 *
 *   The number of threads in the thread pool can be greater than
 *   the current max number of threads. This situtation can occur when
 *   a queue is deleted. Later on if a new queue is created it will simply
 *   use one of the previously created threads. Basically the number of
 *   threads is a "high water mark".
 *
 *   The queues are reference counted. Only when a queue is empty and
 *   its reference count is zero can it be deleted.
 *
 *   We can no longer use a Tcl_Obj to represent the queue because queues can
 *   now be deleted. Tcl_Objs are deleted when the object goes out of
 *   scope, whereas queues are deleted when delete is called. By doing
 *   this the queue can be used across tcl interpreters.
 *
 * ToDo:
 *
 *   Users can leak queues. A queue will stay around until a user
 *   cleans it up. It order to help the user out we would like to
 *   add an "-autoclean" option to queue create function. However,
 *   AOLServer does not currently supply a "good" connection cleanup
 *   callback. We tryed to use "Ns_RegisterConnCleanup" however it does
 *   not have a facility to remove registered callbacks.
 *
 */

#include "nsd.h"

/*
 * If a user does not specify the a max number of threads for a queue,
 * then the following default is used.
 */

#define NS_JOB_DEFAULT_MAXTHREADS 4

typedef enum JobStates {
    JOB_SCHEDULED = 0,
    JOB_RUNNING,
    JOB_DONE,
    JobStatesMax
} JobStates;

typedef enum JobTypes {
    JOB_NON_DETACHED = 0,
    JOB_DETACHED,
    JobTypesMax
} JobTypes;

typedef enum JobRequests {
    JOB_NONE = 0,
    JOB_WAIT,
    JobRequestsMax
} JobRequests;

typedef enum QueueRequests {
    QUEUE_REQ_NONE = 0,
    QUEUE_REQ_DELETE,
    QueueRequestsMax
} QueueRequests;

typedef enum ThreadPoolRequests {
    THREADPOOL_REQ_NONE = 0,
    THREADPOOL_REQ_STOP,
    ThreadPoolRequestsMax
} ThreadPoolRequests;


/*
 * Jobs are enqueued on queues.
 */

typedef struct Job {
    struct Job       *nextPtr;
    const char       *server;
    JobStates         state;
    int               code;
    int               cancel;
    JobTypes          type;
    JobRequests       req;
    char             *errorCode;
    char             *errorInfo;
    const char       *queueId;
    uintptr_t         tid;
    Tcl_AsyncHandler  async;
    Tcl_DString       id;
    Tcl_DString       script;
    Tcl_DString       results;
    Ns_Time           startTime;
    Ns_Time           endTime;
} Job;

/*
 * A queue manages a set of jobs.
 */

typedef struct Queue {
    const char        *name;
    const char        *desc;
    Ns_Mutex           lock;
    Ns_Cond            cond;
    uintptr_t          nextid;
    QueueRequests      req;
    int                maxThreads;
    int                nRunning;
    Tcl_HashTable      jobs;
    int                refCount;
} Queue;


/*
 * A threadpool mananges a global set of threads.
 */
typedef struct ThreadPool {
    Ns_Cond            cond;
    Ns_Mutex           queuelock;
    Tcl_HashTable      queues;
    ThreadPoolRequests req;
    int                nextThreadId;
    unsigned long      nextQueueId;
    int                maxThreads;
    int                nthreads;
    int                nidle;
    Job               *firstPtr;
    int                jobsPerThread;
    Ns_Time            timeout;
} ThreadPool;


/*
 * Function prototypes/forward declarations.
 */
static Ns_ObjvProc ObjvQueue;

static Tcl_ObjCmdProc  JobCancelObjCmd;
static Tcl_ObjCmdProc  JobConfigureObjCmd;
static Tcl_ObjCmdProc  JobCreateObjCmd;
static Tcl_ObjCmdProc  JobDeleteObjCmd;
static Tcl_ObjCmdProc  JobExistsObjCmd;
static Tcl_ObjCmdProc  JobGenIDObjCmd;
static Tcl_ObjCmdProc  JobJobListObjCmd;
static Tcl_ObjCmdProc  JobJobsObjCmd;
static Tcl_ObjCmdProc  JobQueueListObjCmd;
static Tcl_ObjCmdProc  JobQueueObjCmd;
static Tcl_ObjCmdProc  JobQueuesObjCmd;
static Tcl_ObjCmdProc  JobThreadListObjCmd;
static Tcl_ObjCmdProc  JobWaitAnyObjCmd;
static Tcl_ObjCmdProc  JobWaitObjCmd;

static void   JobThread(void *arg);
static Job*   GetNextJob(void);

static Queue* NewQueue(const char* queueName, const char* queueDesc, int maxThreads)
    NS_GNUC_NONNULL(1)  NS_GNUC_NONNULL(2)
    NS_GNUC_RETURNS_NONNULL;

static void   FreeQueue(Queue *queue)
    NS_GNUC_NONNULL(1);

static Job*   NewJob(const char* server, const char *queueName,
                     JobTypes type, const char *script)
    NS_GNUC_NONNULL(2)  NS_GNUC_NONNULL(4)
    NS_GNUC_RETURNS_NONNULL;

static void   FreeJob(Job *jobPtr)
    NS_GNUC_NONNULL(1);

static int    JobAbort(ClientData clientData, Tcl_Interp *interp, int code);

static int    LookupQueue(Tcl_Interp *interp, const char *queueName,
                          Queue **queuePtr, bool locked)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static bool   ReleaseQueue(Queue *queue, bool locked)
    NS_GNUC_NONNULL(1);

static bool   AnyDone(Queue *queue)
    NS_GNUC_NONNULL(1);

static void   SetupJobDefaults(void);

static const char* GetJobCodeStr(int code);
static const char* GetJobStateStr(JobStates state);
static const char* GetJobTypeStr(JobTypes type);
static const char* GetJobReqStr(JobRequests req);
static const char* GetQueueReqStr(QueueRequests req);
static const char* GetTpReqStr(ThreadPoolRequests req);

static int AppendField(Tcl_Interp *interp, Tcl_Obj *list,
                       const char *name, const char *value)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);


static int AppendFieldInt(Tcl_Interp *interp, Tcl_Obj *list,
                          const char *name, int value)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int AppendFieldLong(Tcl_Interp *interp, Tcl_Obj *list,
                           const char *name, long value)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static int AppendFieldDouble(Tcl_Interp *interp, Tcl_Obj *list,
                             const char *name, double value)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static double ComputeDelta(const Ns_Time *start, const Ns_Time *end)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);




/*
 * Globals
 */

static ThreadPool tp;


/*
 *----------------------------------------------------------------------
 *
 * NsInitTclQueueType --
 *
 *	    Initialize the Tcl job queue.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

void
NsTclInitQueueType(void)
{
    Tcl_InitHashTable(&tp.queues, TCL_STRING_KEYS);
    Ns_MutexSetName(&tp.queuelock, "jobThreadPool");
    tp.nextThreadId = 0;
    tp.nextQueueId = 0u;
    tp.maxThreads = 0;
    tp.nthreads = 0;
    tp.nidle = 0;
    tp.firstPtr = NULL;
    tp.req = THREADPOOL_REQ_NONE;
    tp.jobsPerThread = 0;
    tp.timeout.sec = 0;
    tp.timeout.usec = 0;
}


/*
 *----------------------------------------------------------------------
 *
 * NsStartJobsShutdown --
 *
 *	    Signal stop of the Tcl job threads.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    All pending jobs are removed and waiting threads interrupted.
 *
 *----------------------------------------------------------------------
 */

void
NsStartJobsShutdown(void)
{
    Tcl_HashSearch       search;
    const Tcl_HashEntry *hPtr;

    hPtr = Tcl_FirstHashEntry(&tp.queues, &search);
    while (hPtr != NULL) {
        Ns_MutexLock(&tp.queuelock);
    	tp.req = THREADPOOL_REQ_STOP;
    	Ns_CondBroadcast(&tp.cond);
    	Ns_MutexUnlock(&tp.queuelock);
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsWaitJobsShutdown --
 *
 *	    Wait for Tcl job threads to exit.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

void
NsWaitJobsShutdown(const Ns_Time *toPtr)
{
    Tcl_HashSearch       search;
    const Tcl_HashEntry *hPtr;
    Ns_ReturnCode        status = NS_OK;

    hPtr = Tcl_FirstHashEntry(&tp.queues, &search);
    while (status == NS_OK && hPtr != NULL) {
        Ns_MutexLock(&tp.queuelock);
    	while (status == NS_OK && tp.nthreads > 0) {
            status = Ns_CondTimedWait(&tp.cond, &tp.queuelock, toPtr);
    	}
    	Ns_MutexUnlock(&tp.queuelock);
        hPtr = Tcl_NextHashEntry(&search);
    }
    if (status != NS_OK) {
        Ns_Log(Warning, "tcljobs: timeout waiting for exit");
    }
}


/*
 *----------------------------------------------------------------------
 *
 * JobConfigureObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job configure" command.
 *          Configure jobs subsystem.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobConfigureObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int            result = TCL_OK;
    int            jpt = -1;
    const Ns_Time *timeoutPtr = NULL;
    Ns_ObjvSpec    lopts[] = {
        {"-jobsperthread",  Ns_ObjvInt,  &jpt,        NULL},
        {"-timeout",        Ns_ObjvTime, &timeoutPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        char  buf[100];

        Ns_MutexLock(&tp.queuelock);
        SetupJobDefaults();

        if (jpt >= 0) {
            tp.jobsPerThread = jpt;
        }
        if (timeoutPtr != NULL) {
            tp.timeout = *timeoutPtr;
        }
        snprintf(buf, sizeof(buf), "jobsperthread %d timeout %ld:%06ld",
                 tp.jobsPerThread, tp.timeout.sec, tp.timeout.usec);
        Ns_MutexUnlock(&tp.queuelock);
        Tcl_AppendResult(interp, buf, NULL);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobCreateObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job create" command.
 *          Create a new thread pool queue.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobCreateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{

    int          result = TCL_OK, max = NS_JOB_DEFAULT_MAXTHREADS;
    Tcl_Obj     *queueIdObj;
    const char  *descString  = "";
    Ns_ObjvSpec  lopts[] = {
        {"-desc",   Ns_ObjvString,   &descString,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"queueId",     Ns_ObjvObj,  &queueIdObj,  NULL},
        {"?maxThreads", Ns_ObjvInt,  &max,         NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const char     *queueIdString = Tcl_GetString(queueIdObj);
        Tcl_HashEntry  *hPtr;
        int             isNew;

        Ns_MutexLock(&tp.queuelock);
        hPtr = Tcl_CreateHashEntry(&tp.queues, queueIdString, &isNew);
        if (isNew != 0) {
            Queue *queue = NewQueue(Tcl_GetHashKey(&tp.queues, hPtr), descString, max);

            Tcl_SetHashValue(hPtr, queue);
        }
        Ns_MutexUnlock(&tp.queuelock);

        if (isNew == 0) {
            Ns_TclPrintfResult(interp, "queue already exists: %s", queueIdString);
            result = TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, queueIdObj);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JobDeleteObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job delete" command.  Request that the
 *          specified queue be deleted. The queue will only be deleted
 *          when all jobs are removed.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobDeleteObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Queue  *queue = NULL;
    int     result = TCL_OK;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "queueId");
        result = TCL_ERROR;

    } else if (LookupQueue(interp, Tcl_GetString(objv[2]),
                           &queue, NS_FALSE) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        assert(queue != NULL);

        queue->req = QUEUE_REQ_DELETE;
        (void)ReleaseQueue(queue, NS_FALSE);
        Ns_CondBroadcast(&tp.cond);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobQueueObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job queue" command.
 *          Add a new job the specified queue.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobQueueObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK, create = 0, head = 0, detached = 0;
    const char *script = NULL, *jobIdString = NULL, *queueIdString = NULL;
    Ns_ObjvSpec lopts[] = {
        {"-head",      Ns_ObjvBool,    &head,        INT2PTR(NS_TRUE)},
        {"-detached",  Ns_ObjvBool,    &detached,    INT2PTR(NS_TRUE)},
        {"-jobid",     Ns_ObjvString,  &jobIdString, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"queueId",  Ns_ObjvString,  &queueIdString,  NULL},
        {"script",   Ns_ObjvString,  &script,   NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const NsInterp *itPtr = clientData;
        Queue          *queue = NULL;
        Job            *jobPtr = NULL;
        JobTypes        jobType = JOB_NON_DETACHED;
        Tcl_HashEntry  *hPtr;
        int             isNew;
        char            buf[100];

        if (detached != 0) {
            jobType = JOB_DETACHED;
        }

        Ns_MutexLock(&tp.queuelock);
        if (LookupQueue(interp, queueIdString, &queue, NS_TRUE) != TCL_OK) {
            result = TCL_ERROR;
            goto releaseQueue;
        }

        assert(queue != NULL);

        /*
         * Create a new job and add to the Thread Pool's list of jobs.
         */

        jobPtr = NewJob((itPtr->servPtr != NULL) ? itPtr->servPtr->server : NULL,
                        queue->name, jobType, script);
        Ns_GetTime(&jobPtr->startTime);
        if (tp.req == THREADPOOL_REQ_STOP
            || queue->req == QUEUE_REQ_DELETE) {
            Tcl_AppendResult(interp,
                             "The specified queue is being deleted or "
                             "the system is stopping.", NULL);
            FreeJob(jobPtr);
            result = TCL_ERROR;
            goto releaseQueue;
        }

        /*
         * Job id is given, try to see if it is taken already,
         * if yes, return error, it should be unique.
         */

        if (jobIdString != NULL && *jobIdString != '\0') {
            hPtr = Tcl_CreateHashEntry(&queue->jobs, jobIdString, &isNew);
            if (isNew == 0) {
                FreeJob(jobPtr);
                Tcl_AppendResult(interp, "Job ", jobIdString,
                                 " already exists", NULL);
                result = TCL_ERROR;
                goto releaseQueue;
            }
        } else {
            /*
             * Add the job to queue.
             */
            do {
                snprintf(buf, sizeof(buf), "job%" PRIuPTR, queue->nextid++);
                hPtr = Tcl_CreateHashEntry(&queue->jobs, buf, &isNew);
            } while (isNew == 0);

            jobIdString = buf;
        }

        /*
         * Add the job to the thread pool's job list, if -head is
         * specified, insert new job at the beginning, otherwise
         * append new job to the end.
         */

        if (head != 0) {
            jobPtr->nextPtr = tp.firstPtr;
            tp.firstPtr = jobPtr;
        } else {
            Job  **nextPtrPtr = &tp.firstPtr;

            while (*nextPtrPtr != NULL) {
                nextPtrPtr = &((*nextPtrPtr)->nextPtr);
            }
            *nextPtrPtr = jobPtr;
        }

        /*
         * Start a new thread if there are less than maxThreads
         * currently running and there currently no idle threads.
         */

        if (tp.nidle == 0 && tp.nthreads < tp.maxThreads) {
            create = 1;
            ++tp.nthreads;
        } else {
            create = 0;
        }

        Tcl_DStringAppend(&jobPtr->id, jobIdString, -1);
        Tcl_SetHashValue(hPtr, jobPtr);
        Ns_CondBroadcast(&tp.cond);

    releaseQueue:
        if (queue != NULL) {
            (void)ReleaseQueue(queue, NS_TRUE);
        }
        Ns_MutexUnlock(&tp.queuelock);
        if (create != 0) {
            Ns_ThreadCreate(JobThread, NULL, 0, NULL);
        }
        if (result == TCL_OK) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(jobIdString, -1));
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobWaitObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job wait" command.
 *          Wait for the specified job.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobWaitObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int            result = TCL_OK;
    const Ns_Time *deltaTimeoutPtr = NULL;
    const char    *jobIdString;
    Queue         *queue;
    Ns_ObjvSpec    lopts[] = {
        {"-timeout",  Ns_ObjvTime,   &deltaTimeoutPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"queueId",  ObjvQueue,     &queue,       NULL},
        {"jobId",    Ns_ObjvString, &jobIdString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result =  TCL_ERROR;
    } else {
        Ns_Time        timeout = {0,0};
        Job           *jobPtr = NULL;
        Tcl_HashEntry *hPtr;

        if (deltaTimeoutPtr != NULL) {
            /*
             * Set the timeout time. This is an absolute time.
             */
            Ns_GetTime(&timeout);
            Ns_IncrTime(&timeout, deltaTimeoutPtr->sec, deltaTimeoutPtr->usec);
        }

        hPtr = Tcl_FindHashEntry(&queue->jobs, jobIdString);
        if (hPtr == NULL) {
            Ns_TclPrintfResult(interp, "no such job: %s", jobIdString);
            result = TCL_ERROR;
            goto releaseQueue;
        }

        jobPtr = Tcl_GetHashValue(hPtr);

        if (jobPtr->type == JOB_DETACHED) {
            Ns_TclPrintfResult(interp, "can't wait on detached job: %s", jobIdString);
            result = TCL_ERROR;
            goto releaseQueue;
        }

        if (jobPtr->req == JOB_WAIT) {
            Ns_TclPrintfResult(interp, "can't wait on waited job: %s", jobIdString);
            result = TCL_ERROR;
            goto releaseQueue;
        }

        jobPtr->req = JOB_WAIT;

        if (deltaTimeoutPtr != NULL) {
            while (jobPtr->state != JOB_DONE) {
                Ns_ReturnCode timedOut = Ns_CondTimedWait(&queue->cond,
                                                          &queue->lock, &timeout);
                if (timedOut == NS_TIMEOUT) {
                    Tcl_SetResult(interp, "Wait timed out.", TCL_STATIC);
                    Tcl_SetErrorCode(interp, "NS_TIMEOUT", NULL);
                    jobPtr->req = JOB_NONE;
                    result = TCL_ERROR;
                    goto releaseQueue;
                }
            }
        } else {
            while (jobPtr->state != JOB_DONE) {
                Ns_CondWait(&queue->cond, &queue->lock);
            }
        }

        /*
         * At this point the job we were waiting on has completed,
         * so we return the job's results and errorcodes, then
         * clean up the job.
         *
         * The following is a sanity check that ensures no other
         * process removed this job's entry.
         */
        hPtr = Tcl_FindHashEntry(&queue->jobs, jobIdString);
        if (hPtr == NULL || jobPtr == Tcl_GetHashValue(hPtr)) {
            Tcl_SetResult(interp, "Internal ns_job error.", TCL_STATIC);
            /*
             * logically, there should be a "result = TCL_ERROR;"
             * here. however, this would change the results of the regression test
             */
        }
        if (hPtr != NULL) {
            Tcl_DeleteHashEntry(hPtr);
        }

        if (result == TCL_OK) {
            Tcl_DStringResult(interp, &jobPtr->results);
            result = jobPtr->code;
            if (result == TCL_ERROR) {
                if (jobPtr->errorCode != NULL) {
                    Tcl_SetErrorCode(interp, jobPtr->errorCode, NULL);
                }
                if (jobPtr->errorInfo != NULL) {
                    Tcl_AddObjErrorInfo(interp, "\n", 1);
                    Tcl_AddObjErrorInfo(interp, jobPtr->errorInfo, -1);
                }
            }
        }
        FreeJob(jobPtr);

    releaseQueue:
        (void)ReleaseQueue(queue, NS_FALSE);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobCancelObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job cancel" command.
 *          Cancel the specified job.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobCancelObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Queue        *queue;
    int          result = TCL_OK;
    const char  *jobIdString;
    Ns_ObjvSpec  args[] = {
        {"queueId",  ObjvQueue,      &queue,       NULL},
        {"jobId",    Ns_ObjvString,  &jobIdString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result =  TCL_ERROR;

    } else {
        Job                  *jobPtr = NULL;
        const Tcl_HashEntry  *hPtr = Tcl_FindHashEntry(&queue->jobs, jobIdString);

        if (hPtr == NULL) {
            (void)ReleaseQueue(queue, NS_FALSE);
            Ns_TclPrintfResult(interp, "no such job: %s", jobIdString);
            result = TCL_ERROR;

        } else {
            jobPtr = Tcl_GetHashValue(hPtr);
            if (unlikely(jobPtr->req == JOB_WAIT)) {
                (void)ReleaseQueue(queue, NS_FALSE);
                Tcl_AppendResult(interp,"can't cancel job \"",
                                 Tcl_DStringValue(&jobPtr->id),
                                 "\", someone is waiting on it", NULL);
                result = TCL_ERROR;
            }
        }
        if (result == TCL_OK) {
            assert(jobPtr != NULL);
            jobPtr->cancel = 1;
            if (jobPtr->async != NULL) {
                Tcl_AsyncMark(jobPtr->async);
            }
            Ns_CondBroadcast(&queue->cond);
            Ns_CondBroadcast(&tp.cond);
            Tcl_SetObjResult(interp,
                             Tcl_NewBooleanObj(jobPtr->state == JOB_RUNNING));
            (void)ReleaseQueue(queue, NS_FALSE);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobExistsObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job exists" command.  Sets the
 *          Tcl-result to 1if job is running otherwise to 0.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobExistsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Queue       *queue = NULL;
    int          result = TCL_OK;
    const char  *jobIdString;
    Ns_ObjvSpec  args[] = {
        {"queueId",  ObjvQueue,     &queue,       NULL},
        {"jobId",    Ns_ObjvString, &jobIdString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Tcl_HashEntry  *hPtr = Tcl_FindHashEntry(&queue->jobs, jobIdString);

        (void)ReleaseQueue(queue, NS_FALSE);
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(hPtr != NULL));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobWaitAnyObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job waitany" command.
 *          Wait for any job on the queue complete.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobWaitAnyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Queue         *queue;
    int            result = TCL_OK;
    const Ns_Time *deltaTimeoutPtr = NULL;
    Ns_ObjvSpec    lopts[] = {
        {"-timeout",  Ns_ObjvTime, &deltaTimeoutPtr, NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec    args[] = {
        {"queueId",  ObjvQueue,    &queue,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(lopts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_Time         timeout = {0,0};
        Tcl_HashSearch  search;

        if (deltaTimeoutPtr != NULL) {
            /*
             * Set the timeout time. This is an absolute time.
             */
            Ns_GetTime(&timeout);
            Ns_IncrTime(&timeout, deltaTimeoutPtr->sec, deltaTimeoutPtr->usec);
        }

        /*
         * While there are jobs in queue or no jobs are "done", wait
         * on the queue condition variable.
         */

        if (deltaTimeoutPtr != NULL) {
            while ((Tcl_FirstHashEntry(&queue->jobs, &search) != NULL)
                   && (result == TCL_OK)
                   && !AnyDone(queue)) {
                Ns_ReturnCode timedOut = Ns_CondTimedWait(&queue->cond,
                                                          &queue->lock, &timeout);
                if (timedOut == NS_TIMEOUT) {
                    Tcl_SetResult(interp, "Wait timed out.", TCL_STATIC);
                    Tcl_SetErrorCode(interp, "NS_TIMEOUT", NULL);
                    result = TCL_ERROR;
                }
            }
        } else {
            while ((Tcl_FirstHashEntry(&queue->jobs, &search) != NULL)
                   && !AnyDone(queue)) {
                Ns_CondWait(&queue->cond, &queue->lock);
            }
        }

        (void)ReleaseQueue(queue, NS_FALSE);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobJobsObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job jos" command.
 *          Returns a list of job IDs in arbitrary order.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobJobsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Queue         *queue = NULL;
    int            result = TCL_OK;
    Ns_ObjvSpec    args[] = {
        {"queueId",  ObjvQueue,    &queue,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Tcl_HashEntry  *hPtr;
        Tcl_HashSearch        search;

        for (hPtr = Tcl_FirstHashEntry(&queue->jobs, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search)
             ) {
            const char *jobIdString = Tcl_GetHashKey(&queue->jobs, hPtr);
            Tcl_AppendElement(interp, jobIdString);
        }
        (void)ReleaseQueue(queue, NS_FALSE);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JobQueuesObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job queues" command.
 *          Returns a list of the current queues.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobQueuesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;
        
        Ns_MutexLock(&tp.queuelock);
        hPtr = Tcl_FirstHashEntry(&tp.queues, &search);
        while (hPtr != NULL) {
            const Queue *queue = Tcl_GetHashValue(hPtr);
            
            Tcl_AppendElement(interp, queue->name);
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&tp.queuelock);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobJobListObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job joblist" command.
 *          Returns a list of all the jobs in the queue.
 *
 *          Every entry of a "job" consists of:
 *             ID
 *             State   (Scheduled, Running, or Done)
 *             Results (or job script, if job has not yet completed).
 *             Code    (Standard Tcl result code)
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobJobListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Queue        *queue;
    int           result = TCL_OK;
    Ns_ObjvSpec   args[] = {
        {"queueId", ObjvQueue, &queue,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_Obj              *jobList;
        const Tcl_HashEntry  *hPtr;
        Tcl_HashSearch        search;

        /*
         * Create a Tcl List to hold the list of jobs.
         */
        jobList = Tcl_NewListObj(0, NULL);
        for (hPtr = Tcl_FirstHashEntry(&queue->jobs, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search)) {
            const char *jobId1, *jobState, *jobCode, *jobType, *jobReq, *jobResults, *jobScript;
            Tcl_Obj    *jobFieldList;
            double      delta;
            char        thrId[32];
            Job        *jobPtr = (Job *)Tcl_GetHashValue(hPtr);

            jobId1     = Tcl_GetHashKey(&queue->jobs, hPtr);
            jobCode    = GetJobCodeStr( jobPtr->code);
            jobState   = GetJobStateStr(jobPtr->state);
            jobType    = GetJobTypeStr( jobPtr->type);
            jobReq     = GetJobReqStr(  jobPtr->req);
            jobResults = Tcl_DStringValue(&jobPtr->results);
            jobScript  = Tcl_DStringValue(&jobPtr->script);

            if ( jobPtr->state == JOB_SCHEDULED || jobPtr->state == JOB_RUNNING) {
                Ns_GetTime(&jobPtr->endTime);
            }
            delta = ComputeDelta(&jobPtr->startTime, &jobPtr->endTime);
            snprintf(thrId, sizeof(thrId), "%" PRIxPTR, jobPtr->tid);

            /*
             * Create a Tcl List to hold the list of job fields.
             */
            jobFieldList = Tcl_NewListObj(0, NULL);
            if (AppendField(interp, jobFieldList, "id",        jobId1) != TCL_OK
                || AppendField(interp, jobFieldList, "state",  jobState) != TCL_OK
                || AppendField(interp, jobFieldList, "results",jobResults) != TCL_OK
                || AppendField(interp, jobFieldList, "script", jobScript) != TCL_OK
                || AppendField(interp, jobFieldList, "code",   jobCode) != TCL_OK
                || AppendField(interp, jobFieldList, "type",   jobType) != TCL_OK
                || AppendField(interp, jobFieldList, "req",    jobReq) != TCL_OK
                || AppendField(interp, jobFieldList, "thread", thrId) != TCL_OK
                || AppendFieldDouble(interp, jobFieldList, "time", delta) != TCL_OK
                || AppendFieldLong(interp, jobFieldList, "starttime", (long)jobPtr->startTime.sec) != TCL_OK
                || AppendFieldLong(interp, jobFieldList, "endtime", (long)jobPtr->endTime.sec) != TCL_OK
                ) {
                Tcl_DecrRefCount(jobList);
                Tcl_DecrRefCount(jobFieldList);
                result = TCL_ERROR;
                goto releaseQueue;
            }

            /*
             * Add the job to the job list
             */
            if (Tcl_ListObjAppendElement(interp, jobList,
                                         jobFieldList) != TCL_OK) {
                Tcl_DecrRefCount(jobList);
                Tcl_DecrRefCount(jobFieldList);
                result = TCL_ERROR;
                goto releaseQueue;
            }
        }
        Tcl_SetObjResult(interp, jobList);
    releaseQueue:
        (void)ReleaseQueue(queue, NS_FALSE);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * JobQueueListObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job queuelist" command.  Returns a list
 *          of all the queues and the queue information.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobQueueListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const Tcl_HashEntry  *hPtr;
        Tcl_HashSearch        search;
        Tcl_Obj              *queueList;
            
        /*
         * Create a Tcl List to hold the list of jobs.
         */
        queueList = Tcl_NewListObj(0, NULL);
        Ns_MutexLock(&tp.queuelock);

        for (hPtr = Tcl_FirstHashEntry(&tp.queues, &search);
             (hPtr != NULL) && (result == TCL_OK);
             hPtr = Tcl_NextHashEntry(&search)
             ) {
            Tcl_Obj     *queueFieldList;
            const char  *queueReq;
            const Queue *queue = Tcl_GetHashValue(hPtr);

            /*
             * Create a Tcl List to hold the list of queue fields.
             */
            queueFieldList = Tcl_NewListObj(0, NULL);
            queueReq = GetQueueReqStr(queue->req);
            /*
             * Add queue name and other fields
             */
            if (AppendField(interp, queueFieldList, "name", queue->name) != TCL_OK
                || AppendField(interp, queueFieldList, "desc", queue->desc) != TCL_OK
                || AppendFieldInt(interp, queueFieldList, "maxthreads", queue->maxThreads) != TCL_OK
                || AppendFieldInt(interp, queueFieldList, "numrunning", queue->nRunning) != TCL_OK
                || AppendField(interp, queueFieldList, "req", queueReq) != TCL_OK
                ) {
                Tcl_DecrRefCount(queueFieldList);
                result = TCL_ERROR;

            } else if (Tcl_ListObjAppendElement(interp, queueList,
                                                queueFieldList) != TCL_OK) {
                Tcl_DecrRefCount(queueFieldList);
                result = TCL_ERROR;
            }
        }

        if (likely( result == TCL_OK )) {
            Tcl_SetObjResult(interp, queueList);
        } else {
            Tcl_DecrRefCount(queueList);
        }
        Ns_MutexUnlock(&tp.queuelock);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobGenIDObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job genID" command.
 *          Generate a unique queue name.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobGenIDObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int  result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        char    buf[100];
        Ns_Time currentTime;
    
        Ns_GetTime(&currentTime);
        Ns_MutexLock(&tp.queuelock);
        snprintf(buf, sizeof(buf), "queue_id_%lx_%" TCL_LL_MODIFIER "x",
                 tp.nextQueueId++, (Tcl_WideInt) currentTime.sec);
        Ns_MutexUnlock(&tp.queuelock);
        
        Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, -1));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JobThreadListObjCmd, subcommand of NsTclJobCmd --
 *
 *	    Implements the "ns_job threadlist" command.
 *          Return a list of the thread pool's fields.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */
static int
JobThreadListObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_Obj    *tpFieldList;
        const char *tpReq;

        /*
         * Create a Tcl List to hold the list of thread fields.
         */
        tpFieldList = Tcl_NewListObj(0, NULL);
        Ns_MutexLock(&tp.queuelock);
        tpReq = GetTpReqStr(tp.req);
        if (AppendFieldInt(interp, tpFieldList, "maxthreads", tp.maxThreads) != TCL_OK
            || AppendFieldInt(interp, tpFieldList, "numthreads", tp.nthreads) != TCL_OK
            || AppendFieldInt(interp, tpFieldList, "numidle", tp.nidle) != TCL_OK
            || AppendField(interp, tpFieldList, "req", tpReq) != TCL_OK
            ) {
            result = TCL_ERROR;
        }
        Ns_MutexUnlock(&tp.queuelock);

        if (likely( result == TCL_OK )) {
            Tcl_SetObjResult(interp, tpFieldList);
        } else {
            Tcl_DecrRefCount(tpFieldList);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclJobCmd --
 *
 *	    Implement the ns_job command to manage background tasks.
 *
 * Results:
 *	    Standard Tcl result.
 *
 * Side effects:
 *	    Jobs may be queued to run in another thread.
 *
 *----------------------------------------------------------------------
 */

int
NsTclJobObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"cancel",     JobCancelObjCmd},
        {"configure",  JobConfigureObjCmd},
        {"create",     JobCreateObjCmd},
        {"delete",     JobDeleteObjCmd},
        {"exists",     JobExistsObjCmd},
        {"genid",      JobGenIDObjCmd},
        {"joblist",    JobJobListObjCmd},
        {"jobs",       JobJobsObjCmd},
        {"queue",      JobQueueObjCmd},
        {"queuelist",  JobQueueListObjCmd},
        {"queues",     JobQueuesObjCmd},
        {"threadlist", JobThreadListObjCmd},
        {"wait",       JobWaitObjCmd},
        {"waitany",    JobWaitAnyObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
}



/*
 *----------------------------------------------------------------------
 *
 * JobThread --
 *
 *	    Background thread for the ns_job command.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    Jobs will be run from the queue.
 *
 *----------------------------------------------------------------------
 */

static void
JobThread(void *UNUSED(arg))
{
    const char        *err;
    Queue             *queue;
    Tcl_HashEntry     *hPtr;
    Tcl_AsyncHandler  async;
    const Ns_Time    *timePtr;
    Ns_Time           wait;
    int               jpt, njobs, tid;

    (void)Ns_WaitForStartup();
    Ns_MutexLock(&tp.queuelock);
    tid = tp.nextThreadId++;
    Ns_ThreadSetName("-ns_job_%x-", tid);
    Ns_Log(Notice, "Starting thread: -ns_job_%x-", tid);

    async = Tcl_AsyncCreate(JobAbort, NULL);

    SetupJobDefaults();

    /*
     * Setting this parameter to > 0 will cause the thread to
     * graceously exit after processing that many job requests,
     * thus initiating kind-of Tcl-level garbage collection.
     */

    jpt = njobs = tp.jobsPerThread;

    while (jpt == 0 || njobs > 0) {
	Job          *jobPtr;
	Tcl_Interp   *interp;
	int           code;
        Ns_ReturnCode status;

        ++tp.nidle;
        status = NS_OK;
        if (tp.timeout.sec > 0 || tp.timeout.usec > 0) {
            Ns_GetTime(&wait);
            Ns_IncrTime(&wait, tp.timeout.sec, tp.timeout.usec);
            timePtr = &wait;
        } else {
            timePtr = NULL;
        }
        jobPtr = NULL;
        while (status == NS_OK &&
               !(tp.req == THREADPOOL_REQ_STOP) &&
               ((jobPtr = GetNextJob()) == NULL)) {
            status = Ns_CondTimedWait(&tp.cond, &tp.queuelock, timePtr);
        }
        --tp.nidle;
        if (tp.req == THREADPOOL_REQ_STOP || jobPtr == NULL) {
            break;
        }

        if (LookupQueue(NULL, jobPtr->queueId, &queue, NS_TRUE) != TCL_OK) {
            Ns_Log(Fatal, "cannot find queue: %s", jobPtr->queueId);
        }
	assert(queue != NULL);

        interp = Ns_TclAllocateInterp(jobPtr->server);

        Ns_GetTime(&jobPtr->endTime);
        Ns_GetTime(&jobPtr->startTime);

        jobPtr->tid   = Ns_ThreadId();
        jobPtr->code  = TCL_OK;
        jobPtr->state = JOB_RUNNING;
        jobPtr->async = async;

        if (jobPtr->cancel != 0) {
            Tcl_AsyncMark(jobPtr->async);
        }

        Ns_ThreadSetName("-%s:%x", jobPtr->queueId, tid);
        ++queue->nRunning;

        Ns_MutexUnlock(&queue->lock);
        Ns_MutexUnlock(&tp.queuelock);

        code = Tcl_EvalEx(interp, jobPtr->script.string, -1, 0);

        Ns_MutexLock(&tp.queuelock);
        Ns_MutexLock(&queue->lock);

        --queue->nRunning;
        Ns_ThreadSetName("-ns_job_%x-", tid);

        jobPtr->state  = JOB_DONE;
        jobPtr->code   = code;
        jobPtr->tid    = 0u;
        jobPtr->async  = NULL;

        Ns_GetTime(&jobPtr->endTime);

        /*
         * Make sure we show error message for detached job, otherwise
         * it will silently disappear
         */

        if (jobPtr->type == JOB_DETACHED && jobPtr->code != TCL_OK) {
            (void) Ns_TclLogErrorInfo(interp, "\n(context: detached job)");
        }

        /*
         * Save the results.
         */

        Tcl_DStringAppend(&jobPtr->results, Tcl_GetStringResult(interp), -1);
        if (jobPtr->code == TCL_ERROR) {
            err = Tcl_GetVar(interp, "errorCode", TCL_GLOBAL_ONLY);
            if (err != NULL) {
                jobPtr->errorCode = ns_strdup(err);
            }
            err = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
            if (err != NULL) {
                jobPtr->errorInfo = ns_strdup(err);
            }
        }

        Ns_TclDeAllocateInterp(interp);

        /*
         * Clean any detached jobs.
         */

        if (jobPtr->type == JOB_DETACHED) {
            hPtr = Tcl_FindHashEntry(&queue->jobs,
                                     Tcl_DStringValue(&jobPtr->id));
            if (hPtr != NULL) {
                Tcl_DeleteHashEntry(hPtr);
            }
            FreeJob(jobPtr);
        }

        Ns_CondBroadcast(&queue->cond);
        (void)ReleaseQueue(queue, NS_TRUE);

        if ((jpt != 0) && --njobs <= 0) {
            break; /* Served given # of jobs in this thread */
        }
    }

    --tp.nthreads;

    Tcl_AsyncDelete(async);
    Ns_CondBroadcast(&tp.cond);
    Ns_MutexUnlock(&tp.queuelock);

    Ns_Log(Notice, "exiting");
}

/*
 *----------------------------------------------------------------------
 +
 * JobAbort --
 *
 *      Called by Tcl async handling when somebody cancels the job.
 *
 * Results:
 *      Always TCL_ERROR.
 *
 * Side effects:
 *      Causes currently executing Tcl command to return TCL_ERROR.
 *
 *----------------------------------------------------------------------
 */

static int
JobAbort(ClientData UNUSED(clientData), Tcl_Interp *interp, int UNUSED(code))
{
    if (interp != NULL) {
        Tcl_SetErrorCode(interp, "ECANCEL", NULL);
        Tcl_SetResult(interp, "Job cancelled.", TCL_STATIC);
    } else {
        Ns_Log(Warning, "ns_job: job cancelled");
    }

    return TCL_ERROR; /* Forces current command error */
}

/*
 *----------------------------------------------------------------------
 *
 * GetNextJob --
 *
 *      Get the next job from the queue.
 *      The queuelock should be held locked.
 *
 * Results:
 *      The job.
 *
 * Side effects:
 *      Queues have a "maxThreads" so if the queue is already
 *      at "maxThreads", jobs of that queue will be skipped.
 *
 *----------------------------------------------------------------------
 */

static Job*
GetNextJob(void)
{
    Queue         *queue;
    Job           *prevPtr, *jobPtr;
    int            done = 0;

    jobPtr = prevPtr = tp.firstPtr;

    while (done == 0 && jobPtr != NULL) {

        if (LookupQueue(NULL, jobPtr->queueId, &queue, NS_TRUE) != TCL_OK) {
            Ns_Log(Fatal, "cannot find queue: %s", jobPtr->queueId);
        }
	assert(queue != NULL);

        if (queue->nRunning < queue->maxThreads) {

            /*
             * Job can be serviced; remove from the pending list
             */
            if (jobPtr == tp.firstPtr) {
                tp.firstPtr = jobPtr->nextPtr;
            } else {
                prevPtr->nextPtr = jobPtr->nextPtr;
            }

            done = 1;

        } else {

            /*
             * Go to next job.
             */
            prevPtr = jobPtr;
            jobPtr = jobPtr->nextPtr;
        }

        (void)ReleaseQueue(queue, NS_TRUE);
    }

    return jobPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NewQueue --
 *
 *	    Create a thread pool queue.
 *
 * Results:
 *	    Thread pool queue.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

static Queue*
NewQueue(const char *queueName, const char *queueDesc, int maxThreads)
{
    Queue *queue;

    NS_NONNULL_ASSERT(queueName != NULL);
    NS_NONNULL_ASSERT(queueDesc != NULL);

    queue = ns_calloc(1u, sizeof(Queue));
    queue->req = QUEUE_REQ_NONE;

    queue->name = ns_strdup(queueName);
    queue->desc = ns_strdup(queueDesc);

    queue->maxThreads = maxThreads;
    queue->refCount = 0;

    Ns_MutexSetName2(&queue->lock, "tcljob", queueName);
    Tcl_InitHashTable(&queue->jobs, TCL_STRING_KEYS);

    tp.maxThreads += maxThreads;

    return queue;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeQueue --
 *
 *	    Cleanup the queue
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

static void
FreeQueue(Queue *queue)
{
    NS_NONNULL_ASSERT(queue != NULL);

    Ns_MutexDestroy(&queue->lock);
    Tcl_DeleteHashTable(&queue->jobs);
    ns_free((char *)queue->desc);
    ns_free((char *)queue->name);
    ns_free(queue);
}


/*
 *----------------------------------------------------------------------
 *
 * NewJob --
 *
 *	    Create a new job and initialize it.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

static Job*
NewJob(const char* server, const char* queueName, JobTypes type, const char *script)
{
    Job *jobPtr;

    NS_NONNULL_ASSERT(queueName != NULL);
    NS_NONNULL_ASSERT(script != NULL);

    jobPtr = ns_calloc(1u, sizeof(Job));

    jobPtr->server = server;
    jobPtr->type   = type;
    jobPtr->state  = JOB_SCHEDULED;
    jobPtr->code   = TCL_OK;
    jobPtr->req    = JOB_NONE;

    jobPtr->queueId = ns_strdup(queueName);

    Tcl_DStringInit(&jobPtr->id);
    Tcl_DStringInit(&jobPtr->script);
    Tcl_DStringAppend(&jobPtr->script, script, -1);
    Tcl_DStringInit(&jobPtr->results);

    return jobPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * FreeJob --
 *
 *	    Destory a Job structure.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

static void
FreeJob(Job *jobPtr)
{
    NS_NONNULL_ASSERT(jobPtr != NULL);

    Tcl_DStringFree(&jobPtr->results);
    Tcl_DStringFree(&jobPtr->script);
    Tcl_DStringFree(&jobPtr->id);

    ns_free((char *)jobPtr->queueId);

    if (jobPtr->errorCode != NULL) {
        ns_free(jobPtr->errorCode);
    }
    if (jobPtr->errorInfo != NULL) {
        ns_free(jobPtr->errorInfo);
    }

    ns_free(jobPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * LookupQueue --
 *
 *      Find the specified queue and lock it if found.
 *      Specify "locked" true if the "queuelock" is already locked.
 *
 *      With the new locking scheme refCount is not longer necessary.
 *      However, if there is a case in the future where an unlocked
 *      queue can be referenced then we will again need the refCount.
 *
 * Results:
 *      Stanard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
LookupQueue(Tcl_Interp *interp, const char *queueName, Queue **queuePtr,
            bool locked)
{
    const Tcl_HashEntry *hPtr;
    int                  result = TCL_OK;

    NS_NONNULL_ASSERT(queuePtr != NULL);
    NS_NONNULL_ASSERT(queueName != NULL);

    if (!locked) {
        Ns_MutexLock(&tp.queuelock);
    }

    *queuePtr = NULL;

    hPtr = Tcl_FindHashEntry(&tp.queues, queueName);
    if (hPtr != NULL) {
        *queuePtr = Tcl_GetHashValue(hPtr);
        Ns_MutexLock(&(*queuePtr)->lock);
        ++(*queuePtr)->refCount;
    }

    if (!locked) {
        Ns_MutexUnlock(&tp.queuelock);
    }

    if (*queuePtr == NULL) {
        if (interp != NULL) {
            Ns_TclPrintfResult(interp, "no such queue: %s", queueName);
        }
        result = TCL_ERROR;
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * ObjvQueue --
 *
 *      objv converter for Queue*.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
static int
ObjvQueue(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
          Tcl_Obj *CONST* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Queue  *queue = NULL;

        result = LookupQueue(interp, Tcl_GetString(objv[0]), &queue, NS_FALSE);
        if (likely(result == TCL_OK)) {
            Queue  **dest = spec->dest;

            *dest = queue;
            *objcPtr -= 1;
        }
    } else {
        result = TCL_ERROR;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ReleaseQueue --
 *
 *      Releases (unlocks) the queue, deleting it of no other thread
 *      is referencing it (queuePtr->refCount <= 0), queue is empty
 *      and queue delete has been requested.
 *
 *      Pass "locked" as true if the queuelock is already held locked.
 *
 * Results:
 *      NS_TRUE if queue was deleted.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static bool
ReleaseQueue(Queue *queue, bool locked)
{
    Tcl_HashSearch  search;
    bool            deleted = NS_FALSE;

    NS_NONNULL_ASSERT(queue != NULL);

    --queue->refCount;

    /*
     * Delete the queue, honouring constraints
     */

    if (queue->req == QUEUE_REQ_DELETE
        && queue->refCount <= 0
        && (Tcl_FirstHashEntry(&queue->jobs, &search) == NULL)) {
        Tcl_HashEntry *qPtr;

        if (!locked) {
            Ns_MutexLock(&tp.queuelock);
        }

        qPtr = Tcl_FindHashEntry(&tp.queues, queue->name);
        if (qPtr != NULL) {
            Tcl_DeleteHashEntry(qPtr);
            tp.maxThreads -= queue->maxThreads;
            deleted = NS_TRUE;
        }

        Ns_MutexUnlock(&queue->lock);
        FreeQueue(queue);

        if (!locked) {
            Ns_MutexUnlock(&tp.queuelock);
        }
    } else {
        Ns_MutexUnlock(&queue->lock);
    }

    return deleted;
}


/*
 *----------------------------------------------------------------------
 *
 * AnyDone --
 *
 *      Check if any jobs on the queue are "done".
 *
 * Results:
 *      True: there is at least one job done.
 *      False: there are no jobs done.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
AnyDone(Queue *queue)
{
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;
    bool                 result = NS_FALSE;

    NS_NONNULL_ASSERT(queue != NULL);

    hPtr = Tcl_FirstHashEntry(&queue->jobs, &search);

    while (hPtr != NULL) {
	const Job *jobPtr = Tcl_GetHashValue(hPtr);

        if (jobPtr->state == JOB_DONE) {
            result = NS_TRUE;
            break;
        }
        hPtr = Tcl_NextHashEntry(&search);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GetJobCodeStr --
 *
 *     Convert the job code into a string.
 *
 * Results:
 *     Standard Tcl result number.
 *
 * Side effects:
 *     None.
 *
 *----------------------------------------------------------------------
 */

static const char*
GetJobCodeStr(int code)
{
    static const int max_code_index = 5;
    static const char *const codeArr[] = {
        "TCL_OK",       /* 0 */
        "TCL_ERROR",    /* 1 */
        "TCL_RETURN",   /* 2 */
        "TCL_BREAK",    /* 3 */
        "TCL_CONTINUE", /* 4 */
        "UNKNOWN_CODE"  /* 5 */
    };

    /* Check the caller's input. */
    if (code > max_code_index) {
        code = max_code_index;
    }

    return codeArr[code];
}


/*
 *----------------------------------------------------------------------
 *
 * GetJobStateStr --
 *
 *      Convert the job states into a string.
 *
 * Results:
 *      The job state.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static const char*
GetJobStateStr(JobStates state)
{
    static const char *const stateArr[] = {
        "scheduled",        /* 0 */
        "running",          /* 1 */
        "done",             /* 2 */
        "unknown"           /* 3 */
    };

    assert((int)JobStatesMax == Ns_NrElements(stateArr) - 1);
    assert((int)JobStatesMax > (int)state);

    return stateArr[state];
}


/*
 *----------------------------------------------------------------------
 *
 * GetJobTypeStr --
 *
 *      Convert the job states into a string.
 *
 * Results:
 *      The job type.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char*
GetJobTypeStr(JobTypes type)
{
    static const char *const typeArr[] = {
        "nondetached",     /* 0 */
        "detached",        /* 1 */
        "unknown"          /* 2 */
    };

    assert((int)JobTypesMax == Ns_NrElements(typeArr) - 1);
    assert((int)JobTypesMax > (int)type);

    return typeArr[type];
}


/*
 *----------------------------------------------------------------------
 *
 * GetJobReqStr --
 *
 *      Convert the job req into a string.
 *
 * Results:
 *      The job status.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char*
GetJobReqStr(JobRequests req)
{
    static const char *const reqArr[] = {
        "none",     /* 0 */
        "wait",     /* 1 */
        "unknown"   /* 2 */
    };

    assert((int)JobRequestsMax == Ns_NrElements(reqArr) - 1);
    assert((int)JobRequestsMax > (int)req);

    return reqArr[req];
}


/*
 *----------------------------------------------------------------------
 *
 * GetQueueReqStr --
 *
 *      Convert the queue req into a string.
 *
 * Results:
 *      The queue status
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char*
GetQueueReqStr(QueueRequests req)
{
    static const char *const reqArr[] = {
        "none",      /* 0 */
        "delete",    /* 1 */
        "unknown"    /* 2 */
    };

    assert((int)QueueRequestsMax == Ns_NrElements(reqArr) - 1);
    assert((int)QueueRequestsMax > (int)req);

    return reqArr[req];
}


/*
 *----------------------------------------------------------------------
 *
 * GetTpReqStr --
 *
 *      Convert the thread pool req into a string.
 *
 * Results:
 *      The threadpool status.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const char*
GetTpReqStr(ThreadPoolRequests req)
{
    static const char *const reqArr[] = {
        "none",      /* 0 */
        "stop",      /* 1 */
        "unknown"    /* 2 */
    };

    assert((int)ThreadPoolRequestsMax == Ns_NrElements(reqArr) - 1);
    assert((int)ThreadPoolRequestsMax > (int)req);

    return reqArr[req];
}


/*
 *----------------------------------------------------------------------
 *
 * AppendField --
 *
 *      Append job field to the job field list.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AppendField(Tcl_Interp *interp, Tcl_Obj *list, const char *name,
            const char *value)
{
    Tcl_Obj *elObj;
    int      result;

    NS_NONNULL_ASSERT(list != NULL);
    NS_NONNULL_ASSERT(name != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    /*
     * Note: If there is an error occurs within Tcl_ListObjAppendElement
     * it will set the result anyway.
     */

    elObj = Tcl_NewStringObj(name, -1);

    result = Tcl_ListObjAppendElement(interp, list, elObj);
    if (likely( result == TCL_OK) ) {
        elObj = Tcl_NewStringObj(value, -1);
        result = Tcl_ListObjAppendElement(interp, list, elObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AppendFieldInt --
 *
 *      Append job field to the job field list.
 *
 * Results:
 *
 * Side effects:
 *
 *----------------------------------------------------------------------
 */
static int
AppendFieldInt(Tcl_Interp *interp, Tcl_Obj *list, const char *name, int value)
{
    Tcl_Obj *elObj;
    int      result;

    NS_NONNULL_ASSERT(list != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    /*
     * Note: If there is an error occurs within Tcl_ListObjAppendElement
     * it will set the result anyway
     */

    elObj = Tcl_NewStringObj(name, -1);
    result = Tcl_ListObjAppendElement(interp, list, elObj);
    if (likely (result == TCL_OK) ) {
        elObj = Tcl_NewIntObj(value);
        result = Tcl_ListObjAppendElement(interp, list, elObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AppendFieldLong --
 *
 *      Append job field to the job field list.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AppendFieldLong(Tcl_Interp *interp, Tcl_Obj *list, const char *name,
                long value)
{
    Tcl_Obj *elObj;
    int      result;

    NS_NONNULL_ASSERT(list != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    elObj = Tcl_NewStringObj(name, -1);
    result = Tcl_ListObjAppendElement(interp, list, elObj);
    if (likely( result == TCL_OK )) {
        elObj = Tcl_NewLongObj(value);
        result = Tcl_ListObjAppendElement(interp, list, elObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * AppendFieldDouble --
 *
 *      Append the job field to the job field list.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
AppendFieldDouble(Tcl_Interp *interp, Tcl_Obj *list, const char *name,
                  double value)
{
    Tcl_Obj *elObj;
    int      result;

    NS_NONNULL_ASSERT(list != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    elObj = Tcl_NewStringObj(name, -1);
    result = Tcl_ListObjAppendElement(interp, list, elObj);
    if (likely( result == TCL_OK )) {
        elObj = Tcl_NewDoubleObj(value);
        result = Tcl_ListObjAppendElement(interp, list, elObj);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ComputeDelta --
 *
 *      Compute the time difference   .
 *
 * Results:
 *      Difference in milliseconds
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static double
ComputeDelta(const Ns_Time *start, const Ns_Time *end)
{
    Ns_Time diff;

    NS_NONNULL_ASSERT(start != NULL);
    NS_NONNULL_ASSERT(end != NULL);

    (void)Ns_DiffTime(end, start, &diff);

    return ((double)diff.sec * 1000.0) + ((double)diff.usec / 1000.0);
}

/*
 *----------------------------------------------------------------------
 *
 * SetupJobDefaults --
 *
 *      Assigns default configuration parameters if not set yet
 *
 * Results:
 *      None
 *
 * Side effects:
 *      jobsperthread and jobtimeout may be changed
 *
 *----------------------------------------------------------------------
 */
static void
SetupJobDefaults(void)
{
    if(tp.jobsPerThread == 0) {
       tp.jobsPerThread = nsconf.job.jobsperthread;
    }
    if (tp.timeout.sec == 0 && tp.timeout.usec == 0) {
        tp.timeout.sec = nsconf.job.timeout;
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
