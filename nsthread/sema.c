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
 * sema.c --
 *
 *      Counting semaphore routines.  Semaphores differ from ordinary mutex
 *      locks in that they maintain a count instead of a simple locked/unlocked
 *      state.  Threads block if the semaphore count is less than one.
 *
 *      Note:  In general, cleaner code can be implemented with condition variables.
 */

#include "thread.h"

/*
 * The following structure defines a counting semaphore using a lock
 * and condition.
 */

typedef struct {
    Ns_Mutex lock;
    Ns_Cond  cond;
    int      count;
} Sema;


/*
 *----------------------------------------------------------------------
 *
 * Ns_SemaInit --
 *
 *      Initialize a semaphore.   Note that because semaphores are
 *      initialized with a starting count they cannot be automatically
 *      created on first use as with other synchronization objects.
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
Ns_SemaInit(Ns_Sema *semaPtr, int count)
{
    static uintptr_t nextid = 0u;
    Sema *sPtr;

    NS_NONNULL_ASSERT(semaPtr != NULL);

    sPtr = ns_malloc(sizeof(Sema));
    sPtr->count = count;
    NsMutexInitNext(&sPtr->lock, "sm", &nextid);
    Ns_CondInit(&sPtr->cond);
    *semaPtr = (Ns_Sema) sPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SemaDestroy --
 *
 *      Destroy a semaphore.  This routine is almost never used as
 *      synchronization objects are normally created at process startup
 *      and exist until the process exits.
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
Ns_SemaDestroy(Ns_Sema *semaPtr)
{
    NS_NONNULL_ASSERT(semaPtr != NULL);

    if (*semaPtr != NULL) {
        Sema *sPtr = (Sema *) *semaPtr;

        Ns_MutexDestroy(&sPtr->lock);
        Ns_CondDestroy(&sPtr->cond);
        ns_free(sPtr);
        *semaPtr = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SemaWait --
 *
 *      Wait for a semaphore count to be greater than zero.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Calling thread may wait on the condition.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SemaWait(Ns_Sema *semaPtr)
{
    Sema *sPtr;

    NS_NONNULL_ASSERT(semaPtr != NULL);

    sPtr = (Sema *) *semaPtr;
    Ns_MutexLock(&sPtr->lock);
    while (sPtr->count == 0) {
        Ns_CondWait(&sPtr->cond, &sPtr->lock);
    }
    sPtr->count--;
    Ns_MutexUnlock(&sPtr->lock);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SemaPost --
 *
 *      Increment a semaphore count, releasing waiting threads if needed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Threads waiting on the condition, if any, may be resumed.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SemaPost(Ns_Sema *semaPtr, int count)
{
    Sema *sPtr;

    NS_NONNULL_ASSERT(semaPtr != NULL);

    sPtr = (Sema *) *semaPtr;
    Ns_MutexLock(&sPtr->lock);
    sPtr->count += count;
    if (count == 1) {
        Ns_CondSignal(&sPtr->cond);
    } else {
        Ns_CondBroadcast(&sPtr->cond);
    }
    Ns_MutexUnlock(&sPtr->lock);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
