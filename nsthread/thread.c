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
 *	Routines for creating, exiting, and joining threads.
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
    struct Thread  *nextPtr;	     /* Next in list of all threads. */
    time_t	    ctime;	     /* Thread structure create time. */
    int		    flags;	     /* Detached, joined, etc. */
    Ns_ThreadProc  *proc;	     /* Thread startup routine. */
    void           *arg;	     /* Argument to startup proc. */
    uintptr_t       tid;             /* Id set by thread for logging. */
    pid_t           ostid;           /* OS level thread id (if available) */
    unsigned char  *bottomOfStack;   /* for estimating currentStackSize */
    char	    name[NS_THREAD_NAMESIZE+1];   /* Thread name. */
    char	    parent[NS_THREAD_NAMESIZE+1]; /* Parent name. */
} Thread;

static Thread *NewThread(void) NS_GNUC_RETURNS_NONNULL;
static Thread *GetThread(void) NS_GNUC_RETURNS_NONNULL;
static void CleanupThread(void *arg);
static void SetBottomOfStack(void *ptr)  NS_GNUC_NONNULL(1);

/*
 * The following pointer maintains a linked list of all threads.
 */

static Thread *firstThreadPtr;

/*
 * The following maintains the tls key for the thread context.
 */

static Ns_Tls key;
static long defstacksize = 0;


/*
 *----------------------------------------------------------------------
 *
 * NsInitThreads --
 *
 *	Initialize threads interface.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Allocates pthread_key_t for thread context.
 *
 *----------------------------------------------------------------------
 */

