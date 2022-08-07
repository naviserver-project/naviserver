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
 * cslock.c --
 *
 *      Support for critical sections.  Critical sections differ
 *      from mutexes in that a critical section can be repeatedly
 *      locked by the same thread as long as each lock is matched with
 *      a corresponding unlock.  Critical sections are used in cases
 *      where the lock could be called recursively, e.g., for the
 *      Ns_MasterLock.
 *
 *      Note:  Critical sections are almost always a bad idea.  You'll
 *      see below that the number of actual lock and unlock operations are
 *      doubled and threads can end up in condition waits instead of spin
 *      locks.
 */

#include "thread.h"

/*
 * The following structure defines a critcal section including a mutex,
 * thread id of the owner, and a condition variable for waiting threads.
 */

typedef struct CsLock {
    Ns_Mutex        mutex;
    Ns_Cond         cond;
    uintptr_t       tid;
    int             count;
} CsLock;


/*
 *----------------------------------------------------------------------
 *
 * Ns_CsInit --
 *
 *      Initialize a critical section object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A critical section object is allocated from the heap.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CsInit(Ns_Cs *csPtr)
{
    CsLock     *lockPtr;
    static uintptr_t nextid = 0u;

    NS_NONNULL_ASSERT(csPtr != NULL);

    lockPtr = ns_malloc(sizeof(CsLock));
    lockPtr->mutex = NULL;
    NsMutexInitNext(&lockPtr->mutex, "cs", &nextid);
    Ns_CondInit(&lockPtr->cond);
    lockPtr->count = 0;
    *csPtr = (Ns_Cs) lockPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CsDestroy --
 *
 *      Destroy a critical section object.  Note that you would almost
 *      never need to call this function as synchronization objects are
 *      typically created at startup and exist until the server exits.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The underly objects in the critical section are destroy and
 *      the critical section memory returned to the heap.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CsDestroy(Ns_Cs *csPtr)
{
    CsLock *lockPtr = (CsLock *) *csPtr;

    /*
     * Destroy the condition only if it is not null, i.e., initialized
     * by the first use.
     */

    if (lockPtr != NULL) {
        Ns_MutexDestroy(&lockPtr->mutex);
        Ns_CondDestroy(&lockPtr->cond);
        lockPtr->count = 0;
        ns_free(lockPtr);
        *csPtr = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CsEnter --
 *
 *      Lock a critical section object, initializing it first if needed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread may wait on the critical section condition variable if
 *      the critical section is already owned by another thread.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CsEnter(Ns_Cs *csPtr)
{
    CsLock    *lockPtr;
    uintptr_t  tid = Ns_ThreadId();
#ifndef NS_NO_MUTEX_TIMING
    Ns_Time end, diff, startTime;

    Ns_GetTime(&startTime);
#endif
    //fprintf(stderr, "[%" PRIxPTR "] Ns_CsEnter %p\n",  Ns_ThreadId(), (void*)csPtr);

    /*
     * Initialize the critical section if it has never been used before.
     */

    if (*csPtr == NULL) {
        Ns_MasterLock();
        if (*csPtr == NULL) {
            Ns_CsInit(csPtr);
        }
        Ns_MasterUnlock();
    }
    lockPtr = (CsLock *) *csPtr;

    /*
     * Wait on the condition if the critical section is owned by another
     * thread.
     */
    Ns_MutexLock(&lockPtr->mutex);
    while (lockPtr->count > 0 && lockPtr->tid != tid) {
        Ns_CondWait(&lockPtr->cond, &lockPtr->mutex);
    }
    lockPtr->tid = tid;
    lockPtr->count++;
    Ns_MutexUnlock(&lockPtr->mutex);

#ifndef NS_NO_MUTEX_TIMING
    /*
     * Measure waiting time for busy CsLocks locks.
     */
    Ns_GetTime(&end);
    Ns_DiffTime(&end, &startTime, &diff);
    //Ns_IncrTime(&mutexPtr->total_waiting_time, diff.sec, diff.usec);

    if (NS_mutexlocktrace && (diff.sec > 0 || diff.usec > 100000)) {
        /*
         * We can't use Ns_ThreadGetName() here, since at least at the start,
         * it requires a master lock.
         */
        fprintf(stderr, "[%" PRIxPTR "] Ns_CsEnter %p: wait duration " NS_TIME_FMT "\n",
                 Ns_ThreadId(), (void*)csPtr, (int64_t)diff.sec, diff.usec);
    }

    /*
     * To calculate locak duration, we have to extend the CsLock structure)
     */
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CsLeave --
 *
 *      Unlock a critical section once.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Condition is signaled if this is the final unlock of the critical
 *      section.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CsLeave(Ns_Cs *csPtr)
{
    CsLock *lockPtr;

    NS_NONNULL_ASSERT(csPtr != NULL);
    lockPtr = (CsLock *) *csPtr;

    Ns_MutexLock(&lockPtr->mutex);
    if (--lockPtr->count == 0) {
        Ns_CondSignal(&lockPtr->cond);
    }
    Ns_MutexUnlock(&lockPtr->mutex);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
