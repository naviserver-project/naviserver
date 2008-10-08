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
 * tclvar.c --
 *
 *      Tcl shared variables.
 */

#include "nsd.h"

NS_RCSID("@(#) $Header$");


/*
 * Local functions defined in this file.
 */

static void SetVar(NsArray *, Tcl_Obj *key, Tcl_Obj *value);
static void UpdateVar(Tcl_HashEntry *hPtr, Tcl_Obj *obj);
static NsArray *LockArray(void *arg, Tcl_Interp *interp, Tcl_Obj *array,
                        int create);

/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvCreateBuckets --
 *
 *      Create a new array of buckets for a server.
 *
 * Results:
 *      Pointer to bucket array.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

struct NsBucket *
NsTclCreateBuckets(CONST char *server, int n)
{
    char    buf[NS_THREAD_NAMESIZE];
    NsBucket *buckets;

    buckets = ns_malloc(sizeof(NsBucket) * n);
    while (--n >= 0) {
        snprintf(buf, sizeof(buf), "nsv:%d", n);
        Tcl_InitHashTable(&buckets[n].arrays, TCL_STRING_KEYS);
        Ns_MutexInit(&buckets[n].lock);
        Ns_MutexSetName2(&buckets[n].lock, buf, server);
    } 

    return buckets;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvGetObjCmd --
 *
 *      Implements nsv_get.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvGetObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    Tcl_HashEntry *hPtr;
    NsArray         *arrayPtr;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key");
        return TCL_ERROR;
    }
    arrayPtr = LockArray(arg, interp, objv[1], 0);
    if (arrayPtr == NULL) {
        return TCL_ERROR;
    }
    hPtr = Tcl_FindHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]));
    if (hPtr != NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
    }
    NsNsvUnlockArray(arrayPtr);
    if (hPtr == NULL) {
        Tcl_AppendResult(interp, "no such key: ", Tcl_GetString(objv[2]), NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvExistsObjCmd --
 *
 *      Implements nsv_exists.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvExistsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsArray *arrayPtr;
    int    exists;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key");
        return TCL_ERROR;
    }
    exists = 0;
    arrayPtr = LockArray(arg, NULL, objv[1], 0);
    if (arrayPtr != NULL) {
        if (Tcl_FindHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2])) != NULL) {
            exists = 1;
        }
        NsNsvUnlockArray(arrayPtr);
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(exists));

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvSetObjCmd --
 *
 *      Implelments nsv_set.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvSetObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsArray *arrayPtr;

    if (objc == 3) {
        return NsTclNsvGetObjCmd(arg, interp, objc, objv);
    } else if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?value?");
        return TCL_ERROR;
    }
    arrayPtr = LockArray(arg, interp, objv[1], 1);
    SetVar(arrayPtr, objv[2], objv[3]);
    NsNsvUnlockArray(arrayPtr);
    Tcl_SetObjResult(interp, objv[3]);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvIncrObjCmd --
 *
 *      Implements nsv_incr as an obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvIncrObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsArray         *arrayPtr;
    int            count, current, result, new;
    char          *value;
    Tcl_HashEntry *hPtr;

    if (objc != 3 && objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?count?");
        return TCL_ERROR;
    }
    if (objc == 3)  {
        count = 1;
    } else if (Tcl_GetIntFromObj(interp, objv[3], &count) != TCL_OK) {
        return TCL_ERROR;
    }
    arrayPtr = LockArray(arg, interp, objv[1], 1);
    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &new);
    if (new) {
        current = 0;
        result = TCL_OK;
    } else {
        value = Tcl_GetHashValue(hPtr);
        result = Tcl_GetInt(interp, value, &current);
    }
    if (result == TCL_OK) {
        Tcl_Obj *obj = Tcl_NewIntObj(current += count);
        UpdateVar(hPtr, obj);
        Tcl_SetObjResult(interp, obj);
    }
    NsNsvUnlockArray(arrayPtr);

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvLappendObjCmd --
 *
 *      Implements nsv_lappend command.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvLappendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsArray         *arrayPtr;
    int            i, new;
    Tcl_HashEntry *hPtr;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key string ?string ...?");
        return TCL_ERROR;
    }
    arrayPtr = LockArray(arg, interp, objv[1], 1);
    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &new);
    if (new) {
        Tcl_SetListObj(Tcl_GetObjResult(interp), objc-3, objv+3);
    } else {
        Tcl_SetResult(interp, Tcl_GetHashValue(hPtr), TCL_VOLATILE);
        for (i = 3; i < objc; ++i) {
            Tcl_AppendElement(interp, Tcl_GetString(objv[i]));
        }
    }
    UpdateVar(hPtr, Tcl_GetObjResult(interp));
    NsNsvUnlockArray(arrayPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvAppendObjCmd --
 *
 *      Implements nsv_append command.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvAppendObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsArray         *arrayPtr;
    int            i, new;
    Tcl_HashEntry *hPtr;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key string ?string ...?");
        return TCL_ERROR;
    }
    arrayPtr = LockArray(arg, interp, objv[1], 1);
    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &new);
    if (!new) {
        Tcl_SetResult(interp, Tcl_GetHashValue(hPtr), TCL_VOLATILE);
    }
    for (i = 3; i < objc; ++i) {
        Tcl_AppendResult(interp, Tcl_GetString(objv[i]), NULL);
    }
    UpdateVar(hPtr, Tcl_GetObjResult(interp));
    NsNsvUnlockArray(arrayPtr);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvArrayObjCmd --
 *
 *      Implements nsv_array as an obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvArrayObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsArray          *arrayPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    char           *pattern, *key;
    int             i, opt, lobjc, size;
    Tcl_Obj       **lobjv;

    static CONST char *opts[] = {
        "set", "reset", "get", "names", "size", "exists", NULL
    };
    enum ISubCmdIdx {
        CSetIdx, CResetIdx, CGetIdx, CNamesIdx, CSizeIdx, CExistsIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ...");
        return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }
    switch (opt) {
    case CSetIdx:
    case CResetIdx:
        if (objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "array valueList");
            return TCL_ERROR;
        }
        if (Tcl_ListObjGetElements(interp, objv[3], &lobjc,
                                   &lobjv) != TCL_OK) {
            return TCL_ERROR;
        }
        if (lobjc & 1) {
            Tcl_AppendResult(interp, "invalid list: ",
                             Tcl_GetString(objv[3]), NULL);
            return TCL_ERROR;
        }
        arrayPtr = LockArray(arg, interp, objv[2], 1);
        if (opt == CResetIdx) {
            NsNsvFlushArray(arrayPtr);
        }
        for (i = 0; i < lobjc; i += 2) {
            SetVar(arrayPtr, lobjv[i], lobjv[i+1]);
        }
        NsNsvUnlockArray(arrayPtr);
        break;

    case CSizeIdx:
    case CExistsIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "array");
            return TCL_ERROR;
        }
        arrayPtr = LockArray(arg, NULL, objv[2], 0);
        if (arrayPtr == NULL) {
            size = 0;
        } else {
            size = (opt == CSizeIdx) ? arrayPtr->vars.numEntries : 1;
            NsNsvUnlockArray(arrayPtr);
        }
        if (opt == CExistsIdx) {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(size));
        } else {
            Tcl_SetObjResult(interp, Tcl_NewIntObj(size));
        }
        break;

    case CGetIdx:
    case CNamesIdx:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "array ?pattern?");
            return TCL_ERROR;
        }
        arrayPtr = LockArray(arg, NULL, objv[2], 0);
        if (arrayPtr != NULL) {
            pattern = (objc > 3) ? Tcl_GetString(objv[3]) : NULL;
            hPtr = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
            while (hPtr != NULL) {
                key = Tcl_GetHashKey(&arrayPtr->vars, hPtr);
                if (pattern == NULL || Tcl_StringMatch(key, pattern)) {
                    Tcl_AppendElement(interp, key);
                    if (opt == CGetIdx) {
                        Tcl_AppendElement(interp, Tcl_GetHashValue(hPtr));
                    }
                }
                hPtr = Tcl_NextHashEntry(&search);
            }
            NsNsvUnlockArray(arrayPtr);
        }
        break;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvUnsetObjCmd --
 *
 *      Implements nsv_unset as an obj command. 
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      See docs. 
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvUnsetObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsArray         *arrayPtr = NULL;
    Tcl_Obj       *arrayObj;
    Tcl_HashEntry *hPtr = NULL;
    char          *key = NULL;
    int            nocomplain = 0;

    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool, &nocomplain, (void *) NS_TRUE},
        {"--",         Ns_ObjvBreak, NULL,   NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"array", Ns_ObjvObj,    &arrayObj, NULL},
        {"?key",  Ns_ObjvString, &key,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    arrayPtr = LockArray(arg, interp, arrayObj, 0);
    if (arrayPtr == NULL) {
        if (nocomplain) {
            Tcl_ResetResult(interp);
            return TCL_OK;
        }
        return TCL_ERROR;
    }
    if (key == NULL) {
        Tcl_DeleteHashEntry(arrayPtr->entryPtr);
    } else {
        hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
        if (hPtr != NULL) {
            ns_free(Tcl_GetHashValue(hPtr));
            Tcl_DeleteHashEntry(hPtr);
        }
    }
    NsNsvUnlockArray(arrayPtr);
    if (key == NULL) {
        NsNsvFlushArray(arrayPtr);
        Tcl_DeleteHashTable(&arrayPtr->vars);
        ns_free(arrayPtr);
    } else if (hPtr == NULL && !nocomplain) {
        Tcl_AppendResult(interp, "no such key: ", Tcl_GetString(objv[2]), NULL);
        return TCL_ERROR;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclNsvNamesObjCmd --
 *
 *      Implements nsv_names as an obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclNsvNamesObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
{
    NsInterp       *itPtr = arg;
    NsServer       *servPtr = itPtr->servPtr;
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;
    Tcl_Obj        *result;
    NsBucket       *bucketPtr;
    char           *pattern, *key;
    int             i;
    
    if (objc != 1 && objc !=2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?pattern?");
        return TCL_ERROR;
    }
    pattern = objc < 2 ? NULL : Tcl_GetString(objv[1]);

    /* 
     * Walk the bucket list for each array.
     */

    result = Tcl_GetObjResult(interp);
    for (i = 0; i < servPtr->nsv.nbuckets; i++) {
        bucketPtr = &servPtr->nsv.buckets[i];
        Ns_MutexLock(&bucketPtr->lock);
        hPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
        while (hPtr != NULL) {
            key = Tcl_GetHashKey(&bucketPtr->arrays, hPtr);
            if (pattern == NULL || Tcl_StringMatch(key, pattern)) {
                Tcl_ListObjAppendElement(NULL, result,
                                         Tcl_NewStringObj(key, -1));
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&bucketPtr->lock);
    }

    return TCL_OK;
}


static NsArray *
LockArray(void *arg, Tcl_Interp *interp, Tcl_Obj *arrayObj, int create)
{
    NsInterp *itPtr = arg;
    NsArray    *arrayPtr;

    arrayPtr = NsNsvLockArray(itPtr->servPtr, Tcl_GetString(arrayObj), create);
    if (arrayPtr == NULL && interp != NULL) {
        Tcl_AppendResult(interp, "no such array: ", Tcl_GetString(arrayObj), NULL);
    }
    return arrayPtr;
}


/*
 *----------------------------------------------------------------
 *
 * UpdateVar --
 *
 *      Update a variable entry.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      New value is set.
 *
 *----------------------------------------------------------------
 */

static void
UpdateVar(Tcl_HashEntry *hPtr, Tcl_Obj *obj)
{
    char *str, *old, *new;
    int   len;

    str = Tcl_GetStringFromObj(obj, &len);
    old = Tcl_GetHashValue(hPtr);
    new = ns_realloc(old, (size_t)(len+1));
    memcpy(new, str, (size_t)(len+1));
    Tcl_SetHashValue(hPtr, new);
}


/*
 *----------------------------------------------------------------
 *
 * SetVar --
 *
 *      Set (or reset) an array entry.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      New entry is created and updated.
 *
 *----------------------------------------------------------------
 */

static void
SetVar(NsArray *arrayPtr, Tcl_Obj *key, Tcl_Obj *value)
{
    Tcl_HashEntry *hPtr;
    int            new;

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(key), &new);
    UpdateVar(hPtr, value);
}


/*
 *----------------------------------------------------------------
 *
 * LockArray --
 *
 *      Find (or create) the Array structure for an array and
 *      lock it.  Array structure must be later unlocked with
 *      NsNsvUnlockArray.
 *
 * Results:
 *      TCL_OK or TCL_ERROR if no such array.
 *
 * Side effects;
 *      Sets *arrayPtrPtr with Array pointer or leave error in
 *      given Tcl_Interp.
 *
 *----------------------------------------------------------------
 */

NsArray *
NsNsvLockArray(NsServer *servPtr, char *aname, int create)
{
    NsBucket       *bucketPtr;
    Tcl_HashEntry *hPtr;
    NsArray         *arrayPtr;
    register char *p;
    register unsigned int result;
    register int i;
    int new;
   
    p = aname;
    result = 0;
    while (1) {
        i = *p;
        p++;
        if (i == 0) {
            break;
        }
        result += (result<<3) + i;
    }
    i = result % servPtr->nsv.nbuckets;
    bucketPtr = &servPtr->nsv.buckets[i];

    Ns_MutexLock(&bucketPtr->lock);
    if (create) {
        hPtr = Tcl_CreateHashEntry(&bucketPtr->arrays, aname, &new);
        if (!new) {
            arrayPtr = Tcl_GetHashValue(hPtr);
        } else {
            arrayPtr = ns_malloc(sizeof(NsArray));
            arrayPtr->bucketPtr = bucketPtr;
            arrayPtr->entryPtr = hPtr;
            Tcl_InitHashTable(&arrayPtr->vars, TCL_STRING_KEYS);
            Tcl_SetHashValue(hPtr, arrayPtr);
        }
    } else {
        hPtr = Tcl_FindHashEntry(&bucketPtr->arrays, aname);
        if (hPtr == NULL) {
            Ns_MutexUnlock(&bucketPtr->lock);
            return NULL;
        }
        arrayPtr = Tcl_GetHashValue(hPtr);
    }

    return arrayPtr;
}

void
NsNsvUnlockArray(NsArray *arrayPtr)
{
    if (arrayPtr != NULL) {
        Ns_MutexUnlock(&((arrayPtr)->bucketPtr->lock));
    }
}

/*
 *----------------------------------------------------------------
 *
 * NsNsvFlushArray --
 *
 *      Unset all keys in an array.
 *
 * Results:
 *      None.
 *
 * Side effects;
 *      New entry is created and updated.
 *
 *----------------------------------------------------------------
 */

void
NsNsvFlushArray(NsArray *arrayPtr)
{
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    hPtr = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
    while (hPtr != NULL) {
        ns_free(Tcl_GetHashValue(hPtr));
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsNsvGet
 *
 *      Returns newly allocated variable value or NULL if not found or array does not exists
 *
 * Results:
 *      Allocated string
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
NsNsvGet(NsServer *servPtr, char *aname, char *key)
{
    NsArray *arrayPtr;
    char *value = NULL;
    Tcl_HashEntry *hPtr;

    if (servPtr == NULL || aname == NULL || key == NULL) {
        return NULL;
    }
    arrayPtr = NsNsvLockArray(servPtr, aname, 0);
    if (arrayPtr == NULL) {
        return NULL;
    }
    hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
    if (hPtr != NULL) {
        value = ns_strcopy(Tcl_GetHashValue(hPtr));
    }
    NsNsvUnlockArray(arrayPtr);
    return value;
}

/*
 *----------------------------------------------------------------------
 *
 * NsNsvExists
 *
 * Results:
 *      Returns 1 if key exists in the given array, otherwise 0
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsNsvExists(NsServer *servPtr, char *aname, char *key)
{
    int    exists = 0;
    NsArray *arrayPtr;

    if (servPtr == NULL || aname == NULL || key == NULL) {
        return 0;
    }
    arrayPtr = NsNsvLockArray(servPtr, aname, 0);
    if (arrayPtr != NULL) {
        if (Tcl_FindHashEntry(&arrayPtr->vars, key) != NULL) {
            exists = 1;
        }
        NsNsvUnlockArray(arrayPtr);
    }
    return exists;
}

/*
 *----------------------------------------------------------------------
 *
 * NsNsvSet
 *
 *      Assign new value to the key in the given array
 *
 * Results:
 *      Returns NS_OK if set, NS_ERROR if array does not exists
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsNsvSet(NsServer *servPtr, char *aname, char *key, char *value)
{
    int new, len;
    NsArray *arrayPtr;
    char *ostr, *nstr;
    Tcl_HashEntry *hPtr;

    if (servPtr == NULL || aname == NULL || key == NULL || value == NULL) {
        return NS_ERROR;
    }
    arrayPtr = NsNsvLockArray(servPtr, aname, 0);
    if (arrayPtr != NULL) {
        len = strlen(value);
        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &new);
        ostr = Tcl_GetHashValue(hPtr);
        nstr = ns_realloc(ostr, len + 1);
        memcpy(nstr, value, len + 1);
        Tcl_SetHashValue(hPtr, nstr);
        NsNsvUnlockArray(arrayPtr);
        return NS_OK;
    }
    return NS_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * NsNsvIncr
 *
 *      Increases value of the counter in the given array, if
 *      key does not exists, it is created and set to 1
 *
 * Results:
 *      Returns new value of the counter
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsNsvIncr(NsServer *servPtr, char *aname, char *key, int count)
{
    NsArray *arrayPtr;
    Tcl_HashEntry *hPtr;
    int new, value = 0;
    char num[32], *ostr, *nstr;

    if (servPtr == NULL || aname == NULL || key == NULL) {
        return NS_ERROR;
    }
    arrayPtr = NsNsvLockArray(servPtr, aname, 0);
    if (arrayPtr != NULL) {
        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &new);
        ostr = Tcl_GetHashValue(hPtr);
        if (new == 0) {
            value = atoi(ostr);
        }
        sprintf(num, "%d", value + count);
        nstr = ns_realloc(ostr, strlen(num) + 1);
        memcpy(nstr, num, strlen(num) + 1);
        Tcl_SetHashValue(hPtr, nstr);
        NsNsvUnlockArray(arrayPtr);
    }
    return value;
}

/*
 *----------------------------------------------------------------------
 *
 * NsNsvAppend
 *
 *      Append list of strings to the key, creates new if does not exists,
 *      last value must be NULL
 *
 * Results:
 *      Returns NS_OK if assigned, NS_ERROR if array is not found
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsNsvAppendVA(NsServer *servPtr, char *aname, char *key, va_list ap)
{
    int new, len;
    NsArray *arrayPtr;
    Tcl_HashEntry *hPtr;
    char *ostr, *nstr, *value;

    if (servPtr == NULL || aname == NULL || key == NULL) {
        return NS_ERROR;
    }
    arrayPtr = NsNsvLockArray(servPtr, aname, 0);
    if (arrayPtr != NULL) {
        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &new);
        ostr = nstr = Tcl_GetHashValue(hPtr);

        while (1) {
            value = va_arg(ap, char*);
            if (value == NULL) {
                break;
            }
            len = strlen(value);
            nstr = ns_realloc(ostr, len + 1);
            memcpy(nstr, value, len + 1);
            ostr = nstr;
        }

        Tcl_SetHashValue(hPtr, nstr);
        NsNsvUnlockArray(arrayPtr);
        return NS_OK;
    }
    return NS_ERROR;
}

int NsNsvAppend(NsServer *servPtr, char *aname, char *key, ...)
{
    int rc;
    va_list ap;

    va_start(ap, key);
    rc = NsNsvAppendVA(servPtr, aname, key, ap);
    va_end(ap);
    return rc;
}

/*
 *----------------------------------------------------------------------
 *
 * NsNsvUnset
 *
 *      Resets given key inthe array, if key is NULL, flushes the whole array
 *
 * Results:
 *      Returns NS_OK if flushed, NS_ERROR if array not found
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsNsvUnset(NsServer *servPtr, char *aname, char *key)
{
    NsArray *arrayPtr;
    Tcl_HashEntry *hPtr;

    if (servPtr == NULL || aname == NULL) {
        return NS_ERROR;
    }
    arrayPtr = NsNsvLockArray(servPtr, aname, 0);
    if (arrayPtr != NULL) {
        if (key == NULL) {
            Tcl_DeleteHashEntry(arrayPtr->entryPtr);
        } else {
            hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
            if (hPtr != NULL) {
                ns_free(Tcl_GetHashValue(hPtr));
                Tcl_DeleteHashEntry(hPtr);
            }
        }
        NsNsvUnlockArray(arrayPtr);

        if (key == NULL) {
            NsNsvFlushArray(arrayPtr);
            Tcl_DeleteHashTable(&arrayPtr->vars);
            ns_free(arrayPtr);
        }
        return NS_OK;
    }
    return NS_ERROR;
}
