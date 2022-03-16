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
 * sls.c --
 *
 *      Socket local storage: data which persists for the lifetime of a
 *      TCP connection.
 *
 *      See: cls.c for connection local storage.
 */

#include "nsd.h"

/* 
 * Static functions defined in this file.
 */

static void **GetSlot(const Ns_Sls *slsPtr, Ns_Sock *sock) NS_GNUC_PURE;
static Ns_Callback CleanupKeyed;

/* 
 * Static variables defined in this file.
 */

static Ns_Callback **cleanupProcs; /* Array of slot cleanup callbacks. */
static Ns_Sls        kslot;        /* SLS slot for keyed data. */



/*
 *----------------------------------------------------------------------
 *
 * NsInitSls --
 *
 *      Allocate an SLS slot for keyed data shared with the Tcl API.
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
NsInitSls(void)
{
    cleanupProcs = ns_calloc(1u, sizeof(Ns_Callback *));
    Ns_SlsAlloc(&kslot, CleanupKeyed);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsAlloc --
 *
 *      Allocate the next sls id.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The cleanupProcs array is expanded.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SlsAlloc(Ns_Sls *slsPtr, Ns_Callback *cleanup)
{
    uintptr_t id;

    if (Ns_InfoStarted()) {
        Ns_Log(Bug, "Ns_SlsAlloc: server already started");
    }
    id = nsconf.nextSlsId++;
    cleanupProcs = ns_realloc(cleanupProcs, sizeof(Ns_Callback *) * nsconf.nextSlsId);
    cleanupProcs[id] = cleanup;

    *slsPtr = (Ns_Sls)UINT2PTR(id);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsSet --
 *
 *      Set data for given slot.
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
Ns_SlsSet(const Ns_Sls *slsPtr, Ns_Sock *sock, void *data)
{
    void **slotPtr;

    slotPtr = GetSlot(slsPtr, sock);
    if (slotPtr != NULL) {
        *slotPtr = data;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsGet --
 *
 *      Return the given slot's data.
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
Ns_SlsGet(const Ns_Sls *slsPtr, Ns_Sock *sock)
{
    void *const* slotPtr, *result = NULL;

    slotPtr = GetSlot(slsPtr, sock);
    if (slotPtr != NULL) {
        result = *slotPtr;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsSetKeyed --
 *
 *      Copy value into sls under the given key.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Memory for a Tcl_HashTable is allocated on first access.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SlsSetKeyed(Ns_Sock *sock, const char *key, const char *value)
{
    Tcl_HashTable *tblPtr;
    Tcl_HashEntry *hPtr;
    char          *old, *new;
    size_t        len;
    int           created;

    tblPtr = Ns_SlsGet(&kslot, sock);
    if (tblPtr == NULL) {
        tblPtr = ns_malloc(sizeof(Tcl_HashTable));
        Tcl_InitHashTable(tblPtr, TCL_STRING_KEYS);
        Ns_SlsSet(&kslot, sock, tblPtr);
    }
    hPtr = Tcl_CreateHashEntry(tblPtr, key, &created);
    len = strlen(value);
    old = Tcl_GetHashValue(hPtr);
    new = ns_realloc(old, len + 1u);
    memcpy(new, value, len + 1u);
    Tcl_SetHashValue(hPtr, new);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsGetKeyed --
 *
 *      Get value associated with given key.
 *
 * Results:
 *      Pointer to value or NULL if not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_SlsGetKeyed(Ns_Sock *sock, const char *key)
{
    Tcl_HashTable *tblPtr;
    const char    *value = NULL;

    tblPtr = Ns_SlsGet(&kslot, sock);
    if (tblPtr != NULL) {
        const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(tblPtr, key);
        
        if (hPtr != NULL) {
            value = Tcl_GetHashValue(hPtr);
        }
    }
    return value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsAppendKeyed --
 *
 *      Append all key/value pairs from socket local storage to
 *      given dstring in proper Tcl list format.
 *
 * Results:
 *      Pointer to dest.string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_SlsAppendKeyed(Ns_DString *dest, Ns_Sock *sock)
{
    Tcl_HashTable *tblPtr;
    char          *value = NULL;

    tblPtr = Ns_SlsGet(&kslot, sock);
    if (tblPtr != NULL) {
        Tcl_HashSearch        search;
        const Tcl_HashEntry  *hPtr = Tcl_FirstHashEntry(tblPtr, &search);

        while (hPtr != NULL) {
            Ns_DStringAppendElement(dest, Tcl_GetHashKey(tblPtr, hPtr));
            Ns_DStringAppendElement(dest, Tcl_GetHashValue(hPtr));
            hPtr = Tcl_NextHashEntry(&search);
        }
        value = Ns_DStringValue(dest);
    }
    return value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsUnsetKeyed --
 *
 *      Unset the data associated with the given key.
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
Ns_SlsUnsetKeyed(Ns_Sock *sock, const char *key)
{
    Tcl_HashTable *tblPtr;

    tblPtr = Ns_SlsGet(&kslot, sock);
    if (tblPtr != NULL) {
        Tcl_HashEntry *hPtr = Tcl_FindHashEntry(tblPtr, key);

        if (hPtr != NULL) {
            ns_free(Tcl_GetHashValue(hPtr));
            Tcl_DeleteHashEntry(hPtr);
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSlsObjCmd --
 *
 *      Implements "ns_sls".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Depends on sub command.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSlsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const Ns_Conn *conn;
    Ns_Sock       *sock = NULL;
    Ns_DString     ds;
    int            cmd, result = TCL_OK;

    static const char *const cmds[] = {
        "array", "get", "set", "unset", NULL
    };
    enum ISubCmdIdx {
        CArrayIdx, CGetIdx, CSetIdx, CUnsetIdx
    };

    conn = Ns_TclGetConn(interp);
    if (conn != NULL) {
        sock = Ns_ConnSockPtr(conn);
    }
    if (sock == NULL) {
        Ns_TclPrintfResult(interp, "No connection available");
        result = TCL_ERROR;
        
    } else if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], cmds, "command", 0, &cmd) != TCL_OK) {
        result = TCL_ERROR;

    } else {

        switch (cmd) {

        case CArrayIdx:
            Ns_DStringInit(&ds);
            (void) Ns_SlsAppendKeyed(&ds, sock);
            Tcl_DStringResult(interp, &ds);
            break;

        case CGetIdx:
            if (objc < 3 || objc > 4) {
                Tcl_WrongNumArgs(interp, 2, objv, "key ?default?");
                result = TCL_ERROR;
            } else {
                const char *data = Ns_SlsGetKeyed(sock, Tcl_GetString(objv[2]));
            
                if (data == NULL) {
                    if (objc == 4) {
                        Tcl_SetObjResult(interp, objv[3]);
                    } else {
                        Ns_TclPrintfResult(interp, "key does not exist and no default given");
                        result =  TCL_ERROR;
                    }
                } else {
                    Tcl_SetObjResult(interp, Tcl_NewStringObj(data, -1));
                }
            }
            break;

        case CSetIdx:
            if (objc != 4) {
                Tcl_WrongNumArgs(interp, 2, objv, "key value");
                result = TCL_ERROR;
            } else {
                Ns_SlsSetKeyed(sock, Tcl_GetString(objv[2]), Tcl_GetString(objv[3]));
            }
            break;

        case CUnsetIdx:
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "key");
                result = TCL_ERROR;
            } else {
                Ns_SlsUnsetKeyed(sock, Tcl_GetString(objv[2]));
            }
            break;

        default:
            /* unexpected value */
            assert(cmd && 0);
            break;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsSlsCleanup --
 *
 *      Cleanup socket local storage in LIFO order for a closing comm socket.
 *      Iterate up to 5 times to catch cases where a cleanup callback
 *      inadvertently resets a SLS value after it has been destroyed.
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
NsSlsCleanup(Sock *sockPtr)
{
    void *arg;
    int   tries, retry;

    NS_NONNULL_ASSERT(sockPtr != NULL);
    
    tries = 0;
    do {
        uintptr_t i = nsconf.nextSlsId;

        retry = 0;
        while (i-- > 0u) {
            if (cleanupProcs[i] != NULL && sockPtr->sls[i] != NULL) {
                arg = sockPtr->sls[i];
                sockPtr->sls[i] = NULL;
                (*cleanupProcs[i])(arg);
                retry = 1;
            }
        }
    } while ((retry != 0) && tries++ < 5);
}


/*
 *----------------------------------------------------------------------
 *
 * GetSlot --
 *
 *      Return the SLS slot for the given key.
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
GetSlot(const Ns_Sls *slsPtr, Ns_Sock *sock)
{
    Sock      *sockPtr = (Sock *) sock;
    uintptr_t  id      = (uintptr_t)(*slsPtr);

    if (id >= nsconf.nextSlsId) {
        Ns_Fatal("Ns_Sls: invalid key: %td: must be between 0 and %td",
                 id, nsconf.nextSlsId -1);
    }
    return &sockPtr->sls[id];
}


/*
 *----------------------------------------------------------------------
 *
 * CleanupKeyed --
 *
 *      Free memory for keyed data.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
CleanupKeyed(void *arg)
{
    Tcl_HashTable       *tblPtr = arg;
    Tcl_HashSearch       search;
    const Tcl_HashEntry *hPtr;

    hPtr = Tcl_FirstHashEntry(tblPtr, &search);
    while (hPtr != NULL) {
        ns_free(Tcl_GetHashValue(hPtr));
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(tblPtr);
    ns_free(tblPtr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */

