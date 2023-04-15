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
 * tls.c --
 *
 *      Thread local storage support for nsthreads.  Note that the nsthread
 *      library handles thread local storage directly.
 */

#include "thread.h"

/*
 * The following global variable specifies the maximum TLS id.  Modifying
 * this value has no effect.
 */

static uintptr_t nsThreadMaxTls = NS_THREAD_MAXTLS;

/*
 * Static functions defined in this file.
 */

static Ns_TlsCleanup *cleanupProcs[NS_THREAD_MAXTLS];


/*
 *----------------------------------------------------------------------
 *
 * Ns_TlsAlloc --
 *
 *      Allocate the next tls id.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Id is set in given tlsPtr.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TlsAlloc(Ns_Tls *keyPtr, Ns_TlsCleanup *cleanup)
{
    static uintptr_t nextkey = 1u;
    uintptr_t        key;

    NS_NONNULL_ASSERT(keyPtr != NULL);

    Ns_MasterLock();
    if (nextkey == nsThreadMaxTls) {
        Tcl_Panic("Ns_TlsAlloc: exceeded max tls: %" PRIuPTR, nsThreadMaxTls);
    }
    key = nextkey++;
    cleanupProcs[key] = cleanup;
    Ns_MasterUnlock();

    *keyPtr = (void *) key;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TlsSet --
 *
 *      Set the value for a threads tls slot.
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
Ns_TlsSet(const Ns_Tls *keyPtr, void *value)
{
    uintptr_t   key;

    NS_NONNULL_ASSERT(keyPtr != NULL);

    key = (uintptr_t) *keyPtr;
    if (key < 1 || key >= NS_THREAD_MAXTLS) {
        Tcl_Panic("Ns_TlsSet: invalid key: %" PRIuPTR
                  ": should be between 1 and %" PRIuPTR,
                  key, nsThreadMaxTls);
    } else {
        void **slots = NsGetTls();

        slots[key] = value;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TlsGet --
 *
 *      Get this thread's value in a tls slot.
 *
 * Results:
 *      Pointer in slot.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_TlsGet(const Ns_Tls *keyPtr)
{
    uintptr_t  key;
    void      *result;

    NS_NONNULL_ASSERT(keyPtr != NULL);

    key = (uintptr_t) *keyPtr;
    if (key < 1 || key >= NS_THREAD_MAXTLS) {
        result = NULL;
        Tcl_Panic("Ns_TlsGet: invalid key: %" PRIuPTR
                  ": should be between 1 and %" PRIuPTR,
                  key, nsThreadMaxTls);
    } else {
        void  **slots = NsGetTls();

        result = slots[key];
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsCleanupTls --
 *
 *      Cleanup thread local storage in LIFO order for an exiting thread.
 *      Note the careful use of the counters to keep iterating over the
 *      list, up to 5 times, until all TLS values are NULL.  This emulates
 *      the Pthread TLS behavior which catches a destructor inadvertently
 *      calling a library which resets a TLS value after it has been destroyed.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Cleanup procs are invoked for non-null values.
 *
 *----------------------------------------------------------------------
 */

void
NsCleanupTls(void **slots)
{
    NS_NONNULL_ASSERT(slots != NULL);

    if (
#if defined(TCL_IS_FIXED)
        1
#else
        NS_finalshutdown != 1
#endif
        ) {
        int  tries;
        bool retry;

        tries = 0;
        do {
            TCL_OBJC_T i;

            retry = NS_FALSE;
            i = NS_THREAD_MAXTLS;
            while (i-- > 0) {
                if (cleanupProcs[i] != NULL && slots[i] != NULL) {
                    void *arg;

                    arg = slots[i];
                    slots[i] = NULL;
                    (*cleanupProcs[i])(arg);
                    retry = NS_TRUE;
                }
            }
        } while (retry && tries++ < 5);
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
