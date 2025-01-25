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

#ifndef _WIN32
#include <nsconfig.h>
#endif

#ifdef HAVE_PTHREAD

/*
 * pthread.c --
 *
 *      Interface routines for nsthreads using pthreads.
 *
 */

#include "thread.h"
#include <pthread.h>

/*
 * Local functions defined in this file.
 */

static pthread_cond_t *GetCond(Ns_Cond *cond, const char *caller)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_RETURNS_NONNULL;
static void CleanupTls(void *arg)
    NS_GNUC_NONNULL(1);
static void *ThreadMain(void *arg);

/*
 * Solaris has weird way to declare this one so
 * we just make a shortcut because this is what
 * the (Solaris) definition really does.
 */

#if defined(__sun__)
# define PTHREAD_STACK_MIN ((size_t)sysconf(_SC_THREAD_STACK_MIN))
#endif

#ifndef PTHREAD_STACK_MIN
/*
 * Starting with glibc Version 2.34, PTHREAD_STACK_MIN is no longer constant
 * and is redefined to sysconf(_SC_THREAD_STACK_MIN).  This supports dynamic
 * sized register sets for modern architectural features like Arm SVE.
 */
# define PTHREAD_STACK_MIN sysconf(_SC_THREAD_STACK_MIN)
#endif

/*
 * The following single TLS key is used to store the nsthread
 * TLS slots. Due to system limitation(s), we stuff all of the
 * slots into a private array keyed onto this per-thread key,
 * instead of using separate TLS keys for each consumer.
 */

static pthread_key_t key;


/*
 *----------------------------------------------------------------------
 *
 * Nsthreads_LibInit --
 *
 *      Pthread library initialization routine.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Creates pthread key.
 *
 *----------------------------------------------------------------------
 */

