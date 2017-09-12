/*
 * The contents of this file are subject to the Mozilla  Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is AOLserver Code and related documentation distributed by
 * AOL.
 *
 * The Initial Developer of the Original Code is America Online, Inc. Portions
 * created by AOL are Copyright (C) 1999 America Online, Inc. All Rights
 * Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * GNU General Public License (the "GPL"), in which case the provisions of
 * GPL are applicable instead of those above.  If you wish to allow use of
 * your version of this file only under the terms of the GPL and not to allow
 * others to use your version of this file under the License, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL. If you do not delete the
 * provisions above, a recipient may use your version of this file under
 * either the License or the GPL.
 */

/*
 * test.c -
 *
 *	Collection of thread interface tests.  This code is somewhat sloppy
 *	but contains several examples of of using conditions, mutexes,
 *	thread local storage, and creating/joining threads.
 */

#include "nsthread.h"

/*
 * Special direct include of pthread.h for compatibility tests.
 */

#ifdef _WIN32
#define PTHREAD_TEST 0
#else
#include <pthread.h>
#define PTHREAD_TEST 1
#endif

/*
 * Collection of synchronization objects for tests.
 */

static Ns_Mutex block = NULL;
static Ns_Mutex slock = NULL;
static Ns_Mutex lock  = NULL;
static Ns_Cond  cond  = NULL;
static Ns_Tls   key;
static Ns_RWLock rwlock;
static Ns_Sema  sema;
static Ns_Cs    cs;
static Ns_Mutex dlock = NULL;
static Ns_Cond  dcond = NULL;
static int      dstop = 0;

/*
 * Local Prototypes
 */
static void AtExit(void);
static void MemThread(void *arg);

static void Msg(const char *fmt,...)
    NS_GNUC_PRINTF(1, 2);

/*
 * Msg -
 *
 *	Simple message logger with thread id and name.
 */

static void
Msg(const char *fmt,...)
{
    va_list         ap;
    char           *s, *r;
    time_t          now;

    time(&now);
    s = ns_ctime(&now);
    r = strchr(s, INTCHAR('\n'));
    if (r != NULL) {
	*r = '\0';
    }
    va_start(ap, fmt);
    Ns_MutexLock(&slock);
    printf("[%s][%s]: ", Ns_ThreadGetName(), s);
    vfprintf(stdout, fmt, ap);
    printf("\n");
    Ns_MutexUnlock(&slock);
    va_end(ap);
}

/*
 * TlsLogArg -
 *
 *	Log and then free TLS slot data at thread exit.
 */

static void
TlsLogArg(void *arg)
{
    int            *ip = arg;

    Msg("tls cleanup %d", *ip);
    ns_free(ip);
}

/*
 * RecursiveStackCheck, CheckStackThread -
 *
 *	Thread which recursively probes stack for max depth.
 */

static uintptr_t
RecursiveStackCheck(uintptr_t n)
{
#if 0
    if (Ns_CheckStack() == NS_OK) {
        n = RecursiveStackCheck(n);
    }
#endif
    ++n;

    return n;
}

static void
CheckStackThread(void *UNUSED(arg))
{
    uintptr_t n;

    n = RecursiveStackCheck(0);
    Ns_ThreadExit((void *) n);
}

/*
 * WorkThread -
 *
 *	Thread which exercies a variety of sync objects and TLS.
 */

static void
WorkThread(void *arg)
{
    intptr_t        i = (intptr_t) arg;
    intptr_t       *ip;
    time_t          now;
    Ns_Thread       self;
    char            name[32];

    sprintf(name, "-work:%" PRIdPTR "-", i);
    Ns_ThreadSetName(name);

    if (i == 2) {
        Ns_RWLockWrLock(&rwlock);
        Msg("rwlock write acquired");
        sleep(2);
    } else {
        Ns_RWLockRdLock(&rwlock);
        Msg("rwlock read acquired");
        sleep(1);
    }
    Ns_CsEnter(&cs);
    Msg("enter critical section once");
    Ns_CsEnter(&cs);
    Msg("enter critical section twice");
    Ns_CsLeave(&cs);
    Ns_CsLeave(&cs);
    Ns_ThreadSelf(&self);
    arg = Ns_TlsGet(&key);
    Ns_SemaWait(&sema);
    Msg("got semaphore posted from main");
    if (arg == NULL) {
        arg = ns_malloc(sizeof(i));
        Ns_TlsSet(&key, arg);
    }
    ip = arg;
    *ip = i;

    if (i == 5) {
        Ns_Time         to;
        Ns_ReturnCode   st;

        Ns_GetTime(&to);
        Msg("time: %" PRIu64 ".%06ld", (int64_t) to.sec, to.usec);
        Ns_IncrTime(&to, 5, 0);
        Msg("time: %" PRIu64 ".%06ld", (int64_t) to.sec, to.usec);
        Ns_MutexLock(&lock);
        time(&now);
        Msg("timed wait starts: %s", ns_ctime(&now));
        st = Ns_CondTimedWait(&cond, &lock, &to);
        Ns_MutexUnlock(&lock);
        time(&now);
        Msg("timed wait ends: %s - status: %d", ns_ctime(&now), st);
    }
    if (i == 9) {
        Msg("sleep 4 seconds start");
        sleep(4);
        Msg("sleep 4 seconds done");
    }
    time(&now);
    Ns_RWLockUnlock(&rwlock);
    Msg("rwlock unlocked");
    Msg("exiting");
    Ns_ThreadExit((void *) i);
}

