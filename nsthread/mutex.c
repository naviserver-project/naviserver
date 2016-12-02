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
 * mutex.c --
 *
 *	Mutex locks with metering.
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


/*
 * The following structure defines a mutex with
 * string name and lock and busy counters.
 */

typedef struct Mutex {
    void	    *lock;
    struct Mutex    *nextPtr;
    uintptr_t        id;
    unsigned long    nlock;
    unsigned long    nbusy;
    Ns_Time          start_time;
    Ns_Time          total_waiting_time;
    Ns_Time          max_waiting_time;
    Ns_Time          total_lock_time;
    char	     name[NS_THREAD_NAMESIZE+1];
} Mutex;

#define GETMUTEX(mutex) (*(mutex) != NULL ? ((Mutex *)*(mutex)) : GetMutex((mutex)))
static Mutex *GetMutex(Ns_Mutex *mutex) NS_GNUC_NONNULL(1);
static Mutex *firstMutexPtr;

bool NS_mutexlocktrace = NS_FALSE;


/*
 *----------------------------------------------------------------------
 
 * Ns_MutexInit --
 *
 *	Mutex initialization, often called the first time a mutex
 *	is locked.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_MutexInit(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;
    static uintptr_t nextid = 0u;

    NS_NONNULL_ASSERT(mutex != NULL);

    //fprintf(stderr, "=== Ns_MutexInit *mutex = %p current id %ld\n", (void*)*mutex, nextid);
    //assert(*mutex == NULL);
    
    mutexPtr = ns_calloc(1u, sizeof(Mutex));
    mutexPtr->lock = NsLockAlloc();
    Ns_MasterLock();
    mutexPtr->nextPtr = firstMutexPtr;
    firstMutexPtr = mutexPtr;
    mutexPtr->id = nextid++;
    snprintf(mutexPtr->name, sizeof(mutexPtr->name), "mu%" PRIuPTR, mutexPtr->id);
    Ns_MasterUnlock();
    //fprintf(stderr, "=== created mutex %ld name %s\n", mutexPtr->id, mutexPtr->name);
#if 0
    /* 
     *   221 tclenv.c:113 
     *   CreateSynchObject: 209, 210, 212
     */
    // 141 145 206 209 230
    // 210 211 213 
    if (0 && mutexPtr->id == 13) {
        char *p = NULL;
        *p = 1;
    }
#endif    
    *mutex = (Ns_Mutex) mutexPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexSetName, Ns_MutexSetName2 --
 *
 *	Update the string name of a mutex.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
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
    Mutex *mutexPtr;
    size_t prefixLength, nameLength;
    char *p;

    NS_NONNULL_ASSERT(mutex != NULL);
    NS_NONNULL_ASSERT(prefix != NULL);

    mutexPtr = GETMUTEX(mutex);
    prefixLength = strlen(prefix);
    if (prefixLength > NS_THREAD_NAMESIZE - 1) {
	prefixLength = NS_THREAD_NAMESIZE - 1;
	nameLength = 0u;
    } else if (name != NULL) {
	nameLength = strlen(name);
	if ((nameLength + prefixLength + 1) > NS_THREAD_NAMESIZE) {
	    nameLength = NS_THREAD_NAMESIZE - prefixLength - 1;
	}
    }

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
 *	Mutex destroy.  Note this routine is not used very often
 *	as mutexes normally exists in memory until the process exits.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_MutexDestroy(Ns_Mutex *mutex)
{
    Mutex	 *mutexPtr = (Mutex *) *mutex;

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
 *	Lock a mutex, tracking the number of locks and the number of
 *	which were not acquired immediately.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Thread may be suspended if the lock is held.
 *
 *----------------------------------------------------------------------
 */

void
Ns_MutexLock(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;
#ifndef NS_NO_MUTEX_TIMING
    Ns_Time end, diff, startTime;

    Ns_GetTime(&startTime);
#endif

    NS_NONNULL_ASSERT(mutex != NULL);
    
    mutexPtr = GETMUTEX(mutex);
    if (unlikely(!NsLockTry(mutexPtr->lock))) {
	NsLockSet(mutexPtr->lock);
	++mutexPtr->nbusy;

#ifndef NS_NO_MUTEX_TIMING
        /*
         * Measure total and max waiting time for busy mutex locks.
         */
	Ns_GetTime(&end);
        Ns_DiffTime(&end, &startTime, &diff);
	Ns_IncrTime(&mutexPtr->total_waiting_time, diff.sec, diff.usec);

	if (NS_mutexlocktrace && (diff.sec > 1 || diff.usec > 100000)) {
	    fprintf(stderr, "[%lx] Mutex lock %s: wait duration %" PRIu64 ".%06ld\n",
                    (long)(void*)pthread_self(), mutexPtr->name, (int64_t)diff.sec, diff.usec);
	}

        /* 
         * Keep max waiting time since server start. It might be a
	 * good idea to either provide a call to reset the max-time,
	 * or to report wait times above a certain threshold (as an
	 * extra value in the statistics, or in the log file).
         */
        if (Ns_DiffTime(&mutexPtr->max_waiting_time, &diff, NULL) < 0) {
            mutexPtr->max_waiting_time = diff;
            /*fprintf(stderr, "Mutex %s max time %" PRIu64 ".%06ld\n", 
	      mutexPtr->name, (int64_t)diff.sec, diff.usec);*/
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
 *	Attempt to lock a mutex.
 *
 * Results:
 *	NS_OK if locked, NS_TIMEOUT if lock already held.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */

int
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
 *	Unlock a mutex.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Other waiting thread, if any, is resumed.
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
        fprintf(stderr, "[%lx] Mutex unlock %s: lock duration %" PRIu64 ".%06ld\n",
                (long)(void*)pthread_self(), mutexPtr->name, (int64_t)diff.sec, diff.usec);
    }

}


/*
 *----------------------------------------------------------------------
 *
 * Ns_MutexList --
 *
 *	Append info on each lock.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
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
                 " %" PRIuPTR " %lu %lu %" PRIu64 ".%06ld %" PRIu64 ".%06ld %" PRIu64 ".%06ld", 
                 mutexPtr->id, mutexPtr->nlock, mutexPtr->nbusy, 
                 (int64_t)mutexPtr->total_waiting_time.sec, mutexPtr->total_waiting_time.usec,
                 (int64_t)mutexPtr->max_waiting_time.sec, mutexPtr->max_waiting_time.usec,
                 (int64_t)mutexPtr->total_lock_time.sec, mutexPtr->total_lock_time.usec
		 );
        Tcl_DStringAppend(dsPtr, buf, -1);
        Tcl_DStringEndSublist(dsPtr);
    }
    Ns_MasterUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * NsMutexInitNext --
 *
 *	Initialize and name the next internal mutex.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Given counter is updated.
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
 *	Return the private lock pointer for a Ns_Mutex.
 *
 * Results:
 *	Pointer to lock.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void *
NsGetLock(Ns_Mutex *mutex)
{
    Mutex *mutexPtr;

    NS_NONNULL_ASSERT(mutex != NULL);
    
    mutexPtr = GETMUTEX(mutex);
    return mutexPtr->lock;
}


/*
 *----------------------------------------------------------------------
 *
 * GetMutex --
 *
 *	Cast an Ns_Mutex to a Mutex, initializing if needed.
 *
 * Results:
 *	Pointer to Mutex.
 *
 * Side effects:
 *	Mutex is initialized the first time.
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
 *	Obtain the name of a mutex.
 *
 * Results:
 *	String name of the mutex.
 *
 * Side effects:
 *	None.
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
