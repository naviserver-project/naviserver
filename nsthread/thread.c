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
    pid_t           ostid;           /* OS level thread id (if available) */
    unsigned char  *bottomOfStack;   /* for estimating currentStackSize */
    char            name[NS_THREAD_NAMESIZE+1];   /* Thread name. */
    char            parent[NS_THREAD_NAMESIZE+1]; /* Parent name. */
} Thread;

static Thread *NewThread(void) NS_GNUC_RETURNS_NONNULL;
static Thread *GetThread(void) NS_GNUC_RETURNS_NONNULL;
static void CleanupThread(void *arg);
static void SetBottomOfStack(void *ptr)  NS_GNUC_NONNULL(1);

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

#ifdef HAVE_GETTID
    thrPtr->ostid = (pid_t)syscall(SYS_gettid);
#endif

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
            int written;

            Tcl_DStringStartSublist(dsPtr);
            Tcl_DStringAppendElement(dsPtr, thrPtr->name);
            Tcl_DStringAppendElement(dsPtr, thrPtr->parent);
            written = snprintf(buf, sizeof(buf), " %" PRIxPTR " %d %" PRId64,
                               thrPtr->tid, thrPtr->flags, (int64_t) thrPtr->ctime);
            Tcl_DStringAppend(dsPtr, buf, written);
            if (proc != NULL) {
                (*proc)(dsPtr, thrPtr->proc, thrPtr->arg);
                Tcl_DStringAppend(dsPtr, " ", 1);
            } else {
                unsigned char addrBuffer[sizeof(thrPtr->proc)];
                int i;

                /*
                 * Obtain the hex value of the function pointer;
                 */
                memcpy(addrBuffer, &thrPtr->proc, sizeof(thrPtr->proc));
                Tcl_DStringAppend(dsPtr, " 0x", 3);
                for (i = sizeof(thrPtr->proc) - 1; i >= 0 ; i--) {
                    written = snprintf(buf, sizeof(buf), "%02x", addrBuffer[i]);
                    Tcl_DStringAppend(dsPtr, buf, written);
                }
                written = snprintf(buf, sizeof(buf), " %p ", thrPtr->arg);
                Tcl_DStringAppend(dsPtr, buf, written);
            }

            written = ns_uint32toa(buf, (uint32_t)thrPtr->ostid);
            Tcl_DStringAppend(dsPtr, buf, written);

            Tcl_DStringEndSublist(dsPtr);
        }
    }
    Ns_MasterUnlock();
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
#ifdef HAVE_GETTID
        thrPtr->ostid = (pid_t)syscall(SYS_gettid);
#endif
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