void
Nsthreads_LibInit(void)
{
    static bool initialized = NS_FALSE;

    if (!initialized) {
        int err;

        initialized = NS_TRUE;
#ifdef __linux
        {
            size_t n;

            n = confstr(_CS_GNU_LIBPTHREAD_VERSION, NULL, 0);
            if (n > 0) {
                char *buf = ns_malloc(n);

                confstr(_CS_GNU_LIBPTHREAD_VERSION, buf, n);
                if (!strstr (buf, "NPTL")) {
                    Tcl_Panic("Linux \"NPTL\" thread library required. Found: \"%s\"", buf);
                }
                ns_free(buf);
            }
        }
#endif
        err = pthread_key_create(&key, CleanupTls);
        if (err != 0) {
            NsThreadFatal("Nsthreads_LibInit", "pthread_key_create", err);
        }
        NsInitThreads();
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetTls --
 *
 *      Return the TLS slots.
 *
 * Results:
 *      Pointer to slots array.
 *
 * Side effects:
 *      Storage for the slot array is allocated bypassing the
 *      currently configured memory allocator because at the
 *      time this storage is to be reclaimed (see: CleanupTls)
 *      the allocator may already be finalized for this thread.
 *
 *----------------------------------------------------------------------
 */

void **
NsGetTls(void)
{
    void **slots;

    slots = pthread_getspecific(key);
    if (slots == NULL) {
        slots = calloc(NS_THREAD_MAXTLS, sizeof(void *));
        if (slots == NULL) {
            fprintf(stderr, "Fatal: NsGetTls failed to allocate %" PRIuz " bytes.\n",
                    NS_THREAD_MAXTLS * sizeof(void *));
            abort();
        }
        pthread_setspecific(key, slots);
    }
    return slots;
}


/*
 *----------------------------------------------------------------------
 *
 * NsThreadLibName --
 *
 *      Return the string name of the thread library.
 *
 * Results:
 *      Pointer to static string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
NsThreadLibName(void)
{
    return "pthread";
}


/*
 *----------------------------------------------------------------------
 *
 * NsLockAlloc --
 *
 *      Allocate and initialize a mutex lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
NsLockAlloc(void)
{
    pthread_mutex_t *lock;
    int err;

    lock = ns_malloc(sizeof(pthread_mutex_t));
    err = pthread_mutex_init(lock, NULL);
    if (err != 0) {
        NsThreadFatal("NsLockAlloc", "pthread_mutex_init", err);
    }
    return lock;
}


/*
 *----------------------------------------------------------------------
 *
 * NsLockFree --
 *
 *      Free a mutex lock.
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
NsLockFree(void *lock)
{
    int err;

    NS_NONNULL_ASSERT(lock != NULL);

    err = pthread_mutex_destroy((pthread_mutex_t *) lock);
    if (err != 0) {
        NsThreadFatal("NsLockFree", "pthread_mutex_destroy", err);
    }
    ns_free(lock);
}


/*
 *----------------------------------------------------------------------
 *
 * NsLockSet --
 *
 *      Set a mutex lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May wait wakeup event if lock already held.
 *
 *----------------------------------------------------------------------
 */

void
NsLockSet(void *lock)
{
    int err;

    NS_NONNULL_ASSERT(lock != NULL);

    err = pthread_mutex_lock((pthread_mutex_t *) lock);
    if (err != 0) {
        NsThreadFatal("NsLockSet", "pthread_mutex_lock", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsLockTry --
 *
 *      Try to set a mutex lock once.
 *
 * Results:
 *      NS_TRUE if lock set, NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
NsLockTry(void *lock)
{
    int err;

    NS_NONNULL_ASSERT(lock != NULL);

    err = pthread_mutex_trylock((pthread_mutex_t *) lock);
    if (unlikely(err == EBUSY)) {
        return NS_FALSE;
    }
    if (unlikely(err != 0)) {
        NsThreadFatal("NsLockTry", "pthread_mutex_trylock", err);
    }

    return NS_TRUE;
}


/*
 *----------------------------------------------------------------------
 *
 * NsLockUnset --
 *
 *      Unset a mutex lock.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      May signal wakeup event for a waiting thread.
 *
 *----------------------------------------------------------------------
 */

void
NsLockUnset(void *lock)
{
    int err;

    NS_NONNULL_ASSERT(lock != NULL);

    err = pthread_mutex_unlock((pthread_mutex_t *) lock);
    if (unlikely(err != 0)) {
        NsThreadFatal("NsLockUnset", "pthread_mutex_unlock", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsCreateThread --
 *
 *      Pthread specific thread create function called by
 *      Ns_ThreadCreate.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on thread startup routine.
 *
 *----------------------------------------------------------------------
 */

void
NsCreateThread(void *arg, ssize_t stacksize, Ns_Thread *threadPtr)
{
    static const char *func = "NsCreateThread";
    pthread_attr_t     attr;
    pthread_t          thr;
    int                err;

    err = pthread_attr_init(&attr);
    if (err != 0) {
        NsThreadFatal(func, "pthread_attr_init", err);
    }

    /*
     * Set the stack size if specified explicitly.  It is smarter
     * to leave the default on platforms which map large stacks
     * with guard zones (e.g., Solaris and Linux).
     */

    if (stacksize > 0) {
        if (stacksize < PTHREAD_STACK_MIN) {
            stacksize = PTHREAD_STACK_MIN;
        } else {
          /*
           * The stack-size has to be a multiple of the page-size,
           * otherwise pthread_attr_setstacksize fails. When we have
           * _SC_PAGESIZE defined, try to be friendly and round the
           * stack-size to the next multiple of the page-size.
           */
#if defined(_SC_PAGESIZE)
            long pageSize = sysconf(_SC_PAGESIZE);
            stacksize = (((stacksize-1) / pageSize) + 1) * pageSize;
#endif
        }
        err = pthread_attr_setstacksize(&attr, (size_t) stacksize);
        if (err != 0) {
            NsThreadFatal(func, "pthread_attr_setstacksize", err);
        }
    }

    /*
     * System scope always preferred, ignore any unsupported error.
     */
    err = pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    if (err != 0 && err != ENOTSUP) {
        NsThreadFatal(func, "pthread_setscope", err);
    }

    /*
     * In case, there is no threadPtr given, create a detached thread.
     */
    if (threadPtr == NULL) {
        err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (err != 0 && err != ENOTSUP) {
            NsThreadFatal(func, "pthread_setdetachstate", err);
        }
    }

    /*
     * Create the work horse thread
     */
    err = pthread_create(&thr, &attr, ThreadMain, arg);
    if (err != 0) {
        NsThreadFatal(func, "pthread_create", err);
    } else if (threadPtr != NULL) {
        *threadPtr = (Ns_Thread)(uintptr_t) thr;
    }

    /*
     *
     */
    err = pthread_attr_destroy(&attr);
    if (err != 0) {
        NsThreadFatal(func, "pthread_attr_destroy", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsThreadExit --
 *
 *      Terminate a thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Thread will clean itself up via the TLS cleanup code.
 *
 *----------------------------------------------------------------------
 */

void
NsThreadExit(void *arg)
{
   /*
    * Exit the thread really. This will invoke all of the
    * registered TLS cleanup callbacks again (no harm).
    */

    pthread_exit(arg);
}

/*
 *----------------------------------------------------------------------
 *
 * NsThreadResult --
 *
 *      Stub function, which is not necessary when pthreads are used, since
 *      pthread_exit passes a pointer values). However, the situation for
 *      windows is different, and we keep this function here for symmetry with
 *      the version using windows native threads. For background, see
 *      winthread.c.
 *
 * Results:
 *      Pointer value (can be NULL).
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
NsThreadResult(void *arg)
{
    return arg;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadJoin --
 *
 *      Wait for exit of a non-detached thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Requested thread is destroyed after join.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadJoin(Ns_Thread *thread, void **argPtr)
{
    int err;

    NS_NONNULL_ASSERT(thread != NULL);

    err = pthread_join((pthread_t)(uintptr_t)*thread, argPtr);
    if (err != 0) {
        NsThreadFatal("Ns_ThreadJoin", "pthread_join", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadYield --
 *
 *      Yield the CPU to another thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See sched_yield().
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadYield(void)
{
    sched_yield();
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadId --
 *
 *      Return the numeric thread id.
 *
 * Results:
 *      Integer thread id.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

uintptr_t
Ns_ThreadId(void)
{
    pthread_t result = pthread_self();

    return (uintptr_t) result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadSelf --
 *
 *      Return thread handle suitable for Ns_ThreadJoin.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Value at threadPtr is updated with thread's handle.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadSelf(Ns_Thread *threadPtr)
{
    pthread_t result = pthread_self();

    NS_NONNULL_ASSERT(threadPtr != NULL);

    *threadPtr = (Ns_Thread)result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CondInit --
 *
 *      Pthread condition variable initialization.  Note this routine
 *      isn't used directly very often as static condition variables
 *      are now self-initialized when first used.
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
Ns_CondInit(Ns_Cond *cond)
{
    pthread_cond_t *condPtr;
    int             err;

    NS_NONNULL_ASSERT(cond != NULL);

    condPtr = ns_malloc(sizeof(pthread_cond_t));
    err = pthread_cond_init(condPtr, NULL);
    if (err != 0) {
        NsThreadFatal("Ns_CondInit", "pthread_cond_init", err);
    }
    *cond = (Ns_Cond) condPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CondDestroy --
 *
 *      Pthread condition destroy.  Note this routine is almost never
 *      used as condition variables normally exist in memory until
 *      the process exits.
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
Ns_CondDestroy(Ns_Cond *cond)
{
    pthread_cond_t *condPtr = (pthread_cond_t *) *cond;

    if (condPtr != NULL) {
        int err;

        err = pthread_cond_destroy(condPtr);
        if (err != 0) {
            NsThreadFatal("Ns_CondDestroy", "pthread_cond_destroy", err);
        }
        ns_free(condPtr);
        *cond = NULL;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CondSignal --
 *
 *      Pthread condition signal.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See pthread_cond_signal.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CondSignal(Ns_Cond *cond)
{
    int             err;

    NS_NONNULL_ASSERT(cond != NULL);

    err = pthread_cond_signal(GetCond(cond, "Ns_CondSignal"));
    if (err != 0) {
        NsThreadFatal("Ns_CondSignal", "pthread_cond_signal", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CondBroadcast --
 *
 *      Pthread condition broadcast.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See pthread_cond_broadcast.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CondBroadcast(Ns_Cond *cond)
{
    int err;

    NS_NONNULL_ASSERT(cond != NULL);

    err = pthread_cond_broadcast(GetCond(cond, "Ns_CondBroadcast"));
    if (err != 0) {
        NsThreadFatal("Ns_CondBroadcast", "pthread_cond_broadcast", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CondWait --
 *
 *      Pthread indefinite condition wait.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See pthread_cond_wait.
 *
 *----------------------------------------------------------------------
 */

void
Ns_CondWait(Ns_Cond *cond, Ns_Mutex *mutex)
{
    int err;

    NS_NONNULL_ASSERT(cond != NULL);
    NS_NONNULL_ASSERT(mutex != NULL);

    err = pthread_cond_wait(GetCond(cond, "Ns_CondWait"), NsGetLock(mutex));
    if (err != 0) {
        NsThreadFatal("Ns_CondWait", "pthread_cond_wait", err);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CondTimedWait --
 *
 *      Pthread absolute time wait.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      See pthread_cond_timewait.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_CondTimedWait(Ns_Cond *cond, Ns_Mutex *mutex, const Ns_Time *timePtr)
{
    int              err;
    Ns_ReturnCode    status;
    struct timespec  ts;

    NS_NONNULL_ASSERT(cond != NULL);
    NS_NONNULL_ASSERT(mutex != NULL);

    if (timePtr == NULL) {
        Ns_CondWait(cond, mutex);
        return NS_OK;
    }

    /*
     * Convert the microsecond-based Ns_Time to a nanosecond-based
     * struct timespec.
     */

    ts.tv_sec = timePtr->sec;
    ts.tv_nsec = timePtr->usec * 1000;

    /*
     * As documented on Linux, pthread_cond_timedwait may return
     * NS_EINTR if a signal arrives.  We have noticed that
     * NS_EINTR can be returned on Solaris as well although this
     * is not documented.  We assume the wakeup is truly
     * spurious and simply restart the wait knowing that the
     * ts structure has not been modified.
     */

    do {
        err = pthread_cond_timedwait(GetCond(cond, "Ns_CondTimedWait"), NsGetLock(mutex), &ts);
    } while (err == NS_EINTR);
    if (err == ETIMEDOUT) {
        status = NS_TIMEOUT;
    } else if (err != 0) {
        fprintf(stderr, "Ns_CondTimedWait: timestamp " NS_TIME_FMT " secs %ld nanoseconds %ld\n",
                (int64_t)timePtr->sec, timePtr->usec,
                ts.tv_sec, ts.tv_nsec
                );
        NsThreadFatal("Ns_CondTimedWait", "pthread_cond_timedwait", err);
#ifdef NS_TCL_PRE86
        status = NS_ERROR;
#endif
    } else {
        status = NS_OK;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * GetCond --
 *
 *      Cast an Ns_Cond to pthread_cond_t, initializing if needed.
 *
 * Results:
 *      Pointer to pthread_cond_t.
 *
 * Side effects:
 *      Ns_Cond is initialized the first time.
 *
 *----------------------------------------------------------------------
 */

static pthread_cond_t *
GetCond(Ns_Cond *cond, const char *caller)
{
    NS_NONNULL_ASSERT(cond != NULL);

    if (*cond == NULL) {
        fprintf(stderr, "%s: called with uninitialized condition pointer. "
                "This should not happen, call Ns_CondInit() before this call\n",
                caller);
        Ns_MasterLock();
        if (*cond == NULL) {
            Ns_CondInit(cond);
        }
        Ns_MasterUnlock();
    }
    return (pthread_cond_t *) *cond;
}


/*
 *----------------------------------------------------------------------
 *
 * ThreadMain --
 *
 *      Pthread startup routine.
 *
 * Results:
 *      Does not return.
 *
 * Side effects:
 *      NsThreadMain will call Ns_ThreadExit.
 *
 *----------------------------------------------------------------------
 */

static void *
ThreadMain(void *arg)
{
    NsThreadMain(arg);
    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * CleanupTls --
 *
 *      Pthread TLS cleanup.  This routine is called during thread
 *      exit.  This routine could be called more than once if some
 *      other pthread cleanup requires nsthread's TLS.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Storage for the TLS slot array is reclaimed bypassing the
 *      current memory allocator. It is because at this point,
 *      the allocator may already be finalized for this thread.
 *
 *----------------------------------------------------------------------
 */

static void
CleanupTls(void *arg)
{
    void **slots = arg;
    Ns_Thread thread = NULL;

    NS_NONNULL_ASSERT(arg != NULL);

    /*
     * Restore the current slots during cleanup so handlers can access
     * TLS in other slots.
     */

    pthread_setspecific(key, arg);
    Ns_ThreadSelf(&thread);
    NsCleanupTls(slots);
    pthread_setspecific(key, NULL);
    free(slots);
}
#else
# ifndef _WIN32
#  error "pthread support is required"
# endif
#endif


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
