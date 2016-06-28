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

static void SetVar(Array *arrayPtr, const char *key, const char *value, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void UpdateVar(Tcl_HashEntry *hPtr, const char *value, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int IncrVar(Array *arrayPtr, const char *key, int incr, Tcl_WideInt *valuePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static Ns_ReturnCode Unset(Array *arrayPtr, const char *key)
    NS_GNUC_NONNULL(1);

static void Flush(Array *arrayPtr)
    NS_GNUC_NONNULL(1);

static Array *LockArray(const NsServer *servPtr, const char *arrayName, bool create)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void UnlockArray(const Array *arrayPtr)
    NS_GNUC_NONNULL(1);

static Array *LockArrayObj(Tcl_Interp *interp, Tcl_Obj *arrayObj, bool create)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Array *GetArray(Bucket *bucketPtr, const char *arrayName, bool create)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static unsigned int BucketIndex(const char *arrayName) 
    NS_GNUC_NONNULL(1);

/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

Bucket *
NsTclCreateBuckets(const char *server, int nbuckets)
{
    char    buf[NS_THREAD_NAMESIZE];
    Bucket *buckets;

    NS_NONNULL_ASSERT(server != NULL);

    buckets = ns_malloc(sizeof(Bucket) * (size_t)nbuckets);
    while (--nbuckets >= 0) {
        snprintf(buf, sizeof(buf), "nsv:%d", nbuckets);
        Tcl_InitHashTable(&buckets[nbuckets].arrays, TCL_STRING_KEYS);
        Ns_MutexInit(&buckets[nbuckets].lock);
        Ns_MutexSetName2(&buckets[nbuckets].lock, buf, server);
    }

    return buckets;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                  int objc, Tcl_Obj *CONST* objv)
{
    Array               *arrayPtr;
    const Tcl_HashEntry *hPtr;
    Tcl_Obj             *resultObj;
    int                  result = TCL_OK;

    if (unlikely(objc < 3 || objc > 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?varName?");
        return TCL_ERROR;
    }
    arrayPtr = LockArrayObj(interp, objv[1], NS_FALSE);
    if (unlikely(arrayPtr == NULL)) {
        return TCL_ERROR;
    }

    hPtr = Tcl_FindHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]));
    resultObj = likely(hPtr != NULL) ? Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1) : NULL;
    UnlockArray(arrayPtr);

    if (objc == 3) {
	if (likely(resultObj != NULL)) {
	    Tcl_SetObjResult(interp, resultObj);
	} else {
	    Tcl_AppendResult(interp, "no such key: ",
			     Tcl_GetString(objv[2]), NULL);
	    result = TCL_ERROR;
	}
    } else {
	Tcl_SetObjResult(interp, Tcl_NewBooleanObj(resultObj != NULL));
	if (likely(resultObj != NULL)) {
	    if (unlikely(Tcl_ObjSetVar2(interp, objv[3], NULL, resultObj, TCL_LEAVE_ERR_MSG) == NULL)) {
                result = TCL_ERROR;
            }
	}
    }

    return result;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvExistsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                     int objc, Tcl_Obj *CONST* objv)
{
    Array *arrayPtr;
    int    exists = 0;

    if (unlikely(objc != 3)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key");
        return TCL_ERROR;
    }
    arrayPtr = LockArrayObj(interp, objv[1], NS_FALSE);
    if (likely(arrayPtr != NULL)) {
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
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvSetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                  int objc, Tcl_Obj *CONST* objv)
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
        const char *value = Tcl_GetStringFromObj(objv[3], &len);

        arrayPtr = LockArrayObj(interp, objv[1], NS_TRUE);
	assert(arrayPtr != NULL);
        SetVar(arrayPtr, key, value, (size_t)len);
        UnlockArray(arrayPtr);

        Tcl_SetObjResult(interp, objv[3]);
    } else {

        arrayPtr = LockArrayObj(interp, objv[1], NS_FALSE);
        if (unlikely(arrayPtr == NULL)) {
            result = TCL_ERROR;
        } else {
            const Tcl_HashEntry *hPtr;

            hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
            if (likely(hPtr != NULL)) {
                Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
            } else {
                Tcl_AppendResult(interp, "no such key: ", key, NULL);
                result = TCL_ERROR;
            }
            UnlockArray(arrayPtr);
        }
    }

    return result;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvIncrObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                   int objc, Tcl_Obj *CONST* objv)
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
    arrayPtr = LockArrayObj(interp, objv[1], NS_TRUE);
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
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvLappendObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                      int objc, Tcl_Obj *CONST* objv)
{
    Array         *arrayPtr;
    Tcl_HashEntry *hPtr;
    const char    *value;
    int            isNew, len;

    if (unlikely(objc < 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key value ?value ...?");
        return TCL_ERROR;
    }
    arrayPtr = LockArrayObj(interp, objv[1], NS_TRUE);

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
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvAppendObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                     int objc, Tcl_Obj *CONST* objv)
{
    Array         *arrayPtr;
    Tcl_HashEntry *hPtr;
    const char    *value;
    int            i, isNew, len;

    if (unlikely(objc < 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key value ?value ...?");
        return TCL_ERROR;
    }
    arrayPtr = LockArrayObj(interp, objv[1], NS_TRUE);
    assert(arrayPtr != NULL);

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &isNew);
    if (isNew == 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
    }
    for (i = 3; i < objc; ++i) {
        Tcl_AppendResult(interp, Tcl_GetString(objv[i]), NULL);
    }
    value = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &len);
    UpdateVar(hPtr, value, (size_t)len);
    UnlockArray(arrayPtr);

    return TCL_OK;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvUnsetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    int objc, Tcl_Obj *CONST* objv)
{
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

    arrayPtr = LockArrayObj(interp, arrayObj, NS_FALSE);
    if (unlikely(arrayPtr == NULL)) {
        if (nocomplain != 0) {
            Tcl_ResetResult(interp);
        } else {
            result = TCL_ERROR;
        }
        
    } else {
    
        assert(arrayPtr != NULL);

        if (Unset(arrayPtr, key) != NS_OK && key != NULL) {
            Tcl_AppendResult(interp, "no such key: ", key, NULL);
            result = TCL_ERROR;
        }

        /*
         * If everything went well and we have no key specified, delete
         * the array entry.
         */
        if (result == TCL_OK && key == NULL) {
            /*
             * Delete the hash-table of this array and the entry in the
             * table of array names.
             */
            Tcl_DeleteHashTable(&arrayPtr->vars);
            Tcl_DeleteHashEntry(arrayPtr->entryPtr);
        }
        UnlockArray(arrayPtr);
        
        if (result == TCL_OK && key == NULL) {
            /*
             * Free the actual array data strucure and invalidate the
             * Tcl_Obj.
             */
            ns_free(arrayPtr);
            Ns_TclSetTwoPtrValue(arrayObj, NULL, NULL, NULL);
        }
    }
    
    return result;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvNamesObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    const NsServer *servPtr = itPtr->servPtr;
    Tcl_HashSearch  search;
    Tcl_Obj        *resultObj;
    Bucket         *bucketPtr;
    const char     *pattern, *key;
    int             i, result = TCL_OK;

    if (unlikely(objc != 1 && objc !=2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "?pattern?");
        return TCL_ERROR;
    }
    pattern = (objc < 2) ? NULL : Tcl_GetString(objv[1]);

    /*
     * Walk the bucket list for each array.
     */

    resultObj = Tcl_GetObjResult(interp);
    for (i = 0; i < servPtr->nsv.nbuckets; i++) {
        Tcl_HashEntry  *hPtr;

        bucketPtr = &servPtr->nsv.buckets[i];
        Ns_MutexLock(&bucketPtr->lock);
        hPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
        while (hPtr != NULL) {
            key = Tcl_GetHashKey(&bucketPtr->arrays, hPtr);
            if ((pattern == NULL) || (Tcl_StringMatch(key, pattern) != 0)) {
                result = Tcl_ListObjAppendElement(interp, resultObj,
                                                  Tcl_NewStringObj(key, -1));
                if (unlikely(result != TCL_OK)) {
                    break;
                }
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&bucketPtr->lock);
        if (unlikely(result != TCL_OK)) {
            break;
        }
    }

    return result;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvArrayObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    int objc, Tcl_Obj *CONST* objv)
{
    Array          *arrayPtr;
    Tcl_HashSearch  search;
    int             i, opt, lobjc, size;
    Tcl_Obj       **lobjv;

    static const char *const opts[] = {
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

        arrayPtr = LockArrayObj(interp, objv[2], NS_TRUE);
	assert(arrayPtr != NULL);
	
        if (opt == (int)CResetIdx) {
            Flush(arrayPtr);
        }
        for (i = 0; i < lobjc; i += 2) {
            const char *value = Tcl_GetStringFromObj(lobjv[i+1], &size);

            SetVar(arrayPtr, Tcl_GetString(lobjv[i]), value, (size_t)size);
        }
        UnlockArray(arrayPtr);
        break;

    case CSizeIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "array");
            return TCL_ERROR;
        }
        arrayPtr = LockArrayObj(interp, objv[2], NS_FALSE);
        if (arrayPtr == NULL) {
            size = 0;
        } else {
            size = arrayPtr->vars.numEntries;
            UnlockArray(arrayPtr);
        }
	Tcl_SetObjResult(interp, Tcl_NewIntObj(size));
        break;

    case CExistsIdx:
        if (objc != 3) {
            Tcl_WrongNumArgs(interp, 2, objv, "array");
            return TCL_ERROR;
        }
        arrayPtr = LockArrayObj(interp, objv[2], NS_FALSE);
        if (arrayPtr == NULL) {
            size = 0;
        } else {
            size = 1;
            UnlockArray(arrayPtr);
        }
	Tcl_SetObjResult(interp, Tcl_NewBooleanObj(size));
        break;

    case CGetIdx:
    case CNamesIdx:
        if (objc != 3 && objc != 4) {
            Tcl_WrongNumArgs(interp, 2, objv, "array ?pattern?");
            return TCL_ERROR;
        }
        arrayPtr = LockArrayObj(interp, objv[2], NS_FALSE);
        Tcl_ResetResult(interp);
        if (arrayPtr != NULL) {
	    const Tcl_HashEntry *hPtr    = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
	    const char          *pattern = (objc > 3) ? Tcl_GetString(objv[3]) : NULL;

            while (hPtr != NULL) {
                const char *key = Tcl_GetHashKey(&arrayPtr->vars, hPtr);

                if ((pattern == NULL) || (Tcl_StringMatch(key, pattern) != 0)) {
                    Tcl_AppendElement(interp, key);
                    if (opt == (int)CGetIdx) {
                        Tcl_AppendElement(interp, Tcl_GetHashValue(hPtr));
                    }
                }
                hPtr = Tcl_NextHashEntry(&search);
            }
            UnlockArray(arrayPtr);
        }
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        break;
    }

    return TCL_OK;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_VarGet(const char *server, const char *array, const char *key, Ns_DString *dsPtr)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Array *arrayPtr = LockArray(servPtr, array, NS_FALSE);
        if (likely(arrayPtr != NULL)) {
	    const Tcl_HashEntry *hPtr = Tcl_FindHashEntry(&arrayPtr->vars, key);
	    if (likely(hPtr != NULL)) {
		Ns_DStringAppend(dsPtr, Tcl_GetHashValue(hPtr));
		status = NS_OK;
	    }
	    UnlockArray(arrayPtr);
	}
    }
    return status;
}