/*
 * AtExit -
 *
 *	Test of atexit() handler.
 */

static void
AtExit(void)
{
    Msg("atexit handler called!");
}

/*
 * MemThread, MemTime -
 *
 *	Time allocations of malloc and MT-optmized ns_malloc
 */

#define NA 10000
#define BS 1024*16

int nthreads = 10;
int memstart;
int nrunning;

static void
MemThread(void *arg)
{
    int    i;
    void  *ptr;

    Ns_MutexLock(&lock);
    ++nrunning;
    Ns_CondBroadcast(&cond);
    while (memstart == 0) {
	Ns_CondWait(&cond, &lock);
    }
    Ns_MutexUnlock(&lock);

    ptr = NULL;
    for (i = 0; i < NA; ++i) {
	size_t n = (size_t)rand() % BS;
	if (arg != NULL) {
	    if (ptr != NULL) {
		ns_free(ptr);
            }
	    ptr = ns_malloc(n);
	} else {
	    if (ptr != NULL) {
		free(ptr);
            }
	    ptr = malloc(n);
	}
    }
}

static void
MemTime(int ns)
{
    Ns_Time         start, end, diff;
    int             i;
    Ns_Thread      *tids;

    tids = ns_malloc(sizeof(Ns_Thread *) * (size_t)nthreads);
    Ns_MutexLock(&lock);
    nrunning = 0;
    memstart = 0;
    Ns_MutexUnlock(&lock);
    printf("starting %d %smalloc threads...", nthreads, (ns != 0) ? "ns_" : "");
    fflush(stdout);
    for (i = 0; i < nthreads; ++i) {
        Ns_ThreadCreate(MemThread, (void *)(intptr_t) ns, 0, &tids[i]);
    }
    Ns_MutexLock(&lock);
    while (nrunning < nthreads) {
	Ns_CondWait(&cond, &lock);
    }
    printf("waiting....");
    fflush(stdout);
    memstart = 1;
    Ns_CondBroadcast(&cond);
    Ns_GetTime(&start);
    Ns_MutexUnlock(&lock);
    for (i = 0; i < nthreads; ++i) {
	Ns_ThreadJoin(&tids[i], NULL);
    }
    Ns_GetTime(&end);
    Ns_DiffTime(&end, &start, &diff);
    printf("done:  %" PRIu64 "%06ld sec\n", (int64_t) diff.sec, diff.usec);
}


static void
DumpString(Tcl_DString *dsPtr)
{
    char **largv;
    int largc;

    if (Tcl_SplitList(NULL, dsPtr->string, &largc, (CONST char***)&largv) == TCL_OK) {
        int i;

	for (i = 0; i < largc; ++i) {
	    printf("\t%s\n", largv[i]);
	}
	Tcl_Free((char *) largv);
    }
    Tcl_DStringSetLength(dsPtr, 0);
}


static void
DumperThread(void *UNUSED(arg))
{
    Ns_Time         to;
    Tcl_DString     ds;

    Tcl_DStringInit(&ds);
    Ns_ThreadSetName("-dumper-");
    Ns_MutexLock(&block);
    Ns_MutexLock(&dlock);
    while (dstop == 0) {
	Ns_GetTime(&to);
	Ns_IncrTime(&to, 1, 0);
	(void)Ns_CondTimedWait(&dcond, &dlock, &to);
	Ns_MutexLock(&slock);
	Ns_ThreadList(&ds, NULL);
	DumpString(&ds);
	Ns_MutexList(&ds);
	DumpString(&ds);
#ifdef HAVE_TCL_GETMEMORYINFO
	Tcl_GetMemoryInfo(&ds);
#endif
	DumpString(&ds);
	Ns_MutexUnlock(&slock);
    }
    Ns_MutexUnlock(&dlock);
    Ns_MutexUnlock(&block);
}

#if PTHREAD_TEST

/*
 * Routines to test compatibility with pthread-created
 * threads, i.e., that non-Ns_ThreadCreate'd threads
 * can call Ns API's which will cleanup at thread exit.
 */

static Ns_Mutex plock = NULL;
static Ns_Cond pcond  = NULL;
static int pgo = 0;

