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

static void **GetSlot(Ns_Sls *slsPtr, Ns_Sock *sock);
static Ns_Callback CleanupKeyed;

/* 
 * Static variables defined in this file.
 */

static Ns_Callback **cleanupProcs; /* Array of slot cleanup callbacks. */
static Ns_Sls        kslot;        /* Sls slot for keyed data. */



/*
 *----------------------------------------------------------------------
 *
 * NsInitSls --
 *
 *      Allocate an sls slot for keyed data shared with the Tcl API.
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
    cleanupProcs = ns_calloc(1, sizeof(Ns_Callback *));
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
    int id;

    if (Ns_InfoStarted()) {
        Ns_Log(Bug, "Ns_SlsAlloc: server already started");
    }
    id = nsconf.nextSlsId++;
    cleanupProcs = ns_realloc(cleanupProcs, sizeof(Ns_Callback *) * nsconf.nextSlsId);
    cleanupProcs[id] = cleanup;

    *slsPtr = INT2PTR(id);
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
Ns_SlsSet(Ns_Sls *slsPtr, Ns_Sock *sock, void *data)
{
    void **slotPtr;

    slotPtr = GetSlot(slsPtr, sock);
    if (slotPtr) {
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
Ns_SlsGet(Ns_Sls *slsPtr, Ns_Sock *sock)
{
    void **slotPtr;

    slotPtr = GetSlot(slsPtr, sock);
    if (slotPtr) {
        return *slotPtr;
    }
    return NULL;
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
Ns_SlsSetKeyed(Ns_Sock *sock, CONST char *key, CONST char *value)
{
    Tcl_HashTable *tblPtr;
    Tcl_HashEntry *hPtr;
    char          *old, *new;
    size_t        len;
    int           created;

    if ((tblPtr = Ns_SlsGet(&kslot, sock)) == NULL) {
        tblPtr = ns_malloc(sizeof(Tcl_HashTable));
        Tcl_InitHashTable(tblPtr, TCL_STRING_KEYS);
        Ns_SlsSet(&kslot, sock, tblPtr);
    }
    hPtr = Tcl_CreateHashEntry(tblPtr, key, &created);
    len = strlen(value);
    old = Tcl_GetHashValue(hPtr);
    new = ns_realloc(old, (size_t)(len+1));
    memcpy(new, value, (size_t)(len+1));
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

char *
Ns_SlsGetKeyed(Ns_Sock *sock, CONST char *key)
{
    Tcl_HashTable *tblPtr;
    Tcl_HashEntry *hPtr;
    char          *value = NULL;

    if ((tblPtr = Ns_SlsGet(&kslot, sock)) == NULL) {
        return NULL;
    }
    hPtr = Tcl_FindHashEntry(tblPtr, key);
    if (hPtr) {
        value = Tcl_GetHashValue(hPtr);
    }
    return value;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SlsAppendKeyed --
 *
 *      Append all key/value pairs from socket local storage to
 *      given dstring in propper Tcl list format.
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
    Tcl_HashTable  *tblPtr;
    Tcl_HashSearch  search;
    Tcl_HashEntry  *hPtr;

    if ((tblPtr = Ns_SlsGet(&kslot, sock)) == NULL) {
        return NULL;
    }
    hPtr = Tcl_FirstHashEntry(tblPtr, &search);
    while (hPtr) {
        Ns_DStringAppendElement(dest, Tcl_GetHashKey(tblPtr, hPtr));
        Ns_DStringAppendElement(dest, Tcl_GetHashValue(hPtr));
        hPtr = Tcl_NextHashEntry(&search);
    }
    return Ns_DStringValue(dest);
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
Ns_SlsUnsetKeyed(Ns_Sock *sock, CONST char *key)
{
    Tcl_HashTable *tblPtr;

    if ((tblPtr = Ns_SlsGet(&kslot, sock)) != NULL) {
        Tcl_HashEntry *hPtr = Tcl_FindHashEntry(tblPtr, key);

        if (hPtr) {
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
 *      Implements the ns_sls command.
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
NsTclSlsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Conn    *conn;
    Ns_Sock    *sock;
    Ns_DString  ds;
    char       *data;
    int         cmd;

    static const char *cmds[] = {
        "array", "get", "set", "unset", NULL
    };
    enum ISubCmdIdx {
        CArrayIdx, CGetIdx, CSetIdx, CUnsetIdx
    };

    if ((conn = Ns_TclGetConn(interp)) == NULL
        || (sock = Ns_ConnSockPtr(conn)) == NULL) {

        Tcl_SetResult(interp, "No connection available.", NULL);
        return TCL_ERROR;
    }

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], cmds, "command", 0, &cmd)
            != TCL_OK) {
        return TCL_ERROR;
    }

    switch (cmd) {

    case CArrayIdx:
        Ns_DStringInit(&ds);
        Ns_SlsAppendKeyed(&ds, sock);
        Tcl_DStringResult(interp, &ds);
        break;

    case CGetIdx:
        if (objc < 3 || objc > 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "key ?default?");
            return TCL_ERROR;
        }
        data = Ns_SlsGetKeyed(sock, Tcl_GetString(objv[2]));
        if (data == NULL) {
            if (objc == 4) {
                Tcl_SetObjResult(interp, objv[3]);
            } else {
                Tcl_SetResult(interp, "key does not exist and no default given", TCL_STATIC);
                return TCL_ERROR;
            }
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(data, -1));
        }
        break;

    case CSetIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "key value");
            return TCL_ERROR;
        }
        Ns_SlsSetKeyed(sock, Tcl_GetString(objv[2]), Tcl_GetString(objv[3]));
        break;

    case CUnsetIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "key");
            return TCL_ERROR;
        }
        Ns_SlsUnsetKeyed(sock, Tcl_GetString(objv[2]));
        break;

    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsSlsCleanup --
 *
 *      Cleanup socket local storage in LIFO order for a closing comm
 *      socket.  Iterate up to 5 times to catch cases where a cleanup
 *      callback inadvertantly resets a SLS value after it's been destroyed.
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
    int   trys, retry;

    trys = 0;
    do {
        int i = nsconf.nextSlsId;

        retry = 0;
        while (i-- > 0) {
            if (cleanupProcs[i] != NULL && sockPtr->sls[i] != NULL) {
                arg = sockPtr->sls[i];
                sockPtr->sls[i] = NULL;
                (*cleanupProcs[i])(arg);
                retry = 1;
            }
        }
    } while (retry && trys++ < 5);
}


/*
 *----------------------------------------------------------------------
 *
 * GetSlot --
 *
 *      Return the sls slot for the given key.
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
GetSlot(Ns_Sls *slsPtr, Ns_Sock *sock)
{
    Sock      *sockPtr = (Sock *) sock;
    int        id      = PTR2INT(*slsPtr);

    if (id >= nsconf.nextSlsId) {
        Ns_Fatal("Ns_Sls: invalid key: %d: must be between 0 and %d",
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
    Tcl_HashTable  *tblPtr = arg;
    Tcl_HashSearch  search;
    Tcl_HashEntry  *hPtr;

    hPtr = Tcl_FirstHashEntry(tblPtr, &search);
    while (hPtr) {
        ns_free(Tcl_GetHashValue(hPtr));
        hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(tblPtr);
    ns_free(tblPtr);
}
