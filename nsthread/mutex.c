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
 * mutex.c --
 *
 *      Mutex locks with metering.
 */

#include "thread.h"

/*
 * On Windows, Mutex timings can lead to a lock-up during start, when
 * Tcl_GetTime() is used, since the first calls to the mutex are
 * issued from DllMain() at a time before Tcl is initialized. Per
 * default, Windows compilations use the windows-native get time
 * implementation, so it does not harm. However, implementations might
 * choose to compile without mutex timings. In such cases,
 * NS_NO_MUTEX_TIMING should be set during compilation
 */
/*
 * #define NS_NO_MUTEX_TIMING 1
 */

bool NS_mutexlocktrace = NS_FALSE;


/*
 * The following structure defines a mutex with
 * string name and lock and busy counters.
 */

typedef struct Mutex {
    void            *lock;
    struct Mutex    *nextPtr;
    uintptr_t        id;
    unsigned long    nlock;
    unsigned long    nbusy;
    Ns_Time          start_time;
    Ns_Time          total_waiting_time;
    Ns_Time          max_waiting_time;
    Ns_Time          total_lock_time;
    char             name[NS_THREAD_NAMESIZE+1];
} Mutex;

#define GETMUTEX(mutex) (*(mutex) != NULL ? ((Mutex *)*(mutex)) : GetMutex((mutex)))

static Mutex *GetMutex(Ns_Mutex *mutex) NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;
static Mutex *firstMutexPtr = NULL;


/*
 *----------------------------------------------------------------------

 * Ns_MutexInit --
 *
 *      Mutex initialization, often called the first time a mutex
 *      is locked.
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
Ns_MutexInit(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;
    static uintptr_t nextid = 0u;

    NS_NONNULL_ASSERT(mutex != NULL);

    mutexPtr = ns_calloc(1u, sizeof(Mutex));
    mutexPtr->lock = NsLockAlloc();
    Ns_MasterLock();
    mutexPtr->nextPtr = firstMutexPtr;
    firstMutexPtr = mutexPtr;
    mutexPtr->id = nextid++;

    mutexPtr->name[0] = 'm';
    mutexPtr->name[1] = 'u';
    (void) ns_uint64toa(&mutexPtr->name[2], (uint64_t)mutexPtr->id);

    Ns_MasterUnlock();
    /*fprintf(stderr, "=== created mutex %ld name %s\n", mutexPtr->id, mutexPtr->name);*/
    *mutex = (Ns_Mutex) mutexPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexSetName, Ns_MutexSetName2 --
 *
 *      Update the string name of a mutex.  Ns_MutexSetName2 produces a name
 *      based on the two string components and concatenates these with a colon
 *      (":").
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
Ns_MutexSetName(Ns_Mutex *mutex, const char *name)
{
    NS_NONNULL_ASSERT(mutex != NULL);
    NS_NONNULL_ASSERT(name != NULL);

    Ns_MutexSetName2(mutex, name, NULL);
}

