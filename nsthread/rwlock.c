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
 * rwlock.c --
 *
 *      Routines for read/write locks.  Read/write locks differ from a mutex
 *      in that multiple threads can acquire the read lock until a single
 *      thread acquires a write lock.  This code is adapted from that in
 *      Steven's Unix Network Programming, Volume 3.
 *
 *      Note:  Read/write locks are not often a good idea.  The reason
 *      is, like critical sections, the number of actual lock operations
 *      is doubled which makes them more expensive to use.  Cases where the
 *      overhead are justified are then often subject to read locks being
 *      held longer than writer threads can wait and/or writer threads holding
 *      the lock so long that many reader threads back up.  In these cases,
 *      specific reference counting techniques (e.g., the management of
 *      the Req structures in op.c) normally work better.
 */

#include "thread.h"

/*
 * This file contains two different implementations of read/write locks:
 *
 *   a) a POSIX pthread based implementation (when HAVE_PTHREAD is defined)
 *   b) a "hand-written implementation based on a mutex and condition variables,
 *    which is defined since ages in NaviServer.
 *
 * Variant (b) is used typically on WINDOWS, unless one integrtion the windows
 * pthread library.
 *
 */


#ifdef HAVE_PTHREAD

/* ----------------------------------------------------------------------
 * POSIX rwlock
 *----------------------------------------------------------------------
 */
#include <pthread.h>

/*
 * Use MUTEX_TIMING to activate/deactivate timing statistics from locks. for
 * RWLOCKS, we measure just the write locks, which also guarantee exclusive
 * access. The name MUTEX_TIMING is kept as it used as well in mutex.c.
 */
//#define NS_NO_MUTEX_TIMING 1

/*
 * The following structure defines a read/write lock including a mutex
 * to protect access to the structure and condition variables for waiting
 * reader and writer threads.
 */

typedef struct RwLock {
    pthread_rwlock_t rwlock;
    unsigned long    nlock;
    unsigned long    nrlock;
    unsigned long    nwlock;
    unsigned long    nbusy;
    struct RwLock   *nextPtr;
    uintptr_t        id;
    Ns_Time          start_time;
    Ns_Time          total_waiting_time;
    Ns_Time          max_waiting_time;
    Ns_Time          total_lock_time;
    NS_RW            rw;
    char             name[NS_THREAD_NAMESIZE+1];
} RwLock;

static RwLock *GetRwLock(Ns_RWLock *rwPtr, const char *caller)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_RETURNS_NONNULL;


static RwLock *firstRwlockPtr = NULL;

/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockList --
 *
 *      Append info on each lock to Tcl_DString.
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
Ns_RWLockList(Tcl_DString *dsPtr)
{
    RwLock *rwlockPtr;
    char    buf[256];

    Ns_MasterLock();
    for (rwlockPtr = firstRwlockPtr; rwlockPtr != NULL; rwlockPtr = rwlockPtr->nextPtr) {
        Tcl_DStringStartSublist(dsPtr);
        Tcl_DStringAppendElement(dsPtr, rwlockPtr->name);
        Tcl_DStringAppendElement(dsPtr, ""); /* unused? */
#ifndef NS_NO_MUTEX_TIMING
        snprintf(buf, (int)sizeof(buf),
                 " %" PRIuPTR " %lu %lu " NS_TIME_FMT " " NS_TIME_FMT " " NS_TIME_FMT
                 " %lu %lu" ,
                 rwlockPtr->id, rwlockPtr->nlock, rwlockPtr->nbusy,
                 (int64_t)rwlockPtr->total_waiting_time.sec, rwlockPtr->total_waiting_time.usec,
                 (int64_t)rwlockPtr->max_waiting_time.sec, rwlockPtr->max_waiting_time.usec,
                 (int64_t)rwlockPtr->total_lock_time.sec, rwlockPtr->total_lock_time.usec,
                 rwlockPtr->nrlock, rwlockPtr->nwlock);
#else
        snprintf(buf, (int)sizeof(buf),
                 " %" PRIuPTR " %lu %lu " NS_TIME_FMT " " NS_TIME_FMT " " NS_TIME_FMT
                 " %lu %lu" ,
                 rwlockPtr->id, rwlockPtr->nlock, rwlockPtr->nbusy,
                 (int64_t)0, (long)0,
                 (int64_t)0, (long)0,
                 (int64_t)0, (long)0,
                 rwlockPtr->nrlock, rwlockPtr->nwlock);
#endif
        Tcl_DStringAppend(dsPtr, buf, TCL_INDEX_NONE);
        Tcl_DStringEndSublist(dsPtr);
    }
    Ns_MasterUnlock();
}





