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
 * thread.c --
 *
 *      Routines for creating, exiting, and joining threads.
 */

#include "thread.h"

#ifdef HAVE_GETTID
# include <sys/syscall.h>
#endif

/*
 * The following structure maintains all state for a thread
 * including thread local storage slots.
 */

typedef struct Thread {
    struct Thread  *nextPtr;         /* Next in list of all threads. */
    time_t          ctime;           /* Thread structure create time. */
    unsigned int    flags;           /* Detached, joined, etc. */
    Ns_ThreadProc  *proc;            /* Thread startup routine. */
    void           *arg;             /* Argument to startup proc. */
    uintptr_t       tid;             /* Id set by thread for logging. */
    uint64_t        ostid;           /* OS level thread id (if available) */
    unsigned char  *bottomOfStack;   /* for estimating currentStackSize */
    char            name[NS_THREAD_NAMESIZE+1];   /* Thread name. */
    char            parent[NS_THREAD_NAMESIZE+1]; /* Parent name. */
} Thread;

static Thread *NewThread(void) NS_GNUC_RETURNS_NONNULL;
static Thread *GetThread(void) NS_GNUC_RETURNS_NONNULL;
static void CleanupThread(void *arg);
static void SetBottomOfStack(void *ptr) NS_GNUC_NONNULL(1);
static void SetOsThreadId(Thread *thrPtr) NS_GNUC_NONNULL(1);

/*
 * The pointer firstThreadPtr is the anchor of a linked list of all threads.
 */

static Thread *firstThreadPtr;

/*
 * The following maintains the TLS key for the thread context.
 */

static Ns_Tls key;
static size_t defstacksize = 0u;


/*
 *----------------------------------------------------------------------
 *
 * NsInitThreads --
 *
 *      Initialize threads interface.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Allocates pthread_key_t for thread context.
 *
 *----------------------------------------------------------------------
 */