void
NsInitThreads(void)
{
    static int once = 0;

    if (once == 0) {
	once = 1;
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
 *	Create a new thread thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	A new thread is allocated and started.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadCreate(Ns_ThreadProc *proc, void *arg, long stack,
    	    	Ns_Thread *resultPtr)
{
    Thread *thrPtr;
    size_t nameLength;
    const char *name;

    assert(proc != NULL);

    Ns_MasterLock();

    if (stack <= 0) {
        stack = defstacksize;
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
    
    NsCreateThread(thrPtr, stack, resultPtr);
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

long
Ns_ThreadStackSize(long size)
{
    long prev;

    Ns_MasterLock();
    prev = defstacksize;
    if (size > 0) {
        defstacksize = size;
    }
    Ns_MasterUnlock();

    return prev;
}


/*
 *----------------------------------------------------------------------
 *
 * ThreadMain --
 *
 *	Thread startup routine.  Sets the given pre-allocated thread
 *	structure and calls the user specified procedure.
 *
 * Results:
 *	None.  Will call Ns_ThreadExit if not called by the
 *	user code.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
NsThreadMain(void *arg)
{
    Thread  *thrPtr = (Thread *) arg;
    char     name[NS_THREAD_NAMESIZE];

    thrPtr->tid = Ns_ThreadId();
    Ns_TlsSet(&key, thrPtr);
    snprintf(name, sizeof(name), "-thread:%" PRIxPTR "-", thrPtr->tid);
    Ns_ThreadSetName(name);
    SetBottomOfStack(&thrPtr);
#ifdef HAVE_GETTID
    thrPtr->ostid = syscall(SYS_gettid);
#endif
    (*thrPtr->proc) (thrPtr->arg);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadGetName --
 *
 *	Return a pointer to calling thread's string name.
 *
 * Results:
 *	Pointer to thread name string.
 *
 * Side effects:
 *	None.
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
 *	Set the name of the calling thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	String is copied to thread data structure.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadSetName(const char *name,...)
{
    Thread *thisPtr = GetThread();
    va_list ap;

    assert(name != NULL);
    
    Ns_MasterLock();
    va_start(ap, name);
    vsnprintf(thisPtr->name, NS_THREAD_NAMESIZE, name, ap);
    va_end(ap);
    Ns_MasterUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadGetParent --
 *
 *	Return a pointer to calling thread's parent name.
 *
 * Results:
 *	Pointer to thread parent name string.
 *
 * Side effects:
 *	None.
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
 *	Append info for each thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *  	None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ThreadList(Tcl_DString *dsPtr, Ns_ThreadArgProc *proc)
{
    Thread *thrPtr;
    char buf[100];

    assert(dsPtr != NULL);

    Ns_MasterLock();
    thrPtr = firstThreadPtr;
    while (thrPtr != NULL) {
	int written;

        Tcl_DStringStartSublist(dsPtr);
        Tcl_DStringAppendElement(dsPtr, thrPtr->name);
        Tcl_DStringAppendElement(dsPtr, thrPtr->parent);
        snprintf(buf, sizeof(buf), " %" PRIxPTR " %d %" PRIu64,
                 thrPtr->tid, thrPtr->flags, (int64_t) thrPtr->ctime);
        Tcl_DStringAppend(dsPtr, buf, -1);
        if (proc != NULL) {
	    (*proc)(dsPtr, thrPtr->proc, thrPtr->arg);
        } else {
	    /* 
	     * The only legal way to print a function pointer is by
	     * printing the bytes via casting to a character array.
	     */
	    unsigned char *p = (unsigned char *)thrPtr->proc;
	    int i;

            Tcl_DStringAppend(dsPtr, " 0x", 3);
	    for (i = 0; i < sizeof(thrPtr->proc); i++) {
		written = snprintf(buf, sizeof(buf), "%02x", p != NULL ? p[i] : 0);
		Tcl_DStringAppend(dsPtr, buf, written);
            }
            written = snprintf(buf, sizeof(buf), " %p", thrPtr->arg);
            Tcl_DStringAppend(dsPtr, buf, written);
        }
        written = snprintf(buf, sizeof(buf), " %" PRIuMAX , (uintmax_t) thrPtr->ostid);
        Tcl_DStringAppend(dsPtr, buf, written);

        Tcl_DStringEndSublist(dsPtr);
        thrPtr = thrPtr->nextPtr;
    }
    Ns_MasterUnlock();
}


/*
 *----------------------------------------------------------------------
 *
 * NewThread --
 *
 *	Allocate a new thread data structure and add it to the list
 *	of all threads.  The new thread is suitable for a detached,
 *	unknown thread such as the initial thread but Ns_ThreadCreate
 *	will update as necessary before creating the new threads.
 *
 * Results:
 *	Pointer to new Thread.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Thread *
NewThread(void)
{
    Thread *thrPtr;

    thrPtr = ns_calloc(1U, sizeof(Thread));
    thrPtr->ctime = time(NULL);
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
 *	Return this thread's nsthread data structure, initializing
 *	it if necessary, normally for the first thread but also
 *	for threads created without Ns_ThreadCreate.
 *
 * Results:
 *	Pointer to per-thread data structure.
 *
 * Side effects:
 *	Key is allocated the first time.
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
        thrPtr->ostid = syscall(SYS_gettid);
#endif
    }
    return thrPtr;
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
 *	Sets the bottom of the thread stack for estimating available
 *	stack size.
 *
 * Results:
 *	None,
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
SetBottomOfStack(void *ptr) {
    Thread *thisPtr = GetThread();

    assert(ptr != NULL);
    
    thisPtr->bottomOfStack = ptr;
    /*fprintf(stderr, "SetBottomOfStack %p %s bot %p\n", thisPtr, thisPtr->name, ptr);*/
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ThreadGetThreadInfo --
 *
 *	Obtains various size information about the current C stack.
 *
 * Results:
 *	returns maxStackSize and estimatedSize into passed integers
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */
void
Ns_ThreadGetThreadInfo(size_t *maxStackSize, size_t *estimatedSize) {
  Thread *thisPtr = GetThread();

  assert(maxStackSize != NULL);
  assert(estimatedSize != NULL);
  
  Ns_MasterLock();
  *maxStackSize = defstacksize;
  *estimatedSize = abs((int)(thisPtr->bottomOfStack - (unsigned char *)&thisPtr));
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