/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockInit --
 *
 *      Initialize a read/write lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Lock memory is allocated from the heap and initialized.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockInit(Ns_RWLock *rwPtr)
{
    RwLock          *lockPtr;
    static uintptr_t nextid = 0;

    NS_NONNULL_ASSERT(rwPtr != NULL);

    lockPtr = ns_calloc(1u, sizeof(RwLock));

    Ns_MasterLock();
    lockPtr->nextPtr = firstRwlockPtr;
    firstRwlockPtr = lockPtr;
    lockPtr->id = nextid++;
    lockPtr->name[0] = 'r';
    lockPtr->name[1] = 'w';
    (void) ns_uint64toa(&lockPtr->name[2], (uint64_t)lockPtr->id);
    Ns_MasterUnlock();

    lockPtr->rw = NS_READ;
    {
        int err;
#if (defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 500) || (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L)
        pthread_rwlockattr_t attr;
        pthread_rwlockattr_setkind_np(&attr,
                                      PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        err = pthread_rwlock_init(&lockPtr->rwlock, &attr);
#else
        err = pthread_rwlock_init(&lockPtr->rwlock, NULL);
#endif
        if (err != 0) {
            NsThreadFatal("Ns_RWLockInit", "pthread_rwlock_init", err);
        }
    }
    *rwPtr = (Ns_RWLock) lockPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockSetName2  --
 *
 *      Update the string name of a rwlock.  Ns_RWLockSetName2 uses
 *      Ns_MutexSetName2 to set a name based on the two string components and
 *      concatenates these with a colon (":").
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
Ns_RWLockSetName2(Ns_RWLock *rwPtr, const char *prefix, const char *name)
{
    RwLock *lockPtr;
    size_t  prefixLength, nameLength;
    char   *p;

    NS_NONNULL_ASSERT(rwPtr != NULL);
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

    if (*rwPtr == NULL) {
        Ns_RWLockInit(rwPtr);
    }
    lockPtr = GetRwLock(rwPtr, "Ns_RWLockSetName2");

    Ns_MasterLock();
    p = lockPtr->name;
    memcpy(p, prefix, prefixLength + 1u);
    if (name != NULL) {
        p += prefixLength;
        *p++ = ':';
        assert(name != NULL);
        memcpy(p, name, nameLength + 1u);
    }
    Ns_MasterUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockDestroy --
 *
 *      Destroy a read/write lock if it was previously initialized.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Read/write lock objects are destroy and the lock memory is
 *      returned to the heap.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockDestroy(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr = (RwLock *) *rwPtr;

    if (lockPtr != NULL) {
        RwLock  **rwlockPtrPtr;
        int err = pthread_rwlock_destroy(&lockPtr->rwlock);

         if (unlikely(err != 0)) {
             NsThreadFatal("Ns_RWLockDestroy", "pthread_rwlock_destroy", err);
         }
         /*
          * Remove lock from linked list of rwlocks.
          */
         Ns_MasterLock();
         rwlockPtrPtr = &firstRwlockPtr;
         while ((*rwlockPtrPtr) != lockPtr) {
             rwlockPtrPtr = &(*rwlockPtrPtr)->nextPtr;
         }
         *rwlockPtrPtr = lockPtr->nextPtr;
         Ns_MasterUnlock();

        *rwPtr = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockRdLock --
 *
 *      Acquire a read lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread may wait on a condition variable if the read/write lock
 *      currently has a write lock.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockRdLock(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr;
    int     err;
    bool    busy;

    NS_NONNULL_ASSERT(rwPtr != NULL);

    lockPtr = GetRwLock(rwPtr, "Ns_RWLockRdLock");

    err = pthread_rwlock_tryrdlock(&lockPtr->rwlock);
    if (unlikely(err == EBUSY)) {
        busy = NS_TRUE;
    } else if (unlikely(err != 0)) {
        busy = NS_FALSE;
        NsThreadFatal("Ns_RWLockRdLock", "pthread_rwlock_tryrdlock", err);
    } else {
        busy = NS_FALSE;
    }

    if (busy) {
        err = pthread_rwlock_rdlock(&lockPtr->rwlock);
        if (err != 0) {
            NsThreadFatal("Ns_RWLockRdLock", "pthread_rwlock_rdlock", err);
        }
        lockPtr->nbusy++;
    }
    lockPtr->nlock++;
    lockPtr->nrlock++;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockWrLock --
 *
 *      Acquire a write lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread may wait on the write condition if other threads either
 *      have the lock read or write locked.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockWrLock(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr;
    int     err;
    bool    busy;
#ifndef NS_NO_MUTEX_TIMING
    Ns_Time end, diff, startTime;
#endif

    NS_NONNULL_ASSERT(rwPtr != NULL);

    lockPtr = GetRwLock(rwPtr, "Ns_RWLockWrLock");

#ifndef NS_NO_MUTEX_TIMING
    Ns_GetTime(&startTime);
#endif

    err = pthread_rwlock_trywrlock(&lockPtr->rwlock);
    if (unlikely(err == EBUSY)) {
        busy = NS_TRUE;
    } else if (unlikely(err != 0)) {
        busy = NS_FALSE;
        NsThreadFatal("Ns_RWLockWrLock", "pthread_rwlock_trywrlock", err);
    } else {
        busy = NS_FALSE;
    }

    if (busy) {
        err = pthread_rwlock_wrlock(&lockPtr->rwlock);
        if (err != 0) {
            NsThreadFatal("Ns_RWLockWrLock", "pthread_rwlock_wrlock", err);
        }
        lockPtr->nbusy ++;

#ifndef NS_NO_MUTEX_TIMING
        /*
         * Measure total and max waiting time for busy rwlock locks.
         */
        Ns_GetTime(&end);
        Ns_DiffTime(&end, &startTime, &diff);
        Ns_IncrTime(&lockPtr->total_waiting_time, diff.sec, diff.usec);
#endif
    }
#ifndef NS_NO_MUTEX_TIMING
    lockPtr->rw = NS_WRITE;
    lockPtr->start_time = startTime;
#endif
    lockPtr->nlock ++;
    lockPtr->nwlock++;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockUnlock --
 *
 *      Unlock a read/write lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Read or write condition may be signaled.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockUnlock(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr = (RwLock *) *rwPtr;
    int err;

#ifndef NS_NO_MUTEX_TIMING
    /*
     * Measure block times etc only in writer case, which guarantees exclusive
     * access and blocking).
     */
    if (lockPtr->rw == NS_WRITE) {
        Ns_Time end, diff;

        lockPtr->rw = NS_READ;
        Ns_GetTime(&end);
        Ns_DiffTime(&end, &lockPtr->start_time, &diff);
        Ns_IncrTime(&lockPtr->total_lock_time, diff.sec, diff.usec);
    }
#endif

    err = pthread_rwlock_unlock(&lockPtr->rwlock);
    if (err != 0) {
        NsThreadFatal("Ns_RWLockUnlock", "pthread_rwlock_unlock", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * GetRwLock --
 *
 *      Return the read/write lock structure, initializing it if needed.
 *
 * Results:
 *      Pointer to lock.
 *
 * Side effects:
 *      Lock may be initialized.
 *
 *----------------------------------------------------------------------
 */

static RwLock *
GetRwLock(Ns_RWLock *rwPtr, const char *caller)
{
    NS_NONNULL_ASSERT(rwPtr != NULL);

    if (*rwPtr == NULL) {
        fprintf(stderr, "%s: called with uninitialized lock pointer. "
                "This should not happen, call Ns_RWLockInit() before this call\n",
                caller);
        Ns_RWLockInit(rwPtr);
    }

#ifdef KEEP_DOUBLE_LOCK
    if (*rwPtr == NULL) {
        Ns_MasterLock();
        if (*rwPtr == NULL) {
            Ns_RWLockInit(rwPtr);
        }
        Ns_MasterUnlock();
    }
#else
    assert(*rwPtr != NULL);
#endif
    return (RwLock *) *rwPtr;
}

#else /* NOT HAVE_PTHREAD */

/*
 * The following structure defines a read/write lock including a mutex
 * to protect access to the structure and condition variables for waiting
 * reader and writer threads.
 */

typedef struct RwLock {
    Ns_Mutex  mutex;    /* Mutex guarding lock structure. */
    Ns_Cond   rcond;    /* Condition variable for waiting readers. */
    Ns_Cond   wcond;    /* condition variable for waiting writers. */
    int       nreaders; /* Number of readers waiting for lock. */
    int       nwriters; /* Number of writers waiting for lock. */
    int       lockcnt;  /* Lock count, > 0 indicates # of shared
                         * readers, -1 indicates exclusive writer. */
} RwLock;

static RwLock *GetRwLock(Ns_RWLock *rwPtr, const char *caller)
    NS_GNUC_NONNULL(1) NS_GNUC_RETURNS_NONNULL;


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockList --
 *
 *      Append info on each lock to Tcl_DString. Since the rwlock emulation is
 *      based on mutexes, the information on these locks is included in
 *      Ns_MutexList().
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
Ns_RWLockList(Tcl_DString *UNUSED(dsPtr))
{
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockInit --
 *
 *      Initialize a read/write lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Lock memory is allocated from the heap and initialized.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockInit(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr;
    static uintptr_t nextid = 0;

    NS_NONNULL_ASSERT(rwPtr != NULL);

    lockPtr = ns_calloc(1u, sizeof(RwLock));
    NsMutexInitNext(&lockPtr->mutex, "rw", &nextid);
    Ns_CondInit(&lockPtr->rcond);
    Ns_CondInit(&lockPtr->wcond);
    lockPtr->nreaders = 0;
    lockPtr->nwriters = 0;
    lockPtr->lockcnt = 0;
    *rwPtr = (Ns_RWLock) lockPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockSetName2  --
 *
 *      Update the string name of a mutex.  Ns_RWLockSetName2 uses
 *      Ns_MutexSetName2 to set a name based on the two string components and
 *      concatenates these with a colon (":").
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
Ns_RWLockSetName2(Ns_RWLock *rwPtr, const char *prefix, const char *name)
{
    RwLock *lockPtr = (RwLock *)*rwPtr;

    Ns_MutexSetName2(&lockPtr->mutex, prefix, name);

}



/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockDestroy --
 *
 *      Destroy a read/write lock if it was previously initialized.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Read/write lock objects are destroy and the lock memory is
 *      returned to the heap.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockDestroy(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr = (RwLock *) *rwPtr;

    if (lockPtr != NULL) {
        Ns_MutexDestroy(&lockPtr->mutex);
        Ns_CondDestroy(&lockPtr->rcond);
        Ns_CondDestroy(&lockPtr->wcond);
        ns_free(lockPtr);
        *rwPtr = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockRdLock --
 *
 *      Acquire a read lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread may wait on a condition variable if the read/write lock
 *      currently has a write lock.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockRdLock(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr;

    NS_NONNULL_ASSERT(rwPtr != NULL);

    lockPtr = GetRwLock(rwPtr, "Ns_RWLockRdLock");
    Ns_MutexLock(&lockPtr->mutex);

    /*
     * Wait on the read condition while the lock is write-locked or
     * some other thread is waiting for a write lock.
     */

    while (lockPtr->lockcnt < 0 || lockPtr->nwriters > 0) {
        lockPtr->nreaders++;
        Ns_CondWait(&lockPtr->rcond, &lockPtr->mutex);
        lockPtr->nreaders--;
    }

    lockPtr->lockcnt++;
    Ns_MutexUnlock(&lockPtr->mutex);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockWrLock --
 *
 *      Acquire a write lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread may wait on the write condition if other threads either
 *      have the lock read or write locked.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockWrLock(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr;

    NS_NONNULL_ASSERT(rwPtr != NULL);

    lockPtr = GetRwLock(rwPtr, "Ns_RWLockWrLock");

    Ns_MutexLock(&lockPtr->mutex);
    while (lockPtr->lockcnt != 0) {
        lockPtr->nwriters++;
        Ns_CondWait(&lockPtr->wcond, &lockPtr->mutex);
        lockPtr->nwriters--;
    }
    lockPtr->lockcnt = -1;
    Ns_MutexUnlock(&lockPtr->mutex);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RWLockUnlock --
 *
 *      Unlock a read/write lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Read or write condition may be signaled.
 *
 *----------------------------------------------------------------------
 */

void
Ns_RWLockUnlock(Ns_RWLock *rwPtr)
{
    RwLock *lockPtr;

    NS_NONNULL_ASSERT(rwPtr != NULL);
    lockPtr = (RwLock *) *rwPtr;

    Ns_MutexLock(&lockPtr->mutex);
    if (--lockPtr->lockcnt < 0) {
        lockPtr->lockcnt = 0;
    }
    if (lockPtr->nwriters != 0) {
        Ns_CondSignal(&lockPtr->wcond);
    } else if (lockPtr->nreaders != 0) {
        Ns_CondBroadcast(&lockPtr->rcond);
    }
    Ns_MutexUnlock (&lockPtr->mutex);
}


/*
 *----------------------------------------------------------------------
 *
 * GetRwLock --
 *
 *      Return the read/write lock structure, initializing it if needed.
 *
 * Results:
 *      Pointer to lock.
 *
 * Side effects:
 *      Lock may be initialized.
 *
 *----------------------------------------------------------------------
 */

static RwLock *
GetRwLock(Ns_RWLock *rwPtr, const char *caller)
{
    NS_NONNULL_ASSERT(rwPtr != NULL);

    if (*rwPtr == NULL) {
        fprintf(stderr, "%s: called with uninitialized lock pointer. "
                "This should not happen, call Ns_RWLockInit() before this call\n",
                caller);
        Ns_RWLockInit(rwPtr);
    }

#ifdef KEEP_DOUBLE_LOCK
    if (*rwPtr == NULL) {
        Ns_MasterLock();
        if (*rwPtr == NULL) {
            Ns_RWLockInit(rwPtr);
        }
        Ns_MasterUnlock();
    }
#else
    assert(*rwPtr != NULL);
#endif

    return (RwLock *) *rwPtr;
}

#endif /* HAVE_PTHREAD */
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
