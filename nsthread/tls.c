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
 * tls.c --
 *
 *	Thread local storage support for nsthreads.  Note that the nsthread
 *	library handles thread local storage directly.
 */

#include "thread.h"

/*
 * The following global variable specifies the maximum TLS id.  Modifying
 * this value has no effect.
 */

uintptr_t nsThreadMaxTls = NS_THREAD_MAXTLS;

/* 
 * Static functions defined in this file.
 */

static Ns_TlsCleanup *cleanupProcs[NS_THREAD_MAXTLS];


/*
 *----------------------------------------------------------------------
 *
 * Ns_TlsAlloc --
 *
 *	Allocate the next tls id.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Id is set in given tlsPtr.
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
 *	Set the value for a threads tls slot.
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
Ns_TlsSet(Ns_Tls *keyPtr, void *value)
{
    void      **slots = NsGetTls();
    uintptr_t   key;

    NS_NONNULL_ASSERT(keyPtr != NULL);

    key = (uintptr_t) *keyPtr;
     
    if (key < 1 || key >= NS_THREAD_MAXTLS) {
        Tcl_Panic("Ns_TlsSet: invalid key: %" PRIuPTR
                  ": should be between 1 and %" PRIuPTR,
                  key, nsThreadMaxTls);
    }
    slots[key] = value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TlsGet --
 *
 *	Get this thread's value in a tls slot.
 *
 * Results:
 *	Pointer in slot.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void *
Ns_TlsGet(Ns_Tls *keyPtr)
{
    void      **slots = NsGetTls();
    uintptr_t   key;

    NS_NONNULL_ASSERT(keyPtr != NULL);
    
    key = (uintptr_t) *keyPtr;
    if (key < 1 || key >= NS_THREAD_MAXTLS) {
        Tcl_Panic("Ns_TlsGet: invalid key: %" PRIuPTR
                  ": should be between 1 and %" PRIuPTR,
                  key, nsThreadMaxTls);
    }
    return slots[key];
}


/*
 *----------------------------------------------------------------------
 *
 * NsCleanupTls --
 *
 *	Cleanup thread local storage in LIFO order for an exiting thread.
 *	Note the careful use of the counters to keep iterating over the
 *	list, up to 5 times, until all TLS values are NULL.  This emulates
 *	the Pthread TLS behavior which catches a destructor inadvertantly
 *	calling a library which resets a TLS value after it's been destroyed.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Cleanup procs are invoked for non-null values.
 *
 *----------------------------------------------------------------------
 */

void
NsCleanupTls(void **slots)
{
    NS_NONNULL_ASSERT(slots != NULL);

    if (NS_finalshutdown != 1) {
        int trys, retry;
        
        trys = 0;
        do {
            int i;

            retry = 0;
            i = NS_THREAD_MAXTLS;
            while (i-- > 0) {
                if (cleanupProcs[i] != NULL && slots[i] != NULL) {
                    void *arg;
                        
                    arg = slots[i];
                    slots[i] = NULL;
                    (*cleanupProcs[i])(arg);
                    retry = 1;
                }
            }
        } while (retry && trys++ < 5);
    }
    Tcl_FinalizeThread();
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
