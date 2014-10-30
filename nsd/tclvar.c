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

/*
 * The following structure defines a collection of arrays.
 * Only the arrays within a given bucket share a lock,
 * allowing for more concurency in nsv.
 */

typedef struct Bucket {
    Ns_Mutex      lock;
    Tcl_HashTable arrays;
} Bucket;

/*
 * The following structure maintains the context for each
 * variable array.
 */

typedef struct Array {
    Bucket        *bucketPtr; /* Array bucket. */
    Tcl_HashEntry *entryPtr;  /* Entry in bucket array table. */
    Tcl_HashTable  vars;      /* Table of variables. */
    long           locks;     /* Number of array locks */
} Array;

/*
 * Local functions defined in this file.
 */

static void SetVar(Array *arrayPtr, CONST char *key, CONST char *value, size_t len);
static void UpdateVar(Tcl_HashEntry *hPtr, CONST char *value, size_t len);
static int IncrVar(Array *arrayPtr, CONST char *key, int incr,
                   Tcl_WideInt *valuePtr);

static int Unset(Array *arrayPtr, CONST char *key);
static void Flush(Array *arrayPtr);

static Array *LockArray(const NsServer *servPtr, const char *array, int create);
static void UnlockArray(const Array *arrayPtr);

static Array *LockArrayObj(Tcl_Interp *interp, Tcl_Obj *arrayObj, int create);
static unsigned int BucketIndex(const char* arrayName) NS_GNUC_NONNULL(1);