/*
 *-----------------------------------------------------------------------------
 *
 * Ns_VarExists
 *
 *      Return 1 if the key exists int the given array.
 *
 * Results:
 *      NS_TRUE or NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

bool
Ns_VarExists(const char *server, const char *array, const char *key)
{
    const NsServer *servPtr;
    bool            exists = NS_FALSE;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
	Array *arrayPtr = LockArray(servPtr, array, NS_FALSE);
        
        if (likely(arrayPtr != NULL)) {
	    if (Tcl_FindHashEntry(&arrayPtr->vars, key) != NULL) {
		exists = NS_TRUE;
	    }
	    UnlockArray(arrayPtr);
	}
    }
    return exists;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_VarSet(const char *server, const char *array, const char *key,
          const char *value, ssize_t len)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
	Array *arrayPtr = LockArray(servPtr, array, NS_TRUE);
        
	if (likely(arrayPtr != NULL)) {
	    SetVar(arrayPtr, key, value, (len > -1) ? (size_t)len : strlen(value));
	    UnlockArray(arrayPtr);
	    status = NS_OK;
	}
    }
    return status;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

Tcl_WideInt
Ns_VarIncr(const char *server, const char *array, const char *key, int incr)
{
    const NsServer *servPtr;
    Tcl_WideInt     counter = -1;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
	Array *arrayPtr = LockArray(servPtr, array, NS_TRUE);
        
	if (likely(arrayPtr != NULL)) {
	    (void) IncrVar(arrayPtr, key, incr, &counter);
	    UnlockArray(arrayPtr);
	}
    }
    return counter;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_VarAppend(const char *server, const char *array, const char *key,
             const char *value, ssize_t len)
{
    const NsServer *servPtr;
    int             isNew;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
	Array  *arrayPtr = LockArray(servPtr, array, NS_TRUE);
        if (likely(arrayPtr != NULL)) {
	    Tcl_HashEntry *hPtr;
	    size_t         oldLen, newLen;
	    char          *oldString, *newString;

	    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &isNew);

	    oldString = Tcl_GetHashValue(hPtr);
	    oldLen = (oldString != NULL) ? strlen(oldString) : 0u;

	    newLen = oldLen + ((len > -1) ? (size_t)len : strlen(value)) + 1u;
	    newString = ns_realloc(oldString, newLen + 1u);
	    memcpy(newString + oldLen, value, newLen + 1u);

	    Tcl_SetHashValue(hPtr, newString);
	    
	    UnlockArray(arrayPtr);
	    status = NS_OK;
	}
    }

    return status;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_VarUnset(const char *server, const char *array, const char *key)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(key != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
	Array  *arrayPtr = LockArray(servPtr, array, NS_FALSE);
        if (likely(arrayPtr != NULL)) {
	    status = Unset(arrayPtr, key);
	    UnlockArray(arrayPtr);
	}
    }
    return status;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

static unsigned int
BucketIndex(const char *arrayName) {
    unsigned int index = 0u;

    for (;;) {
	register unsigned int i = UCHAR(*(arrayName++));
	if (unlikely(i == 0u)) {
            break;
        }
        index += (index << 3u) + i;
    }
    return index;
}


/*
 *-----------------------------------------------------------------------------
 *
 * GetArray --
 *
 *      Lookup the array from the provided bucket. The function is assumed to
 *      be called when the mutex of the bucket is already locked.
 *
 * Results:
 *      Pointer to Array or NULL.
 *
 * Side effects;
 *      Array is created if it does not exist and 'create' is NS_TRUE.
 *
 *-----------------------------------------------------------------------------
 */