void
Ns_MutexSetName2(Ns_Mutex *mutex, const char *prefix, const char *name)
{
    Mutex  *mutexPtr;
    size_t  prefixLength, nameLength;
    char   *p;

    NS_NONNULL_ASSERT(mutex != NULL);
    NS_NONNULL_ASSERT(prefix != NULL);

    prefixLength = strlen(prefix);
    if (prefixLength > NS_THREAD_NAMESIZE - 1) {
        prefixLength = NS_THREAD_NAMESIZE - 1;
        nameLength = 0u;
    } else if (name != NULL) {
        nameLength = strlen(name);
        if ((nameLength + prefixLength + 1) > NS_THREAD_NAMESIZE) {
            nameLength = NS_THREAD_NAMESIZE - prefixLength - 1;
        }
    } else {
        nameLength = 0u;
    }

    mutexPtr = GETMUTEX(mutex);
    assert(mutexPtr != NULL);

    Ns_MasterLock();
    p = mutexPtr->name;
    memcpy(p, prefix, prefixLength + 1u);
    if (name != NULL) {
        p += prefixLength;
        *p++ = ':';
        assert(name != NULL);
        memcpy(p, name, nameLength + 1u);
    }
    Ns_MasterUnlock();

    //fprintf(stderr, "=== renaming mutex %ld to %s\n", mutexPtr->id, mutexPtr->name);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexDestroy --
 *
 *      Mutex destroy.  Note this routine is not used very often
 *      as mutexes normally exists in memory until the process exits.
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
Ns_MutexDestroy(Ns_Mutex *mutex)
{
    Mutex        *mutexPtr = (Mutex *) *mutex;

    if (mutexPtr != NULL) {
        Mutex  **mutexPtrPtr;

        NsLockFree(mutexPtr->lock);
        Ns_MasterLock();
        mutexPtrPtr = &firstMutexPtr;
        while ((*mutexPtrPtr) != mutexPtr) {
            mutexPtrPtr = &(*mutexPtrPtr)->nextPtr;
        }
        *mutexPtrPtr = mutexPtr->nextPtr;
        Ns_MasterUnlock();
        ns_free(mutexPtr);
        *mutex = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexLock --
 *
 *      Lock a mutex, tracking the number of locks and the number of
 *      which were not acquired immediately.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread may be suspended if the lock is held.
 *
 *----------------------------------------------------------------------
 */

void
Ns_MutexLock(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;
#ifndef NS_NO_MUTEX_TIMING
    Ns_Time startTime;

    Ns_GetTime(&startTime);
#endif

    NS_NONNULL_ASSERT(mutex != NULL);

    mutexPtr = GETMUTEX(mutex);
    assert(mutexPtr != NULL);
    if (unlikely(!NsLockTry(mutexPtr->lock))) {
        NsLockSet(mutexPtr->lock);
        ++mutexPtr->nbusy;

#ifndef NS_NO_MUTEX_TIMING
        {
            Ns_Time endTime, diffTime;
            long    delta;

            /*
             * Measure total and max waiting time for busy mutex locks.
             */
            Ns_GetTime(&endTime);
            delta = Ns_DiffTime(&endTime, &startTime, &diffTime);
            if (likely(delta >= 0)) {
                Ns_IncrTime(&mutexPtr->total_waiting_time, diffTime.sec, diffTime.usec);

                if (NS_mutexlocktrace && (diffTime.sec > 0 || diffTime.usec > 100000)) {
                    fprintf(stderr, "[%s] Mutex lock %s: wait duration " NS_TIME_FMT "\n",
                            Ns_ThreadGetName(), mutexPtr->name, (int64_t)diffTime.sec, diffTime.usec);
                }
            } else {
                fprintf(stderr, "[%s] Mutex lock %s warning: wait duration " NS_TIME_FMT " is negative\n",
                        Ns_ThreadGetName(), mutexPtr->name, (int64_t)diffTime.sec, diffTime.usec);
            }

            /*
             * Keep max waiting time since server start. It might be a
             * good idea to either provide a call to reset the max-time,
             * or to report wait times above a certain threshold (as an
             * extra value in the statistics, or in the log file).
             */
            if (Ns_DiffTime(&mutexPtr->max_waiting_time, &diffTime, NULL) < 0) {
                mutexPtr->max_waiting_time = diffTime;
                /*fprintf(stderr, "Mutex %s max time " NS_TIME_FMT "\n",
                  mutexPtr->name, (int64_t)diff.sec, diff.usec);*/
            }
        }
#endif
    }
#ifndef NS_NO_MUTEX_TIMING
    mutexPtr->start_time = startTime;
#endif
    ++mutexPtr->nlock;

}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexTryLock --
 *
 *      Attempt to lock a mutex.
 *
 * Results:
 *      NS_OK if locked, NS_TIMEOUT if lock already held.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_MutexTryLock(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;

    NS_NONNULL_ASSERT(mutex != NULL);

    mutexPtr = GETMUTEX(mutex);
    if (!NsLockTry(mutexPtr->lock)) {
        return NS_TIMEOUT;
    }
    ++mutexPtr->nlock;
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexUnlock --
 *
 *      Unlock a mutex.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Other waiting thread, if any, is resumed.
 *
 *----------------------------------------------------------------------
 */

void
Ns_MutexUnlock(Ns_Mutex *mutex)
{
    Mutex *mutexPtr = (Mutex *) *mutex;

#ifndef NS_NO_MUTEX_TIMING
    Ns_Time end, diff;

    Ns_GetTime(&end);
    Ns_DiffTime(&end, &mutexPtr->start_time, &diff);
    Ns_IncrTime(&mutexPtr->total_lock_time, diff.sec, diff.usec);
#endif

    NsLockUnset(mutexPtr->lock);

    if (NS_mutexlocktrace && (diff.sec > 1 || diff.usec > 100000)) {
        fprintf(stderr, "[%s] Mutex unlock %s: lock duration " NS_TIME_FMT "\n",
                Ns_ThreadGetName(), mutexPtr->name, (int64_t)diff.sec, diff.usec);
    }

#ifdef NS_MUTEX_NAME_DEBUG
    /*
     * In case we find a mutex with the name starting with 'mu[0-9]', produce
     * a crash.  We assume here, that the user does not name mutexes like
     * this. However, this should NOT be active in production environments.
     */
    if (mutexPtr->name[0] == 'm'
        && mutexPtr->name[1] == 'u'
        && mutexPtr->name[2] >= '0'
        && mutexPtr->name[2] <= '9'
        ) {
        char *p = NULL;

        fprintf(stderr, "anonymous mutex: with id %ld name %s\n", mutexPtr->id, mutexPtr->name);
        *p = 'a';
    }
#endif

}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexList --
 *
 *      Append info on each lock.
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
Ns_MutexList(Tcl_DString *dsPtr)
{
    Mutex *mutexPtr;
    char buf[200];

    Ns_MasterLock();
    for (mutexPtr = firstMutexPtr; mutexPtr != NULL; mutexPtr = mutexPtr->nextPtr) {
        Tcl_DStringStartSublist(dsPtr);
        Tcl_DStringAppendElement(dsPtr, mutexPtr->name);
        Tcl_DStringAppendElement(dsPtr, ""); /* unused? */
        snprintf(buf, (int)sizeof(buf),
                 " %" PRIuPTR " %lu %lu " NS_TIME_FMT " " NS_TIME_FMT " " NS_TIME_FMT,
                 mutexPtr->id, mutexPtr->nlock, mutexPtr->nbusy,
                 (int64_t)mutexPtr->total_waiting_time.sec, mutexPtr->total_waiting_time.usec,
                 (int64_t)mutexPtr->max_waiting_time.sec, mutexPtr->max_waiting_time.usec,
                 (int64_t)mutexPtr->total_lock_time.sec, mutexPtr->total_lock_time.usec
                 );
        Tcl_DStringAppend(dsPtr, buf, TCL_INDEX_NONE);
        Tcl_DStringEndSublist(dsPtr);
    }
    Ns_MasterUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * NsMutexInitNext --
 *
 *      Initialize and name the next internal mutex.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Given counter is updated.
 *
 *----------------------------------------------------------------------
 */

void
NsMutexInitNext(Ns_Mutex *mutex, const char *prefix, uintptr_t *nextPtr)
{
    uintptr_t id;
    char buf[NS_THREAD_NAMESIZE];

    NS_NONNULL_ASSERT(mutex != NULL);
    NS_NONNULL_ASSERT(prefix != NULL);
    NS_NONNULL_ASSERT(nextPtr != NULL);

    Ns_MasterLock();
    id = *nextPtr;
    *nextPtr = id + 1u;
    Ns_MasterUnlock();
    snprintf(buf, sizeof(buf), "ns:%s:%" PRIuPTR, prefix, id);
    Ns_MutexInit(mutex);
    Ns_MutexSetName(mutex, buf);
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetLock --
 *
 *      Return the private lock pointer for an Ns_Mutex.
 *
 * Results:
 *      Pointer to lock.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
NsGetLock(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;

    NS_NONNULL_ASSERT(mutex != NULL);

    mutexPtr = GETMUTEX(mutex);
    assert(mutexPtr != NULL);

    return mutexPtr->lock;
}


/*
 *----------------------------------------------------------------------
 *
 * GetMutex --
 *
 *      Cast an Ns_Mutex to a Mutex, initializing if needed.
 *
 * Results:
 *      Pointer to Mutex.
 *
 * Side effects:
 *      Mutex is initialized the first time.
 *
 *----------------------------------------------------------------------
 */

static Mutex *
GetMutex(Ns_Mutex *mutex)
{
    NS_NONNULL_ASSERT(mutex != NULL);

    Ns_MasterLock();
    if (*mutex == NULL) {
        Ns_MutexInit(mutex);
    }
    Ns_MasterUnlock();
    return (Mutex *) *mutex;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexGetName --
 *
 *      Obtain the name of a mutex.
 *
 * Results:
 *      String name of the mutex.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_MutexGetName(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;

    NS_NONNULL_ASSERT(mutex != NULL);

    mutexPtr = GETMUTEX(mutex);
    return mutexPtr->name;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