/*
 *----------------------------------------------------------------------
 *
 * NsTclCreateBuckets --
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

Bucket *
NsTclCreateBuckets(CONST char *server, int nbuckets)
{
    char    buf[NS_THREAD_NAMESIZE];
    Bucket *buckets;

    buckets = ns_malloc(sizeof(Bucket) * nbuckets);
    while (--nbuckets >= 0) {
        snprintf(buf, sizeof(buf), "nsv:%d", nbuckets);
        Tcl_InitHashTable(&buckets[nbuckets].arrays, TCL_STRING_KEYS);
        Ns_MutexInit(&buckets[nbuckets].lock);
        Ns_MutexSetName2(&buckets[nbuckets].lock, buf, server);
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
NsTclNsvGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Array         *arrayPtr;
    Tcl_HashEntry *hPtr;
    Tcl_Obj       *resultObj;

    if (unlikely(objc < 3 || objc > 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?varName?");
        return TCL_ERROR;
    }
    if (unlikely((arrayPtr = LockArrayObj(interp, objv[1], 0)) == NULL)) {
        return TCL_ERROR;
    }

    hPtr = Tcl_FindHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]));
    resultObj = likely(hPtr != NULL) ? Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1) : NULL;
    UnlockArray(arrayPtr);

    if (objc == 3) {
	if (resultObj) {
	    Tcl_SetObjResult(interp, resultObj);
	} else {
	    Tcl_AppendResult(interp, "no such key: ",
			     Tcl_GetString(objv[2]), NULL);
	    return TCL_ERROR;
	}
    } else {
	Tcl_SetObjResult(interp, Tcl_NewIntObj(resultObj != NULL));
	if (resultObj) {
	    Tcl_ObjSetVar2(interp, objv[3], NULL, resultObj, 0);
	}
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
NsTclNsvExistsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Array *arrayPtr;
    int    exists = 0;

    if (unlikely(objc != 3)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key");
        return TCL_ERROR;
    }
    if (unlikely((arrayPtr = LockArrayObj(interp, objv[1], 0)) != NULL)) {
        if (Tcl_FindHashEntry(&arrayPtr->vars,
                              Tcl_GetString(objv[2])) != NULL) {
            exists = 1;
        }
        UnlockArray(arrayPtr);
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
NsTclNsvSetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Array         *arrayPtr;
    char          *key;
    int            len, result = TCL_OK;

    if (unlikely(objc != 3 && objc != 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?value?");
        return TCL_ERROR;
    }
    key = Tcl_GetString(objv[2]);

    if (likely(objc == 4)) {
        char *value = Tcl_GetStringFromObj(objv[3], &len);

        arrayPtr = LockArrayObj(interp, objv[1], 1);
	assert(arrayPtr != NULL);
        SetVar(arrayPtr, key, value, (size_t)len);
        UnlockArray(arrayPtr);

        Tcl_SetObjResult(interp, objv[3]);
    } else {
        Tcl_HashEntry *hPtr;

        if ((arrayPtr = LockArrayObj(interp, objv[1], 0)) == NULL) {
            return TCL_ERROR;
        }
        if ((hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key)) != NULL) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
        } else {
            Tcl_AppendResult(interp, "no such key: ", key, NULL);
            result = TCL_ERROR;
        }
        UnlockArray(arrayPtr);
    }

    return result;
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
NsTclNsvIncrObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Array         *arrayPtr;
    Tcl_WideInt    current;
    int            result, count = 1;

    if (unlikely(objc != 3 && objc != 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?count?");
        return TCL_ERROR;
    }
    if (unlikely(objc == 4 && Tcl_GetIntFromObj(interp, objv[3], &count) != TCL_OK)) {
        return TCL_ERROR;
    }
    arrayPtr = LockArrayObj(interp, objv[1], 1);
    assert(arrayPtr != NULL);
    result = IncrVar(arrayPtr, Tcl_GetString(objv[2]), count, &current);
    UnlockArray(arrayPtr);

    if (likely(result == TCL_OK)) {
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(current));
    } else {
        Tcl_AppendResult(interp, "array variable is not an integer", NULL);
    }

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
NsTclNsvLappendObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Array         *arrayPtr;
    Tcl_HashEntry *hPtr;
    char          *value;
    int            isNew, len;

    if (unlikely(objc < 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key value ?value ...?");
        return TCL_ERROR;
    }
    arrayPtr = LockArrayObj(interp, objv[1], 1);

    assert(arrayPtr != NULL);

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &isNew);
    if (unlikely(isNew != 0)) {
        Tcl_SetListObj(Tcl_GetObjResult(interp), objc-3, objv+3);
    } else {
        int i;

        Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
        for (i = 3; i < objc; ++i) {
            Tcl_AppendElement(interp, Tcl_GetString(objv[i]));
        }
    }
    value = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &len);
    UpdateVar(hPtr, value, (size_t)len);
    UnlockArray(arrayPtr);

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
NsTclNsvAppendObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Array         *arrayPtr;
    Tcl_HashEntry *hPtr;
    char          *value;
    int            i, isNew, len;

    if (unlikely(objc < 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key value ?value ...?");
        return TCL_ERROR;
    }
    arrayPtr = LockArrayObj(interp, objv[1], 1);
    assert(arrayPtr != NULL);

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &isNew);
    if (isNew == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
    }
    for (i = 3; i < objc; ++i) {
        Tcl_AppendResult(interp, Tcl_GetString(objv[i]), NULL);
    }
    value = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &len);
    UpdateVar(hPtr, value, len);
    UnlockArray(arrayPtr);

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
NsTclNsvUnsetObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp *itPtr = clientData;
     Tcl_Obj  *arrayObj;
    Array    *arrayPtr;
    char     *key = NULL;
    int       nocomplain = 0, result = TCL_OK;

    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool,  &nocomplain, INT2PTR(1)},
        {"--",          Ns_ObjvBreak, NULL,        NULL},
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

    if ((arrayPtr = LockArrayObj(interp, arrayObj, 0)) == NULL) {
        if (nocomplain) {
            Tcl_ResetResult(interp);
            return TCL_OK;
        }
        return TCL_ERROR;
    }

    if (Unset(arrayPtr, key) != NS_OK && key != NULL) {
        Tcl_AppendResult(interp, "no such key: ", key, NULL);
        result = TCL_ERROR;
    }
    UnlockArray(arrayPtr);

    /*
     * If everything went well, delete the array entry, free the hash
     * table and invalidate the Tcl_Obj.
     */
    if (result == TCL_OK && key == NULL) {
	NsServer       *servPtr = itPtr->servPtr;
	Bucket         *bucketPtr;
	const char     *arrayString = Tcl_GetString(arrayObj);
	unsigned int    index = BucketIndex(arrayString);
	Tcl_HashEntry  *hPtr;

	bucketPtr = &servPtr->nsv.buckets[index % (unsigned int)servPtr->nsv.nbuckets];

	Ns_MutexLock(&bucketPtr->lock);
	hPtr = Tcl_FindHashEntry(&bucketPtr->arrays, arrayString);
	    
	if (hPtr != NULL) {
	    Tcl_DeleteHashTable(&arrayPtr->vars);
	    ns_free(arrayPtr);
	    Tcl_DeleteHashEntry(hPtr);
	    Ns_TclSetTwoPtrValue(arrayObj, NULL, NULL, NULL);
	}
	Ns_MutexUnlock(&bucketPtr->lock);
    }

    return result;
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
NsTclNsvNamesObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp       *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    Tcl_HashSearch  search;
    Tcl_Obj        *result;
    Bucket         *bucketPtr;
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
        Tcl_HashEntry  *hPtr;

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
NsTclNsvArrayObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Array          *arrayPtr;
    Tcl_HashSearch  search;
    int             i, opt, lobjc, size;
    Tcl_Obj       **lobjv;

    static const char *opts[] = {
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
        if (lobjc % 2 == 1) {
            Tcl_AppendResult(interp, "invalid list: ",
                             Tcl_GetString(objv[3]), NULL);
            return TCL_ERROR;
        }

        arrayPtr = LockArrayObj(interp, objv[2], 1);
	assert(arrayPtr != NULL);
	
        if (opt == CResetIdx) {
            Flush(arrayPtr);
        }
        for (i = 0; i < lobjc; i += 2) {
            char *value = Tcl_GetStringFromObj(lobjv[i+1], &size);

            SetVar(arrayPtr, Tcl_GetString(lobjv[i]), value, (size_t)size);
        }
        UnlockArray(arrayPtr);
        break;

    case CSizeIdx:
    case CExistsIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "array");
            return TCL_ERROR;
        }
        arrayPtr = LockArrayObj(interp, objv[2], 0);
        if (arrayPtr == NULL) {
            size = 0;
        } else {
            size = (opt == CSizeIdx) ? arrayPtr->vars.numEntries : 1;
            UnlockArray(arrayPtr);
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
        arrayPtr = LockArrayObj(interp, objv[2], 0);
        Tcl_ResetResult(interp);
        if (arrayPtr != NULL) {
	    Tcl_HashEntry  *hPtr    = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
	    char           *pattern = (objc > 3) ? Tcl_GetString(objv[3]) : NULL;

            while (hPtr != NULL) {
                char *key = Tcl_GetHashKey(&arrayPtr->vars, hPtr);

                if (pattern == NULL || Tcl_StringMatch(key, pattern)) {
                    Tcl_AppendElement(interp, key);
                    if (opt == CGetIdx) {
                        Tcl_AppendElement(interp, Tcl_GetHashValue(hPtr));
                    }
                }
                hPtr = Tcl_NextHashEntry(&search);
            }
            UnlockArray(arrayPtr);
        }
        break;
    }

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VarGet
 *
 *      Append array value to dstring.
 *
 * Results:
 *      Pointer to dstring on success, NULL if server, array or key do
 *      do not exist.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_VarGet(CONST char *server, CONST char *array, CONST char *key,
          Ns_DString *dsPtr)
{
    NsServer      *servPtr;
    Array         *arrayPtr;
    int            status = NS_ERROR;

    if ((servPtr = NsGetServer(server)) != NULL
        && (arrayPtr = LockArray(servPtr, array, 0)) != NULL) {
        Tcl_HashEntry *hPtr;

        if ((hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key)) != NULL) {
            Ns_DStringAppend(dsPtr, Tcl_GetHashValue(hPtr));
            status = NS_OK;
        }
        UnlockArray(arrayPtr);
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VarExists
 *
 *      Return 1 if the key exists int the given array.
 *
 * Results:
 *      1 or 0.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_VarExists(CONST char *server, CONST char *array, CONST char *key)
{
    NsServer *servPtr;
    Array    *arrayPtr;
    int       exists = 0;

    if ((servPtr = NsGetServer(server)) != NULL
        && (arrayPtr = LockArray(servPtr, array, 0)) != NULL) {

        if (Tcl_FindHashEntry(&arrayPtr->vars, key) != NULL) {
            exists = 1;
        }
        UnlockArray(arrayPtr);
    }
    return exists;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VarSet
 *
 *      Assign new value to the key in the given array.
 *
 * Results:
 *      Returns NS_OK if set, NS_ERROR if server does not exist.
 *
 * Side effects:
 *      Clobbers any existing value under this key.
 *
 *----------------------------------------------------------------------
 */

int
Ns_VarSet(CONST char *server, CONST char *array, CONST char *key,
          CONST char *value, ssize_t len)
{
    NsServer      *servPtr;
    Array         *arrayPtr;
    int            status = NS_ERROR;

    if ((servPtr = NsGetServer(server)) != NULL
        && (arrayPtr = LockArray(servPtr, array, 1)) != NULL) {

        SetVar(arrayPtr, key, value, len > -1 ? len : strlen(value));
        UnlockArray(arrayPtr);
        status = NS_OK;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VarIncr
 *
 *      Increment the value of the value (up or down) in the given array.
 *
 * Results:
 *      The new value of the counter.
 *
 * Side effects:
 *      Missing keys are initialised to 1.
 *
 *----------------------------------------------------------------------
 */

Tcl_WideInt
Ns_VarIncr(CONST char *server, CONST char *array, CONST char *key, int incr)
{
    NsServer      *servPtr;
    Array         *arrayPtr;
    Tcl_WideInt    counter = -1;

    if ((servPtr = NsGetServer(server)) != NULL
        && (arrayPtr = LockArray(servPtr, array, 1)) != NULL) {

        (void) IncrVar(arrayPtr, key, incr, &counter);
        UnlockArray(arrayPtr);
    }
    return counter;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VarAppend
 *
 *      Append string to existing array value.
 *
 * Results:
 *      NS_OK if assigned, NS_ERROR if array is found.
 *
 * Side effects:
 *      Array and value are created if they do not already exist.
 *
 *----------------------------------------------------------------------
 */

int
Ns_VarAppend(CONST char *server, CONST char *array, CONST char *key,
             CONST char *value, ssize_t len)
{
    NsServer      *servPtr;
    Array         *arrayPtr;
    char          *oldString;
    int            isNew, status = NS_ERROR;

    if ((servPtr = NsGetServer(server)) != NULL
        && (arrayPtr = LockArray(servPtr, array, 1)) != NULL) {
	Tcl_HashEntry *hPtr;
        size_t         oldLen, newLen;
	char          *newString;

        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &isNew);

        oldString = Tcl_GetHashValue(hPtr);
        oldLen = oldString ? strlen(oldString) : 0U;

        newLen = oldLen + (len > -1 ? len : strlen(value)) + 1;
        newString = ns_realloc(oldString, newLen + 1);
        memcpy(newString + oldLen, value, newLen + 1);

        Tcl_SetHashValue(hPtr, newString);

        UnlockArray(arrayPtr);
        status = NS_OK;
    }
    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_VarUnset
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
Ns_VarUnset(CONST char *server, CONST char *array, CONST char *key)
{
    NsServer      *servPtr;
    Array         *arrayPtr;
    int            status = NS_ERROR;

    if ((servPtr = NsGetServer(server)) != NULL
        && (arrayPtr = LockArray(servPtr, array, 0)) != NULL) {

        status = Unset(arrayPtr, key);
        UnlockArray(arrayPtr);
    }
    return status;
}


/*
 *----------------------------------------------------------------
 *
 * LockArray, UnlockArray --
 *
 *      Lock the array of the given name.
 *      Array structure must be later unlocked with UnlockArray.
 *
 * Results:
 *      Pointer to Array or NULL.
 *
 * Side effects;
 *      Array is created if 'create' is 1.
 *
 *----------------------------------------------------------------
 */

static unsigned int
BucketIndex(const char* arrayName) {
    unsigned int index = 0U;

    while (1) {
	register int i = *(arrayName++);
	if (unlikely(i == 0)) {
            break;
        }
        index += (index << 3U) + i;
    }
    return index;
}


/*
 *----------------------------------------------------------------
 *
 * LockArray, UnlockArray --
 *
 *      Lock the array of the given name.
 *      Array structure must be later unlocked with UnlockArray.
 *
 * Results:
 *      Pointer to Array or NULL.
 *
 * Side effects;
 *      Array is created if 'create' is 1.
 *
 *----------------------------------------------------------------
 */

static Array *
LockArray(const NsServer *servPtr, const char *array, int create)
{
    Bucket        *bucketPtr;
    Tcl_HashEntry *hPtr;
    Array         *arrayPtr;
    unsigned int   index;
    int            isNew;

    index = BucketIndex(array);
    bucketPtr = &servPtr->nsv.buckets[index % (unsigned int)servPtr->nsv.nbuckets];

    Ns_MutexLock(&bucketPtr->lock);
    if (unlikely(create)) {
        hPtr = Tcl_CreateHashEntry(&bucketPtr->arrays, array, &isNew);
        if (isNew == 0) {
            arrayPtr = Tcl_GetHashValue(hPtr);
        } else {
            arrayPtr = ns_malloc(sizeof(Array));
	    arrayPtr->locks = 0;
            arrayPtr->bucketPtr = bucketPtr;
            arrayPtr->entryPtr = hPtr;
            Tcl_InitHashTable(&arrayPtr->vars, TCL_STRING_KEYS);
            Tcl_SetHashValue(hPtr, arrayPtr);
        }
    } else {
        hPtr = Tcl_FindHashEntry(&bucketPtr->arrays, array);
        if (unlikely(hPtr == NULL)) {
            Ns_MutexUnlock(&bucketPtr->lock);
            return NULL;
        }
        arrayPtr = Tcl_GetHashValue(hPtr);
    }
    arrayPtr->locks++;

    return arrayPtr;
}

static void
UnlockArray(const Array *arrayPtr)
{
    if (likely(arrayPtr != NULL)) {
        Ns_MutexUnlock(&((arrayPtr)->bucketPtr->lock));
    }
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
UpdateVar(Tcl_HashEntry *hPtr, CONST char *value, size_t len)
{
    char *oldString, *newString;

    oldString = Tcl_GetHashValue(hPtr);
    newString = ns_realloc(oldString, (size_t) (len + 1));
    memcpy(newString, value, (size_t) (len + 1));
    Tcl_SetHashValue(hPtr, newString);
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
SetVar(Array *arrayPtr, CONST char *key, CONST char *value, size_t len)
{
    Tcl_HashEntry *hPtr;
    int            isNew;

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &isNew);
    UpdateVar(hPtr, value, len);
}


/*
 *----------------------------------------------------------------
 *
 * IncrVar --
 *
 *      Increment the value of the variable.
 *
 * Results:
 *      TCL_OK, or TCL_ERROR if existing value is not an integer.
 *      The new value is returned in valuePtr.
 *
 * Side effects;
 *      New entry is created and updated.
 *
 *----------------------------------------------------------------
 */

static int
IncrVar(Array *arrayPtr, CONST char *key, int incr,
        Tcl_WideInt *valuePtr)
{
    Tcl_HashEntry *hPtr;
    CONST char    *oldString;
    int            isNew, status;
    Tcl_WideInt    counter = -1;

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &isNew);
    oldString = Tcl_GetHashValue(hPtr);

    if (isNew != 0) {
        counter = 0;
        status = TCL_OK;
    } else {
        status = (Ns_StrToWideInt(oldString, &counter) == NS_OK)
            ? TCL_OK : TCL_ERROR;
    }

    if (status == TCL_OK) {
        char buf[TCL_INTEGER_SPACE+2];

        counter += incr;
        snprintf(buf, sizeof(buf), "%" TCL_LL_MODIFIER "d", counter);
        UpdateVar(hPtr, buf, strlen(buf));
    }
    *valuePtr = counter;

    return status;
}


/*
 *----------------------------------------------------------------
 *
 * Unset --
 *
 *      Unset the given key. If no key given flush the entire array.
 *
 * Results:
 *      NS_ERROR if key requested but not present.
 *
 * Side effects;
 *      None.
 *
 *----------------------------------------------------------------
 */

static int
Unset(Array *arrayPtr, CONST char *key)
{
    int            status = NS_ERROR;

    if (key != NULL) {
        Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);

        if (hPtr != NULL) {
            ns_free(Tcl_GetHashValue(hPtr));
            Tcl_DeleteHashEntry(hPtr);
            status = NS_OK;
        }
    } else {
        Flush(arrayPtr);
        status = NS_OK;
    }

    return status;
}


/*
 *----------------------------------------------------------------
 *
 * Flush --
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

static void
Flush(Array *arrayPtr)
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
 *----------------------------------------------------------------
 *
 * LockArrayObj --
 *
 *      Lock the array of the given name.
 *      Array structure must be later unlocked with UnlockArray.
 *
 * Results:
 *      Pointer to locked array or NULL.
 *
 * Side effects;
 *      Array is created if 'create' is true.
 *
 *----------------------------------------------------------------
 */

static Array *
LockArrayObj(Tcl_Interp *interp, Tcl_Obj *arrayObj, int create)
{
    Array              *arrayPtr = NULL;
    static const char  *arrayType = "nsv:array";

    if (likely(Ns_TclGetOpaqueFromObj(arrayObj, arrayType, (void **) &arrayPtr)
	       == TCL_OK)) {
        Ns_MutexLock(&((arrayPtr)->bucketPtr->lock));
	arrayPtr->locks++;
    } else {
        NsInterp *itPtr = NsGetInterpData(interp);

        arrayPtr = LockArray(itPtr->servPtr, Tcl_GetString(arrayObj), create);
        if (arrayPtr != NULL) {
            Ns_TclSetOpaqueObj(arrayObj, arrayType, arrayPtr);
        } else if (!create) {
            Tcl_AppendResult(interp, "no such array: ",
                             Tcl_GetString(arrayObj), NULL);
        }
    }

    return arrayPtr;
}



/*
 *----------------------------------------------------------------
 *
 * NsTclNsvBucketObjCmd --
 *
 *      TclObjCommand to return the names of the arrays kept in
 *      various buckets of the current interp.  If called a bucket
 *      number it returns a list array kept in that bucket. If called
 *      with no arguments, it returns a list of every bucket (list of
 *      lists).
 *
 * Results:
 *      Tcl result code
 *
 * Side effects;
 *      None.
 *
 *----------------------------------------------------------------
 */

int
NsTclNsvBucketObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp       *itPtr = clientData;
    NsServer       *servPtr = itPtr->servPtr;
    int		    bucketNr = -1, i;
    Bucket         *bucketPtr;
    Tcl_Obj        *resultObj, *listObj;

    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?bucket-number?");
	return TCL_ERROR;
    }
    if (objc == 2 && 
	(Tcl_GetIntFromObj(interp, objv[1], &bucketNr) != TCL_OK 
	 || bucketNr < 0 
	 || bucketNr >= servPtr->nsv.nbuckets
	 )) {
        Tcl_ResetResult(interp);
        Tcl_AppendResult(interp, "bucket number is not a valid integer", NULL);
        return TCL_ERROR;
    }

    /* LOCK for servPtr->nsv ? */
    resultObj = Tcl_GetObjResult(interp);
    for (i = 0; i < servPtr->nsv.nbuckets; i++) {
        Tcl_HashEntry  *hPtr;
	Tcl_HashSearch  search;

        if (bucketNr > -1 && i != bucketNr) {
	    continue;
        }
	listObj = Tcl_NewListObj(0, NULL);
        bucketPtr = &servPtr->nsv.buckets[i];
        Ns_MutexLock(&bucketPtr->lock);
        hPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
        while (hPtr != NULL) {
	    CONST char *key  = Tcl_GetHashKey(&bucketPtr->arrays, hPtr);
	    Array *arrayPtr  = Tcl_GetHashValue(hPtr);
	    Tcl_Obj *elemObj = Tcl_NewListObj(0, NULL);

	    Tcl_ListObjAppendElement(NULL, elemObj, Tcl_NewStringObj(key, -1));
	    Tcl_ListObjAppendElement(NULL, elemObj, Tcl_NewIntObj(arrayPtr->locks));
	    Tcl_ListObjAppendElement(NULL, listObj, elemObj);

            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&bucketPtr->lock);
	Tcl_ListObjAppendElement(interp, resultObj, listObj);
    }

    return TCL_OK;
}