static Array *
GetArray(Bucket *bucketPtr, const char *arrayName, bool create) {
    Tcl_HashEntry *hPtr;
    Array         *arrayPtr;

    NS_NONNULL_ASSERT(bucketPtr != NULL);
    NS_NONNULL_ASSERT(arrayName != NULL);
        
    if (unlikely(create)) {
        int isNew;
        
        hPtr = Tcl_CreateHashEntry(&bucketPtr->arrays, arrayName, &isNew);
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
        hPtr = Tcl_FindHashEntry(&bucketPtr->arrays, arrayName);
        if (unlikely(hPtr == NULL)) {
            Ns_MutexUnlock(&bucketPtr->lock);
            return NULL;
        }
        arrayPtr = Tcl_GetHashValue(hPtr);
    }
    arrayPtr->locks++;

    return arrayPtr;
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */
static Array *
LockArray(const NsServer *servPtr, const char *arrayName, bool create)
{
    Bucket        *bucketPtr;
    unsigned int   index;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(arrayName != NULL);

    index = BucketIndex(arrayName);
    bucketPtr = &servPtr->nsv.buckets[index % (unsigned int)servPtr->nsv.nbuckets];
    Ns_MutexLock(&bucketPtr->lock);
    
    return GetArray(bucketPtr, arrayName, create);
}

static void
UnlockArray(const Array *arrayPtr)
{
    NS_NONNULL_ASSERT(arrayPtr != NULL);
    Ns_MutexUnlock(&((arrayPtr)->bucketPtr->lock));
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

static void
UpdateVar(Tcl_HashEntry *hPtr, const char *value, size_t len)
{
    char *oldString, *newString;

    NS_NONNULL_ASSERT(hPtr != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    oldString = Tcl_GetHashValue(hPtr);
    newString = ns_realloc(oldString, len + 1u);
    memcpy(newString, value, len + 1u);
    Tcl_SetHashValue(hPtr, newString);
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

static void
SetVar(Array *arrayPtr, const char *key, const char *value, size_t len)
{
    Tcl_HashEntry *hPtr;
    int            isNew;

    NS_NONNULL_ASSERT(arrayPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, &isNew);
    UpdateVar(hPtr, value, len);
}


/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

static int
IncrVar(Array *arrayPtr, const char *key, int incr, Tcl_WideInt *valuePtr)
{
    Tcl_HashEntry *hPtr;
    CONST char    *oldString;
    int            isNew, status;
    Tcl_WideInt    counter = -1;

    NS_NONNULL_ASSERT(arrayPtr != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(valuePtr != NULL);

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
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

static Ns_ReturnCode
Unset(Array *arrayPtr, const char *key)
{
    Ns_ReturnCode status = NS_ERROR;

    NS_NONNULL_ASSERT(arrayPtr != NULL);

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
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

static void
Flush(Array *arrayPtr)
{
    Tcl_HashEntry  *hPtr;
    Tcl_HashSearch  search;

    NS_NONNULL_ASSERT(arrayPtr != NULL);

    hPtr = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
    while (hPtr != NULL) {
        ns_free(Tcl_GetHashValue(hPtr));
        Tcl_DeleteHashEntry(hPtr);
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *-----------------------------------------------------------------------------
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
 *      Array is created if it does not exists and 'create' is true.
 *
 *-----------------------------------------------------------------------------
 */

static Array *
LockArrayObj(Tcl_Interp *interp, Tcl_Obj *arrayObj, bool create)
{
    Array              *arrayPtr = NULL;
    Bucket             *bucketPtr;
    static const char  *const arrayType = "nsv:array";
    const char         *arrayName;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(arrayObj != NULL);

    arrayName = Tcl_GetString(arrayObj);

    if (likely(Ns_TclGetOpaqueFromObj(arrayObj, arrayType, (void **) &bucketPtr) == TCL_OK)
        && bucketPtr != NULL) {

        Ns_MutexLock(&bucketPtr->lock);
        arrayPtr = GetArray(bucketPtr, arrayName, create);
    } else {
        const NsInterp *itPtr = NsGetInterpData(interp);

        arrayPtr = LockArray(itPtr->servPtr, arrayName, create);
        if (likely(arrayPtr != NULL)) {
            Ns_TclSetOpaqueObj(arrayObj, arrayType, arrayPtr->bucketPtr);
        }
    }
    
    if (arrayPtr == NULL && !create) {
        Tcl_AppendResult(interp, "no such array: ", arrayName, NULL);
    }

    return arrayPtr;
}



/*
 *-----------------------------------------------------------------------------
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
 *-----------------------------------------------------------------------------
 */

int
NsTclNsvBucketObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const NsInterp *itPtr = clientData;
    const NsServer *servPtr = itPtr->servPtr;
    int		    bucketNr = -1, i, result = TCL_OK;
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
        const Tcl_HashEntry *hPtr;
	Tcl_HashSearch       search;

        if (bucketNr > -1 && i != bucketNr) {
	    continue;
        }
	listObj = Tcl_NewListObj(0, NULL);
        bucketPtr = &servPtr->nsv.buckets[i];
        Ns_MutexLock(&bucketPtr->lock);
        hPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
        while (hPtr != NULL) {
	    const char  *key      = Tcl_GetHashKey(&bucketPtr->arrays, hPtr);
	    const Array *arrayPtr = Tcl_GetHashValue(hPtr);
	    Tcl_Obj     *elemObj  = Tcl_NewListObj(0, NULL);

	    result = Tcl_ListObjAppendElement(interp, elemObj, Tcl_NewStringObj(key, -1));
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, elemObj, Tcl_NewIntObj(arrayPtr->locks));
            }
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, listObj, elemObj);
            }
            if (unlikely(result != TCL_OK)) {
                break;
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Ns_MutexUnlock(&bucketPtr->lock);
        if (likely(result == TCL_OK)) {
            result = Tcl_ListObjAppendElement(interp, resultObj, listObj);
        }
        if (unlikely(result != TCL_OK)) {
            break;
        }
    }

    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