static void
PthreadTlsCleanup(void *arg)
{
    intptr_t i = (intptr_t) arg;
    printf("pthread[%" PRIxPTR "]: log: %" PRIdPTR"\n", (uintptr_t) pthread_self(), i);
}

static void *
Pthread(void *arg)
{
    static Ns_Tls tls;

    /*
     * Allocate TLS first time (this is recommended TLS
     * self-initialization style.
     */

    if (tls == NULL) {
	Ns_MasterLock();
	if (tls == NULL) {
	     Ns_TlsAlloc(&tls, PthreadTlsCleanup);
	}
	Ns_MasterUnlock();
    }

    Ns_TlsSet(&tls, arg);

    /*
     * Wait for exit signal from main().
     */

    Ns_MutexLock(&plock);
    while (pgo == 0) {
	Ns_CondWait(&pcond, &plock);
    }
    Ns_MutexUnlock(&plock);
    return arg;
}

#endif

/*
 * main -
 *
 *	Fire off a bunch of weird threads to exercise the thread
 *	interface.
 */

int main(int argc, char *argv[])
{
    intptr_t        i;
    Ns_Thread       threads[10];
    Ns_Thread       self, dumper;
    void *arg;
    char *p;
#if PTHREAD_TEST
    pthread_t tids[10];
#endif

    Tcl_FindExecutable(argv[0]);
    Nsthreads_LibInit();

    Ns_ThreadSetName("-main-");

    /*
     * Jump directly to memory test if requested.
     */

    for (i = 1; i < argc; ++i) {
	p = argv[i];
	switch (*p) {
	    case 'n':
		break;
	    case 'm':
	    	nthreads = (int)strtol(p + 1, NULL, 10);
		goto mem;
		break;
	}
    }

    Ns_ThreadCreate(DumperThread, NULL, 0, &dumper);
    Ns_MutexSetName(&lock, "startlock");
    Ns_MutexSetName(&dlock, "dumplock");
    Ns_MutexSetName(&slock, "msglock");
    Ns_MutexSetName(&block, "busylock");
    Ns_ThreadStackSize(81920);
    Ns_SemaInit(&sema, 3);
    Msg("sema initialized to 3");
    atexit(AtExit);
    Msg("pid = %d", getpid());
    Ns_TlsAlloc(&key, TlsLogArg);
    for (i = 0; i < 10; ++i) {
        Msg("starting work thread %" PRIdPTR, i);
        Ns_ThreadCreate(WorkThread, (void *) i, 0, &threads[i]);
    }
    sleep(1);
    /* Ns_CondSignal(&cond); */
    Ns_SemaPost(&sema, 10);
    Msg("sema post 10");
    Ns_RWLockWrLock(&rwlock);
    Msg("rwlock write locked (main thread)");
    sleep(1);
    Ns_RWLockUnlock(&rwlock);
    Msg("rwlock write unlocked (main thread)");
    for (i = 0; i < 10; ++i) {
        void *codeArg;
        Msg("waiting for thread %" PRIdPTR " to exit", i);
        Ns_ThreadJoin(&threads[i], &codeArg);
        Msg("thread %" PRIdPTR " exited - code: %" PRIuPTR, i, (uintptr_t) codeArg);
    }
#if PTHREAD_TEST
    for (i = 0; i < 10; ++i) {
        pthread_create(&tids[i], NULL, Pthread, (void *) i);
        printf("pthread: create %" PRIdPTR " = %" PRIxPTR "\n", i, (uintptr_t) tids[i]);
        Ns_ThreadYield();
    }
    Ns_MutexLock(&plock);
    pgo = 1;
    Ns_MutexUnlock(&plock);
    Ns_CondBroadcast(&pcond);
    for (i = 0; i < 10; ++i) {
        pthread_join(tids[i], &arg);
        printf("pthread: join %" PRIdPTR " = %" PRIdPTR "\n", i, (intptr_t) arg);
    }
#endif
    Ns_ThreadSelf(&self);
    Ns_MutexLock(&dlock);
    dstop = 1;
    Ns_CondSignal(&dcond);
    Ns_MutexUnlock(&dlock);
    Ns_ThreadJoin(&dumper, NULL);
    Msg("threads joined");
    {
        int j;
        for (j = 0; j < 10; ++j) {
            Ns_ThreadCreate(CheckStackThread, NULL, 8192*(j+1), &threads[j]);
        }
        for (j = 0; j < 10; ++j) {
            Ns_ThreadJoin(&threads[j], &arg);
            printf("check stack %d = %" PRIdPTR "\n", j, (intptr_t) arg);
        }
    }
    /*Ns_ThreadEnum(DumpThreads, NULL);*/
    /*Ns_MutexEnum(DumpLocks, NULL);*/
mem:
    MemTime(0);
    MemTime(1);
    return 0;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