void
NsInitThreads(void)
{
    static bool initialized = NS_FALSE;

    if (!initialized) {
        initialized = NS_TRUE;
        NsInitMaster();
        NsInitReentrant();
        Ns_TlsAlloc(&key, CleanupThread);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadCreate --
 *
 *      Create a new thread thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A new thread is allocated and started.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadCreate(Ns_ThreadProc *proc, void *arg, ssize_t stackSize,
                Ns_Thread *resultPtr)
{
    Thread     *thrPtr;
    size_t      nameLength;
    const char *name;

    NS_NONNULL_ASSERT(proc != NULL);

    Ns_MasterLock();

    if (stackSize < 0) {
        stackSize = (ssize_t)defstacksize;
    }

    /*
     * Allocate a new thread structure and update values
     * which are known for threads created here.
     */

    thrPtr = NewThread();
    thrPtr->proc = proc;
    thrPtr->arg = arg;
    if (resultPtr == NULL) {
        thrPtr->flags = NS_THREAD_DETACHED;
    }
    name = Ns_ThreadGetName();
    nameLength = strlen(name);
    assert(nameLength <= NS_THREAD_NAMESIZE);
    memcpy(thrPtr->parent, name, nameLength + 1u);
    Ns_MasterUnlock();

    NsCreateThread(thrPtr, stackSize, resultPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadStackSize --
 *
 *      Set default stack size.
 *
 * Results:
 *      Previous stack size.
 *
 * Side effects:
 *      New threads will use default size.
 *
 *----------------------------------------------------------------------
 */

ssize_t
Ns_ThreadStackSize(ssize_t size)
{
    ssize_t prev;

    Ns_MasterLock();
    prev = (ssize_t)defstacksize;
    if (size > 0) {
        defstacksize = (size_t)size;
    }
    Ns_MasterUnlock();

    return prev;
}


static void
SetOsThreadId(Thread *thrPtr)
{
    thrPtr->ostid = 0u;
#ifdef HAVE_GETTID
    thrPtr->ostid = (uint64_t)syscall(SYS_gettid);
#elif defined(HAVE_PTHREAD_THREADID_NP)
    {
        uint64_t ostid;

        if (pthread_threadid_np(NULL, &ostid) == 0) {
            thrPtr->ostid = ostid;
        }
    }
#elif defined(_WIN32)
    thrPtr->ostid = (uint64_t)GetCurrentThreadId();
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * ThreadMain --
 *
 *      Thread startup routine.  Sets the given preallocated thread
 *      structure and calls the user specified procedure.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Will call Ns_ThreadExit() if not already done by the user code.
 *
 *----------------------------------------------------------------------
 */

void
NsThreadMain(void *arg)
{
    Thread  *thrPtr = (Thread *) arg;

    Ns_MasterLock();
    thrPtr->tid = Ns_ThreadId();
    Ns_MasterUnlock();
    Ns_TlsSet(&key, thrPtr);
    Ns_ThreadSetName("-thread:%" PRIxPTR "-", thrPtr->tid);
    SetBottomOfStack(&thrPtr);
    SetOsThreadId(thrPtr);

    /*
     * Invoke the user-supplied workhorse for this thread.
     * "Hier spielt die Musik!"
     */
    (*thrPtr->proc) (thrPtr->arg);

    /*
     * Controllably exit this thread, pulling all of the
     * cleanup callbacks that need to be run.
     */
    Ns_ThreadExit(NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadGetName --
 *
 *      Return a pointer to calling thread's string name.
 *
 * Results:
 *      Pointer to thread name string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ThreadGetName(void)
{
    Thread *thisPtr = GetThread();

    return thisPtr->name;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadSetName --
 *
 *      Set the name of the calling thread.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      String is copied to thread data structure.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadSetName(const char *fmt, ...)
{
    Thread *thisPtr = GetThread();
    va_list ap;

    NS_NONNULL_ASSERT(fmt != NULL);

    Ns_MasterLock();
    va_start(ap, fmt);
    vsnprintf(thisPtr->name, NS_THREAD_NAMESIZE, fmt, ap);
    va_end(ap);
    Ns_MasterUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadGetParent --
 *
 *      Return a pointer to calling thread's parent name.
 *
 * Results:
 *      Pointer to thread parent name string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_ThreadGetParent(void)
{
    Thread *thisPtr = GetThread();

    return thisPtr->parent;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadList --
 *
 *      Append info for each thread.
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
Ns_ThreadList(Tcl_DString *dsPtr, Ns_ThreadArgProc *proc)
{
    Thread *thrPtr;
    char    buf[100];

    NS_NONNULL_ASSERT(dsPtr != NULL);

    Ns_MasterLock();
    for (thrPtr = firstThreadPtr; (thrPtr != NULL); thrPtr = thrPtr->nextPtr) {

        if ((thrPtr->flags & NS_THREAD_EXITED) == 0u) {
            TCL_SIZE_T written;

            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppendElement(dsPtr, thrPtr->name);
            Tcl_DStringAppendElement(dsPtr, thrPtr->parent);
            written = (TCL_SIZE_T)snprintf(buf, sizeof(buf), " %" PRIxPTR " %d %" PRId64,
                                           thrPtr->tid, thrPtr->flags, (int64_t) thrPtr->ctime);
            Tcl_DStringAppend(dsPtr, buf, written);
            if (proc != NULL) {
                (*proc)(dsPtr, thrPtr->proc, thrPtr->arg);
                Tcl_DStringAppend(dsPtr, " ", 1);
            } else {
                unsigned char addrBuffer[sizeof(thrPtr->proc)];
                TCL_SIZE_T i;

                /*
                 * Obtain the hex value of the function pointer;
                 */
                memcpy(addrBuffer, &thrPtr->proc, sizeof(thrPtr->proc));
                Tcl_DStringAppend(dsPtr, " 0x", 3);
                for (i = sizeof(thrPtr->proc) - 1; i >= 0 ; i--) {
                    written = (TCL_SIZE_T)snprintf(buf, sizeof(buf), "%02x", addrBuffer[i]);
                    Tcl_DStringAppend(dsPtr, buf, written);
                }
                written = (TCL_SIZE_T)snprintf(buf, sizeof(buf), " %p ", thrPtr->arg);
                Tcl_DStringAppend(dsPtr, buf, written);
            }

            written = (TCL_SIZE_T)ns_uint64toa(buf, thrPtr->ostid);
            Tcl_DStringAppend(dsPtr, buf, written);

            Tcl_DStringEndSublist(dsPtr);
        }
    }
    Ns_MasterUnlock();
}


static void
ThreadCputimeAppend(Tcl_DString *dsPtr, uint64_t tid,
                    uint64_t userUsec, uint64_t systemUsec)
{
    char buf[64];

    /*
     * Append dictionary key.
     */
    ns_uint64toa(buf, tid);
    Tcl_DStringAppendElement(dsPtr, buf);

    /*
     * Append dictionary value as a nested list:
     *
     *     {user 123 system 456}
     */
    Tcl_DStringStartSublist(dsPtr);

    Tcl_DStringAppendElement(dsPtr, "user");
    ns_uint64toa(buf, userUsec);
    Tcl_DStringAppendElement(dsPtr, buf);

    Tcl_DStringAppendElement(dsPtr, "system");
    ns_uint64toa(buf, systemUsec);
    Tcl_DStringAppendElement(dsPtr, buf);

    Tcl_DStringEndSublist(dsPtr);
}

#if defined(__linux__)

static bool
ThreadCputimeLinux(uint64_t tid, long ticksPerSecond,
                   uint64_t *userUsecPtr, uint64_t *systemUsecPtr)
{
    char    path[128];
    char    buffer[4096];
    char   *p;
    int     fd;
    ssize_t nread;
    uint64_t uticks, sticks;

    *userUsecPtr = 0u;
    *systemUsecPtr = 0u;

    if (ticksPerSecond <= 0) {
        return NS_FALSE;
    }

    snprintf(path, sizeof(path),
             "/proc/self/task/%" PRIu64 "/stat", tid);

    fd = open(path, O_RDONLY);
    if (fd == -1) {
        return NS_FALSE;
    }

    nread = read(fd, buffer, sizeof(buffer) - 1u);
    close(fd);

    if (nread <= 0) {
        return NS_FALSE;
    }
    buffer[nread] = '\0';

    /*
     * The second field is "(comm)" and may contain spaces or closing
     * parentheses. Start after its final ')'.
     */
    p = strrchr(buffer, ')');
    if (p == NULL || p[1] != ' ') {
        return NS_FALSE;
    }
    p += 2;

    /*
     * p starts at field 3, "state". Advance to fields 14 and 15.
     *
     * A small token parser is preferable to parsing the complete line
     * with one sscanf().
     */
    {
        unsigned int field;
        char        *end;

        /*
         * Skip fields 3 through 13.
         */
        for (field = 3u; field <= 13u; field++) {
            while (*p != '\0' && !CHARTYPE(space, *p)) {
                p++;
            }
            while (CHARTYPE(space, *p)) {
                p++;
            }
        }

        errno = 0;
        uticks = strtoull(p, &end, 10);
        if (end == p || errno != 0) {
            return NS_FALSE;
        }

        p = end;
        while (CHARTYPE(space, *p)) {
            p++;
        }

        errno = 0;
        sticks = strtoull(p, &end, 10);
        if (end == p || errno != 0) {
            return NS_FALSE;
        }
    }

    *userUsecPtr =
        uticks * UINT64_C(1000000) / (uint64_t)ticksPerSecond;
    *systemUsecPtr =
        sticks * UINT64_C(1000000) / (uint64_t)ticksPerSecond;

    return NS_TRUE;
}

static void
ThreadCputimesLinux(Tcl_DString *dsPtr,
                    const uint64_t *tids, size_t count)
{
    size_t i;
    long   ticksPerSecond = sysconf(_SC_CLK_TCK);

    for (i = 0u; i < count; i++) {
        uint64_t userUsec, systemUsec;

        if (ThreadCputimeLinux(tids[i], ticksPerSecond, &userUsec, &systemUsec)) {
            ThreadCputimeAppend(dsPtr, tids[i],
                                userUsec, systemUsec);
        }
    }
}

#elif defined(__APPLE__)

#include <mach/mach.h>
#include <mach/thread_info.h>

static bool
ThreadIdWanted(uint64_t tid, const uint64_t *tids, size_t count)
{
    size_t i;

    for (i = 0u; i < count; i++) {
        if (tids[i] == tid) {
            return NS_TRUE;
        }
    }
    return NS_FALSE;
}

static void
ThreadCputimesDarwin(Tcl_DString *dsPtr,
                     const uint64_t *tids, size_t count)
{
    thread_act_array_t     threads = NULL;
    mach_msg_type_number_t threadCount = 0u;
    kern_return_t          kr;
    mach_msg_type_number_t i;

    kr = task_threads(mach_task_self(), &threads, &threadCount);
    if (kr != KERN_SUCCESS) {
        return;
    }

    for (i = 0u; i < threadCount; i++) {
        thread_identifier_info_data_t identifier;
        mach_msg_type_number_t identifierCount =
            THREAD_IDENTIFIER_INFO_COUNT;

        kr = thread_info(threads[i],
                         THREAD_IDENTIFIER_INFO,
                         (thread_info_t)&identifier,
                         &identifierCount);

        if (kr == KERN_SUCCESS
            && ThreadIdWanted(identifier.thread_id, tids, count)) {

            thread_basic_info_data_t basic;
            mach_msg_type_number_t basicCount =
                THREAD_BASIC_INFO_COUNT;

            kr = thread_info(threads[i],
                             THREAD_BASIC_INFO,
                             (thread_info_t)&basic,
                             &basicCount);

            if (kr == KERN_SUCCESS) {
                uint64_t userUsec =
                    (uint64_t)basic.user_time.seconds
                    * UINT64_C(1000000)
                    + (uint64_t)basic.user_time.microseconds;

                uint64_t systemUsec =
                    (uint64_t)basic.system_time.seconds
                    * UINT64_C(1000000)
                    + (uint64_t)basic.system_time.microseconds;

                ThreadCputimeAppend(dsPtr,
                                    identifier.thread_id,
                                    userUsec,
                                    systemUsec);
            }
        }
    }

    /*
     * task_threads() returns send rights and allocated array storage.
     */
    for (i = 0u; i < threadCount; i++) {
        (void)mach_port_deallocate(mach_task_self(), threads[i]);
    }

    if (threads != NULL) {
        (void)vm_deallocate(
            mach_task_self(),
            (vm_address_t)threads,
            (vm_size_t)(threadCount * sizeof(*threads)));
    }
}

#elif defined(_WIN32)
static uint64_t
FileTimeToUsec(const FILETIME *timePtr)
{
    ULARGE_INTEGER value;

    value.LowPart  = timePtr->dwLowDateTime;
    value.HighPart = timePtr->dwHighDateTime;

    /*
     * FILETIME is measured in 100-nanosecond units.
     */
    return value.QuadPart / UINT64_C(10);
}

static void
ThreadCputimesWindows(Tcl_DString *dsPtr,
                      const uint64_t *tids, size_t count)
{
    size_t i;

    for (i = 0u; i < count; i++) {
        HANDLE   thread;
        FILETIME creationTime, exitTime, kernelTime, userTime;

        if (tids[i] > UINT32_MAX) {
            continue;
        }

        thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
                            FALSE, (DWORD)tids[i]);
        if (thread == NULL) {
            continue;
        }

        if (GetThreadTimes(thread,
                           &creationTime, &exitTime,
                           &kernelTime, &userTime) != 0) {
            ThreadCputimeAppend(dsPtr,
                                tids[i],
                                FileTimeToUsec(&userTime),
                                FileTimeToUsec(&kernelTime));
        }

        CloseHandle(thread);
    }
}
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadCpuTimes --
 *
 *      Append cumulative per-thread CPU times to dsPtr as a Tcl
 *      dictionary. Dictionary keys are the operating-system thread IDs
 *      reported by Ns_ThreadList(). Each value is a sub-dictionary with
 *      user and system CPU time in microseconds:
 *
 *          tid {user usec system usec}
 *
 *      Threads for which accounting information cannot be obtained are
 *      omitted. On unsupported platforms, an empty dictionary is
 *      returned.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Queries operating-system thread accounting information and
 *      appends Tcl-list elements to dsPtr.
 *
 *----------------------------------------------------------------------
 */
void
Ns_ThreadCpuTimes(Tcl_DString *dsPtr)
{
    Thread   *thrPtr;
    uint64_t *tids;
    size_t    count = 0u;
    size_t    i = 0u;

    NS_NONNULL_ASSERT(dsPtr != NULL);

    /*
     * Take a stable snapshot of the active NaviServer OS thread IDs.
     * Do not hold the master lock while performing OS queries.
     */
    Ns_MasterLock();

    for (thrPtr = firstThreadPtr;
         thrPtr != NULL;
         thrPtr = thrPtr->nextPtr) {

        if ((thrPtr->flags & NS_THREAD_EXITED) == 0u
            && thrPtr->ostid != 0u) {
            count++;
        }
    }

    tids = count > 0u
        ? ns_malloc(count * sizeof(uint64_t))
        : NULL;

    for (thrPtr = firstThreadPtr;
         thrPtr != NULL;
         thrPtr = thrPtr->nextPtr) {

        if ((thrPtr->flags & NS_THREAD_EXITED) == 0u
            && thrPtr->ostid != 0u) {
            tids[i++] = thrPtr->ostid;
        }
    }

    Ns_MasterUnlock();

#if defined(__linux__)
    ThreadCputimesLinux(dsPtr, tids, count);

#elif defined(__APPLE__)
    ThreadCputimesDarwin(dsPtr, tids, count);

#elif defined(_WIN32)
    ThreadCputimesWindows(dsPtr, tids, count);

#else
    /*
     * No implementation for this platform yet. Returning an empty
     * dictionary is intentional.
     */
    (void)dsPtr;
    (void)tids;
    (void)count;
#endif

    if (tids != NULL) {
        ns_free(tids);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadExit --
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
Ns_ThreadExit(void *arg)
{
    NsThreadShutdownStarted();

    /*
     * Clear TLS slots for this (now exiting) thread controllably,
     * augmenting the TLS cleanup invoked automatically by
     * the system's thread exit machinery. It is at this place
     * that we have the thread completely initialized, so an
     * proper cleanup has better chance to finish its work.
     */

    NsCleanupTls(NsGetTls());

    /*
     * Exiting thread needs to finalize the Tcl API after
     * all of the cleanup has been performed. Failing to
     * do so results in severe memory leakage.
     */

    Tcl_FinalizeThread();

   /*
    * Now, exit the thread really. This will invoke all of the
    * registered TLS cleanup callbacks again (no harm).
    */

    NsThreadExit(arg);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadResult --
 *
 *      Obtain the result of a terminating thread. The purpose of this
 *      function is to make the symbol Ns_ThreadResult() piublic and to keep
 *      the NsThreadResult() private, similar to Ns_ThreadExit and
 *      NsThreadExit().
 *
 * Results:
 *      Thread result, might be NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void *
Ns_ThreadResult(void *arg) {
    return NsThreadResult(arg);
}

/*
 *----------------------------------------------------------------------
 *
 * NewThread --
 *
 *      Allocate a new thread data structure and add it to the list
 *      of all threads.  The new thread is suitable for a detached,
 *      unknown thread such as the initial thread but Ns_ThreadCreate
 *      will update as necessary before creating the new threads.
 *
 * Results:
 *      Pointer to new Thread.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Thread *
NewThread(void)
{
    Thread *thrPtr;

    thrPtr = ns_calloc(1u, sizeof(Thread));
    thrPtr->ctime = time(NULL);
    memcpy(thrPtr->name, "-NONE-", 7);
    Ns_MasterLock();
    thrPtr->nextPtr = firstThreadPtr;
    firstThreadPtr = thrPtr;
    Ns_MasterUnlock();

    return thrPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * GetThread --
 *
 *      Return this thread's nsthread data structure, initializing
 *      it if necessary, normally for the first thread but also
 *      for threads created without Ns_ThreadCreate.
 *
 * Results:
 *      Pointer to per-thread data structure.
 *
 * Side effects:
 *      Key is allocated the first time.
 *
 *----------------------------------------------------------------------
 */

static Thread *
GetThread(void)
{
    Thread *thrPtr;

    thrPtr = Ns_TlsGet(&key);
    if (thrPtr == NULL) {
        thrPtr = NewThread();
        thrPtr->flags = NS_THREAD_DETACHED;
        thrPtr->tid = Ns_ThreadId();
        Ns_TlsSet(&key, thrPtr);
        SetOsThreadId(thrPtr);
    }
    return thrPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsThreadShutdownStarted --
 *
 *  Record in the thread structure that this thread is currently exiting. When
 *  e.g. a call during the (TLS)-cleanup calls Ns_ThreadList() or a similar
 *  command, then the thread arg structure might be already freed by an
 *  earlier cleanup call. By marking the thread as being deleted, we can
 *  handle such cases.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */
void
NsThreadShutdownStarted(void)
{
    Thread *thisPtr = GetThread();

    Ns_MasterLock();
    thisPtr->flags |= NS_THREAD_EXITED;
    Ns_MasterUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * CleanupThread --
 *
 *  TLS cleanup for the nsthread context.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void
CleanupThread(void *arg)
{
    Thread **thrPtrPtr;
    Thread *thrPtr = arg;

    Ns_MasterLock();
    thrPtrPtr = &firstThreadPtr;
    while (*thrPtrPtr != thrPtr) {
        thrPtrPtr = &(*thrPtrPtr)->nextPtr;
    }
    *thrPtrPtr = thrPtr->nextPtr;
    thrPtr->nextPtr = NULL;
    Ns_MasterUnlock();
    ns_free(thrPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * SetBottomOfStack --
 *
 *      Sets the bottom of the thread stack for estimating available
 *      stack size.
 *
 * Results:
 *      None,
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
SetBottomOfStack(void *ptr) {
    Thread *thisPtr = GetThread();

    NS_NONNULL_ASSERT(ptr != NULL);

    thisPtr->bottomOfStack = ptr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadGetThreadInfo --
 *
 *      Obtains various size information about the current C stack.
 *
 * Results:
 *      returns maxStackSize and estimatedSize into passed integers
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
void
Ns_ThreadGetThreadInfo(size_t *maxStackSize, size_t *estimatedSize) {
  Thread *thisPtr = GetThread();

  NS_NONNULL_ASSERT(maxStackSize != NULL);
  NS_NONNULL_ASSERT(estimatedSize != NULL);

  Ns_MasterLock();
  *maxStackSize = defstacksize;
  *estimatedSize = (size_t)labs((long)(thisPtr->bottomOfStack - (unsigned char *)&thisPtr));
  Ns_MasterUnlock();
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
