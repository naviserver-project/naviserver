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
 * cls.c --
 *
 *      Connection local storage.
 */

#include "nsd.h"

/*
 * Static functions defined in this file.
 */

static Ns_Callback *cleanupProcs[NS_CONN_MAXCLS+1];
static void **GetSlot(const Ns_Cls *clsPtr, Ns_Conn *conn) NS_GNUC_PURE;


/*
 *----------------------------------------------------------------------
 *
 * Ns_ClsAlloc --
 *
 *      Allocate the next cls id.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Id is set in given clsPtr.
 *
 *----------------------------------------------------------------------
 */

void
Ns_ClsAlloc(Ns_Cls *clsPtr, Ns_Callback *cleanupProc)
{
    static uintptr_t nextId = 1u;
    uintptr_t        id;

    Ns_MasterLock();
    if (nextId == NS_CONN_MAXCLS) {
        Ns_Fatal("Ns_ClsAlloc: exceeded max cls: %d", NS_CONN_MAXCLS);
    }
    id = nextId++;
    cleanupProcs[id] = cleanupProc;
    Ns_MasterUnlock();
    *clsPtr = INT2PTR(id);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ClsSet --
 *
 *      Set the value for a threads cls slot.
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
Ns_ClsSet(const Ns_Cls *clsPtr, Ns_Conn *conn, void *value)
{
    void **slotPtr;

    slotPtr = GetSlot(clsPtr, conn);
    *slotPtr = value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ClsGet --
 *
 *      Get this thread's value in a cls slot.
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
Ns_ClsGet(const Ns_Cls *clsPtr, Ns_Conn *conn)
{
    void *const*slotPtr;

    slotPtr = GetSlot(clsPtr, conn);
    return *slotPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsClsCleanup --
 *
 *      Cleanup connection local storage in a manner similar to
 *      thread local storage.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Depends on callbacks.
 *
 *----------------------------------------------------------------------
 */

void
NsClsCleanup(Conn *connPtr)
{
    int tries, retry;
    void *arg;

    NS_NONNULL_ASSERT(connPtr != NULL);

    tries = 0;
    do {
      unsigned int i;

        retry = 0;
        i = NS_CONN_MAXCLS;
        while (i-- > 0u) {
            if (cleanupProcs[i] != NULL && connPtr->cls[i] != NULL) {
                arg = connPtr->cls[i];
                connPtr->cls[i] = NULL;
                (*cleanupProcs[i])(arg);
                retry = 1;
            }
        }
    } while ((retry != 0) && (tries++ < 5));
}


/*
 *----------------------------------------------------------------------
 *
 * GetSlot --
 *
 *      Return the cls slot for the given key.
 *
 * Results:
 *      Pointer to slot.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void **
GetSlot(const Ns_Cls *clsPtr, Ns_Conn *conn)
{
    Conn *connPtr = (Conn *) conn;
    uintptr_t idx = (uintptr_t) *clsPtr;

    if (idx < 1u || idx >= NS_CONN_MAXCLS) {
        Ns_Fatal("Ns_Cls: invalid key: %" PRIuPTR ": must be between 1 and %d",
                 idx, NS_CONN_MAXCLS);
    }
    return &connPtr->cls[idx];
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
