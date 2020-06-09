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

#define USE_RWLOCK 1

/*
 * The following structure defines a collection of arrays.
 * Only the arrays within a given bucket share a lock,
 * allowing for more concurrency in nsv.
 */

typedef struct Bucket {
#ifdef USE_RWLOCK
    Ns_RWLock     lock;
#else
    Ns_Mutex      lock;
#endif
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
 * The following are the valid return values of an Ns_DriverAcceptProc.
 */

typedef enum {
    NSV_READ,
    NSV_WRITE
} NSV_LOCK;


/*
 * Local functions defined in this file.
 */

static void SetVar(Array *arrayPtr, const char *keyString, const char *value, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void UpdateVar(Tcl_HashEntry *hPtr, const char *value, size_t len)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int IncrVar(Array *arrayPtr, const char *keyString, int incr, Tcl_WideInt *valuePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static Ns_ReturnCode Unset(Array *arrayPtr, const char *keyString)
    NS_GNUC_NONNULL(1);

static void Flush(Array *arrayPtr)
    NS_GNUC_NONNULL(1);

static Array *LockArray(const NsServer *servPtr, const char *arrayName, bool create, NSV_LOCK rw)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void UnlockArray(const Array *arrayPtr)
    NS_GNUC_NONNULL(1);

static Array *LockArrayObj(Tcl_Interp *interp, Tcl_Obj *arrayObj, bool create, NSV_LOCK rw)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static Array *GetArray(Bucket *bucketPtr, const char *arrayName, bool create)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static unsigned int BucketIndex(const char *arrayName)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;

static int GetArrayAndKey(Tcl_Interp *interp, Tcl_Obj *arrayObj, const char *keyString,
                          NSV_LOCK rw, Array  **arrayPtrPtr, Tcl_Obj **objPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(5) NS_GNUC_NONNULL(6);


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
    /*fprintf(stderr, "=== %d buckets require %lu bytes, array needs %ld bytes\n",
      nbuckets, sizeof(Bucket) * (size_t)nbuckets, sizeof(Array));*/
    memcpy(buf, "nsv:", 4);
    while (--nbuckets >= 0) {
        (void) ns_uint32toa(&buf[4], (uint32_t)nbuckets);
        Tcl_InitHashTable(&buckets[nbuckets].arrays, TCL_STRING_KEYS);
        buckets[nbuckets].lock = NULL;
#ifdef USE_RWLOCK
        Ns_RWLockInit(&buckets[nbuckets].lock);
        Ns_RWLockSetName2(&buckets[nbuckets].lock, buf, server);
#else
        Ns_MutexInit(&buckets[nbuckets].lock);
        Ns_MutexSetName2(&buckets[nbuckets].lock, buf, server);
#endif
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
                  int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (unlikely(objc < 3 || objc > 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?varName?");
        result = TCL_ERROR;

    } else {
        Array *arrayPtr = LockArrayObj(interp, objv[1], NS_FALSE, NSV_READ);

        if (unlikely(arrayPtr == NULL)) {
            result = TCL_ERROR;

        } else {
            Tcl_Obj             *resultObj;
            const Tcl_HashEntry *hPtr;
            const char          *keyString = Tcl_GetString(objv[2]);

            hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, NULL);
            resultObj = likely(hPtr != NULL) ? Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1) : NULL;
            UnlockArray(arrayPtr);

            if (objc == 3) {
                if (likely(resultObj != NULL)) {
                    Tcl_SetObjResult(interp, resultObj);
                } else {
                    Ns_TclPrintfResult(interp, "no such key: %s", keyString);
                    Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "NSV", "KEY", keyString, NULL);
                    result = TCL_ERROR;
                }
            } else /* (objc == 4) */ {
                Tcl_SetObjResult(interp, Tcl_NewBooleanObj(resultObj != NULL));
                if (likely(resultObj != NULL)) {
                    if (unlikely(Tcl_ObjSetVar2(interp, objv[3], NULL, resultObj, TCL_LEAVE_ERR_MSG) == NULL)) {
                        result = TCL_ERROR;
                    }
                }
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
                     int objc, Tcl_Obj *const* objv)
{
    int result;

    if (unlikely(objc != 3)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key");
        result = TCL_ERROR;
    } else {
        bool   exists = NS_FALSE;
        Array *arrayPtr = LockArrayObj(interp, objv[1], NS_FALSE, NSV_READ);

        if (likely(arrayPtr != NULL)) {
            if (Tcl_CreateHashEntry(&arrayPtr->vars,
                                    Tcl_GetString(objv[2]), NULL) != NULL) {
                exists = NS_TRUE;
            }
            UnlockArray(arrayPtr);
        }
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(exists));
        result = TCL_OK;
    }

    return result;
}

static bool
SetResultToOldValue(Tcl_Interp *interp, Array *arrayPtr, const char *key)
{
    const Tcl_HashEntry *hPtr;
    bool                 result;

    /*
     * Get old value
     */
    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, key, NULL);
    if (likely(hPtr != NULL)) {
        result = NS_TRUE;
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
    } else {
        result = NS_FALSE;
        Tcl_SetObjResult(interp, Tcl_NewStringObj("", 0));
    }
    return result;
}



/*
 *-----------------------------------------------------------------------------
 *
 * NsTclNsvSetObjCmd --
 *
 *      Implements nsv_set.
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
                  int objc, Tcl_Obj *const* objv)
{
    int      result = TCL_OK, doReset = 0, doDefault = 0;
    Array   *arrayPtr;
    Tcl_Obj *arrayObj, *valueObj = NULL;
    char    *keyString;

    Ns_ObjvSpec lopts[] = {
        {"-default", Ns_ObjvBool,   &doDefault, INT2PTR(NS_TRUE)},
        {"-reset",   Ns_ObjvBool,   &doReset,   INT2PTR(NS_TRUE)},
        {"--",       Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"array",  Ns_ObjvObj,    &arrayObj,  NULL},
        {"key",    Ns_ObjvString, &keyString, NULL},
        {"?value",  Ns_ObjvObj,   &valueObj,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if ((doDefault != 0) && (doReset != 0)) {
        Ns_TclPrintfResult(interp, "only '-default' or '-reset' can be used");
        result = TCL_ERROR;

    } else if (valueObj != NULL) {
        int         len;
        bool        setArrayValue = NS_TRUE, returnNewValue = NS_TRUE;
        const char *value = Tcl_GetStringFromObj(valueObj, &len);

        arrayPtr = LockArrayObj(interp, arrayObj, NS_TRUE, NSV_WRITE);
        assert(arrayPtr != NULL);

        /*
         * Handle special flags.
         */
        if (unlikely((doReset != 0) || (doDefault != 0))) {
            bool didExist = SetResultToOldValue(interp, arrayPtr, keyString);

            if (doReset != 0) {
                /*
                 * When "-reset" was given, we return always the old value.
                 */
                returnNewValue = NS_FALSE;
            }
            if (doDefault != 0) {
                /*
                 * When "-default" was given, we return the old value, when
                 * the array element existed already.
                 */
                if (didExist) {
                    returnNewValue = NS_FALSE;
                    setArrayValue = NS_FALSE;
                } else {
                    /*
                     * It is a new array element, so set it.
                     */
                    setArrayValue = NS_TRUE;
                }
            }
        }
        /*
         * Set the array to the provided value.
         */
        if (setArrayValue) {
            SetVar(arrayPtr, keyString, value, (size_t)len);
        }
        UnlockArray(arrayPtr);

        if (returnNewValue) {
            Tcl_SetObjResult(interp, valueObj);
        }

    } else if (doReset == (int)NS_TRUE) {

        /*
         * Get the old value and unset.
         */

        arrayPtr = LockArrayObj(interp, arrayObj, NS_FALSE, NSV_WRITE);
        if (unlikely(arrayPtr == NULL)) {
            result = TCL_ERROR;

        } else {
            SetResultToOldValue(interp, arrayPtr, keyString);
            (void) Unset(arrayPtr, keyString);
            UnlockArray(arrayPtr);
        }

    } else if (doDefault == (int)NS_TRUE) {
        Ns_TclPrintfResult(interp, "can't use '-default' without providing a value for key %s", keyString);
        result = TCL_ERROR;

    } else {

        /*
         * This is the undocumented but used (e.g. in nstrace.tcl) variant of
         * "ns_set" behaving like "nsv_get".
         */

        arrayPtr = LockArrayObj(interp, objv[1], NS_FALSE, NSV_READ);
        if (arrayPtr == NULL) {
            result = TCL_ERROR;
        } else {
            const Tcl_HashEntry *hPtr = NULL;

            hPtr = Tcl_FindHashEntry(&arrayPtr->vars, keyString);
            if (likely(hPtr != NULL)) {
                Tcl_SetObjResult(interp, Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
            }
            UnlockArray(arrayPtr);
            if (hPtr == NULL) {
                Ns_TclPrintfResult(interp, "no such key: %s", keyString);
                Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "NSV", "KEY", keyString, NULL);
                result = TCL_ERROR;
            }
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
                   int objc, Tcl_Obj *const* objv)
{
    int  result, count = 1;

    if (unlikely(objc != 3 && objc != 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key ?increment?");
        result = TCL_ERROR;

    } else if (unlikely(objc == 4 && Tcl_GetIntFromObj(interp, objv[3], &count) != TCL_OK)) {
        result = TCL_ERROR;

    } else {
        Tcl_WideInt  current;
        Array       *arrayPtr = LockArrayObj(interp, objv[1], NS_TRUE, NSV_WRITE);

        assert(arrayPtr != NULL);
        result = IncrVar(arrayPtr, Tcl_GetString(objv[2]), count, &current);
        UnlockArray(arrayPtr);

        if (likely(result == TCL_OK)) {
            Tcl_SetObjResult(interp, Tcl_NewWideIntObj(current));
        } else {
            Ns_TclPrintfResult(interp, "array variable is not an integer");
        }
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
                      int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (unlikely(objc < 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key value ?value ...?");
        result = TCL_ERROR;
    } else {
        Array         *arrayPtr;
        Tcl_HashEntry *hPtr;
        int            isNew, i;
        Tcl_DString    ds;

        arrayPtr = LockArrayObj(interp, objv[1], NS_TRUE, NSV_WRITE);
        assert(arrayPtr != NULL);

        Tcl_DStringInit(&ds);

        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &isNew);
        if (unlikely(isNew == 0)) {
            Tcl_DStringAppend(&ds, Tcl_GetHashValue(hPtr), -1);
        }

        for (i = 3; i < objc; ++i) {
            Tcl_DStringAppendElement(&ds, Tcl_GetString(objv[i]));
        }

        UpdateVar(hPtr, ds.string, (size_t)ds.length);
        UnlockArray(arrayPtr);

        Tcl_DStringResult(interp, &ds);
    }
    return result;
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
                     int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (unlikely(objc < 4)) {
        Tcl_WrongNumArgs(interp, 1, objv, "array key value ?value ...?");
        result = TCL_ERROR;
    } else {
        Array         *arrayPtr;
        Tcl_HashEntry *hPtr;
        int            i, isNew;
        Tcl_DString    ds;

        arrayPtr = LockArrayObj(interp, objv[1], NS_TRUE, NSV_WRITE);
        assert(arrayPtr != NULL);

        Tcl_DStringInit(&ds);

        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, Tcl_GetString(objv[2]), &isNew);
        if (unlikely(isNew == 0)) {
            Tcl_DStringAppend(&ds, Tcl_GetHashValue(hPtr), -1);
        }

        for (i = 3; i < objc; ++i) {
            int          length;
            const char *value = Tcl_GetStringFromObj(objv[i], &length);

            Tcl_DStringAppend(&ds, value, length);
        }

        UpdateVar(hPtr, ds.string, (size_t)ds.length);
        UnlockArray(arrayPtr);

        Tcl_DStringResult(interp, &ds);

    }
    return result;
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
                    int objc, Tcl_Obj *const* objv)
{
    Tcl_Obj    *arrayObj;
    char       *keyString = NULL;
    int         nocomplain = 0, result = TCL_OK;
    Ns_ObjvSpec opts[] = {
        {"-nocomplain", Ns_ObjvBool,  &nocomplain, INT2PTR(NS_TRUE)},
        {"--",          Ns_ObjvBreak, NULL,        NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"array", Ns_ObjvObj,    &arrayObj,  NULL},
        {"?key",  Ns_ObjvString, &keyString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Array *arrayPtr = LockArrayObj(interp, arrayObj, NS_FALSE, NSV_WRITE);

        if (unlikely(arrayPtr == NULL)) {
            result = TCL_ERROR;

        } else {

            assert(arrayPtr != NULL);

            if (Unset(arrayPtr, keyString) != NS_OK && keyString != NULL) {
                Ns_TclPrintfResult(interp, "no such key: %s", keyString);
                Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "NSV", "KEY", keyString, NULL);
                result = TCL_ERROR;
            }

            /*
             * If everything went well and we have no key specified, delete
             * the array entry.
             */
            if (result == TCL_OK && keyString == NULL) {
                /*
                 * Delete the hash-table of this array and the entry in the
                 * table of array names.
                 */
                Tcl_DeleteHashTable(&arrayPtr->vars);
                Tcl_DeleteHashEntry(arrayPtr->entryPtr);
            }
            UnlockArray(arrayPtr);

            if (result == TCL_OK && keyString == NULL) {
                /*
                 * Free the actual array data structure and invalidate the
                 * Tcl_Obj.
                 */
                ns_free(arrayPtr);
                Ns_TclSetTwoPtrValue(arrayObj, NULL, NULL, NULL);
            }
        }
    }

    if (nocomplain != 0) {
        Tcl_ResetResult(interp);
        result = TCL_OK;
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
NsTclNsvNamesObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int result = TCL_OK;

    if (unlikely(objc != 1 && objc !=2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "?pattern?");
        result = TCL_ERROR;

    } else {
        const NsInterp *itPtr = clientData;
        const NsServer *servPtr = itPtr->servPtr;
        Tcl_Obj        *resultObj;
        const char     *pattern;
        int             i;

        pattern = (objc < 2) ? NULL : Tcl_GetString(objv[1]);

        /*
         * Walk the bucket list for each array.
         */

        resultObj = Tcl_GetObjResult(interp);
        for (i = 0; i < servPtr->nsv.nbuckets; i++) {
            const Tcl_HashEntry *hPtr;
            Tcl_HashSearch       search;
            Bucket              *bucketPtr = &servPtr->nsv.buckets[i];

#ifdef USE_RWLOCK
            Ns_RWLockRdLock(&bucketPtr->lock);
#else
            Ns_MutexLock(&bucketPtr->lock);
#endif
            hPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
            while (hPtr != NULL) {
                const char *keyString = Tcl_GetHashKey(&bucketPtr->arrays, hPtr);

                if ((pattern == NULL) || (Tcl_StringMatch(keyString, pattern) != 0)) {
                    result = Tcl_ListObjAppendElement(interp, resultObj,
                                                      Tcl_NewStringObj(keyString, -1));
                    if (unlikely(result != TCL_OK)) {
                        break;
                    }
                }
                hPtr = Tcl_NextHashEntry(&search);
            }
#ifdef USE_RWLOCK
            Ns_RWLockUnlock(&bucketPtr->lock);
#else
            Ns_MutexUnlock(&bucketPtr->lock);
#endif

            if (unlikely(result != TCL_OK)) {
                break;
            }
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
                    int objc, Tcl_Obj *const* objv)
{
    int                      opt, result = TCL_OK;
    static const char *const opts[] = {
        "set", "reset", "get", "names", "size", "exists", NULL
    };
    enum ISubCmdIdx {
        CSetIdx, CResetIdx, CGetIdx, CNamesIdx, CSizeIdx, CExistsIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ...");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        int        lobjc, size;
        Array     *arrayPtr;
        Tcl_Obj  **lobjv;

        switch (opt) {
        case CSetIdx:   NS_FALL_THROUGH; /* fall through */
        case CResetIdx:
            if (objc != 4) {
                Tcl_WrongNumArgs(interp, 2, objv, "array valueList");
                result = TCL_ERROR;

            } else if (Tcl_ListObjGetElements(interp, objv[3], &lobjc, &lobjv) != TCL_OK) {
                result = TCL_ERROR;

            } else if (lobjc % 2 == 1) {
                Ns_TclPrintfResult(interp, "invalid list: %s", Tcl_GetString(objv[3]));
                result = TCL_ERROR;

            } else {
                int  i;

                arrayPtr = LockArrayObj(interp, objv[2], NS_TRUE, NSV_WRITE);
                assert(arrayPtr != NULL);

                if (opt == (int)CResetIdx) {
                    Flush(arrayPtr);
                }
                for (i = 0; i < lobjc; i += 2) {
                    const char *value = Tcl_GetStringFromObj(lobjv[i+1], &size);

                    SetVar(arrayPtr, Tcl_GetString(lobjv[i]), value, (size_t)size);
                }
                UnlockArray(arrayPtr);
            }
            break;

        case CSizeIdx:
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "array");
                result = TCL_ERROR;

            } else {
                arrayPtr = LockArrayObj(interp, objv[2], NS_FALSE, NSV_READ);
                if (arrayPtr == NULL) {
                    size = 0;
                } else {
                    size = arrayPtr->vars.numEntries;
                    UnlockArray(arrayPtr);
                }
                Tcl_SetObjResult(interp, Tcl_NewIntObj(size));
            }
            break;

        case CExistsIdx:
            if (objc != 3) {
                Tcl_WrongNumArgs(interp, 2, objv, "array");
                result = TCL_ERROR;

            } else {
                arrayPtr = LockArrayObj(interp, objv[2], NS_FALSE, NSV_READ);
                if (arrayPtr == NULL) {
                    size = 0;
                } else {
                    size = 1;
                    UnlockArray(arrayPtr);
                }
                Tcl_SetObjResult(interp, Tcl_NewBooleanObj(size));
            }
            break;

        case CGetIdx:   NS_FALL_THROUGH; /* fall through */
        case CNamesIdx:
            if (objc != 3 && objc != 4) {
                Tcl_WrongNumArgs(interp, 2, objv, "array ?pattern?");
                result = TCL_ERROR;

            } else {
                Tcl_HashSearch  search;

                arrayPtr = LockArrayObj(interp, objv[2], NS_FALSE, NSV_READ);
                Tcl_ResetResult(interp);
                if (arrayPtr != NULL) {
                    Tcl_Obj             *listObj = Tcl_NewListObj(0, NULL);
                    const Tcl_HashEntry *hPtr    = Tcl_FirstHashEntry(&arrayPtr->vars, &search);
                    const char          *pattern = (objc > 3) ? Tcl_GetString(objv[3]) : NULL;

                    while (hPtr != NULL) {
                        const char *keyString = Tcl_GetHashKey(&arrayPtr->vars, hPtr);

                        if ((pattern == NULL) || (Tcl_StringMatch(keyString, pattern) != 0)) {
                            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(keyString, -1));
                            if (opt == (int)CGetIdx) {
                                Tcl_ListObjAppendElement(interp, listObj,
                                                         Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1));
                            }
                        }
                        hPtr = Tcl_NextHashEntry(&search);
                    }
                    UnlockArray(arrayPtr);
                    Tcl_SetObjResult(interp, listObj);
                }
            }
            break;

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }
    }
    return result;
}

/*
 *-----------------------------------------------------------------------------
 *
 * GetArrayAndKey --
 *
 *      Get access to array and dictionary object in one command.  Returns
 *      TCL_ERROR, when either the array or the dict does not exist.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */
static int
GetArrayAndKey(Tcl_Interp *interp, Tcl_Obj *arrayObj, const char *keyString,
               NSV_LOCK rw,
               Array  **arrayPtrPtr, Tcl_Obj **objPtr)
{
    int            result = TCL_OK;
    Tcl_Obj       *obj = NULL;
    Array         *arrayPtr;
    Tcl_HashEntry *hPtr;

    arrayPtr = LockArrayObj(interp, arrayObj, NS_FALSE, rw);
    if (arrayPtr != NULL) {
        hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, NULL);
        if (unlikely(hPtr == NULL)) {
            Ns_TclPrintfResult(interp, "no such key: %s", keyString);
            Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "NSV", "KEY", keyString, NULL);
            result = TCL_ERROR;
        } else {
            obj = Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1);
        }
    } else {
        result = TCL_ERROR;
    }
    *arrayPtrPtr = arrayPtr;
    *objPtr = obj;

    return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * NsTclNsvDictObjCmd --
 *
 *      Implements nsv_dict as an obj command.
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
NsTclNsvDictObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp,
                    int objc, Tcl_Obj *const* objv)
{
    int                      opt, result;
    static const char *const opts[] = {
        "append",
        "exists",
        "get",
        "getdef",
        "getwithdefault",
        "incr",
        "keys",
        "lappend",
        "set",
        "size",
        "unset",
        NULL
    };
    enum ISubCmdIdx {
        CAppendIdx,
        CExistsIdx,
        CGetIdx,
        CGetdefIdx,
        CGetdefwithdefaultIdx,
        CIncrIdx,
        CKeysIdx,
        CLappendIdx,
        CSetIdx,
        CSizeIdx,
        CUnsetIdx
    };

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ...");
        result = TCL_ERROR;

    } else if (Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                            &opt) != TCL_OK) {
        result = TCL_ERROR;

    } else {
        Array      *arrayPtr;
        Tcl_Obj    *arrayObj, *keyObj, *dictKeyObj, *dictValueObj, *dictObj;

        if (opt == CGetdefwithdefaultIdx) {
            opt = CGetdefIdx;
        }

        switch (opt) {

        case CKeysIdx:     NS_FALL_THROUGH; /* fall through */
        case CSizeIdx: {
            /*
             * Operations on the full dict
             */
            char       *pattern = NULL;
            Ns_ObjvSpec sizeArgs[] = {
                {"array",     Ns_ObjvObj,  &arrayObj,     NULL},
                {"key",       Ns_ObjvObj,  &keyObj,       NULL},
                {NULL, NULL, NULL, NULL}
            }, keysArgs [] =  {
                {"array",     Ns_ObjvObj,    &arrayObj,   NULL},
                {"key",       Ns_ObjvObj,    &keyObj,     NULL},
                {"?pattern",  Ns_ObjvString, &pattern,    NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL,
                             (opt == CSizeIdx ? sizeArgs : keysArgs),
                             interp, 2, objc, objv) != NS_OK) {
                result = TCL_ERROR;

            } else {
                result = GetArrayAndKey(interp, arrayObj, Tcl_GetString(keyObj), NSV_READ,
                                        &arrayPtr, &dictObj);
                if (result == TCL_OK) {
                    if (opt == CSizeIdx) {
                        int size;

                        result = Tcl_DictObjSize(interp, dictObj, &size);
                        if (result == TCL_OK) {
                            Tcl_SetObjResult(interp, Tcl_NewIntObj(size));
                        }
                    } else {
                        Tcl_DictSearch search;
                        Tcl_Obj       *listObj;
                        int            done = 0;

                        assert(opt == CKeysIdx);

                        listObj = Tcl_NewListObj(0, NULL);
                        Tcl_DictObjFirst(NULL, dictObj, &search, &dictKeyObj, NULL, &done);
                        for (; done == 0; Tcl_DictObjNext(&search, &dictKeyObj, NULL, &done)) {
                            if (!pattern || Tcl_StringMatch(Tcl_GetString(dictKeyObj), pattern)) {
                                Tcl_ListObjAppendElement(NULL, listObj, dictKeyObj);
                            }
                        }
                        Tcl_DictObjDone(&search);
                        Tcl_SetObjResult(interp, listObj);
                    }
                }
                if (arrayPtr != NULL) {
                    UnlockArray(arrayPtr);
                }
            }
            break;
        }

        case CExistsIdx:  NS_FALL_THROUGH; /* fall through */
        case CGetIdx:     NS_FALL_THROUGH; /* fall through */
        case CGetdefIdx:  NS_FALL_THROUGH; /* fall through */
        case CUnsetIdx: {
            /*
             * Operations on a dict key
             */
            int          nargs = 0;
            Tcl_Obj     *varnameObj = NULL;
            Ns_ObjvSpec getArgs[] = {
                {"array",     Ns_ObjvObj,  &arrayObj,     NULL},
                {"key",       Ns_ObjvObj,  &keyObj,       NULL},
                {"?dictkeys", Ns_ObjvArgs, &nargs,        NULL},
                {NULL, NULL, NULL, NULL}
            }, existsArgs[] = {
                {"array",     Ns_ObjvObj,  &arrayObj,     NULL},
                {"key",       Ns_ObjvObj,  &keyObj,       NULL},
                {"dictkeys",  Ns_ObjvArgs, &nargs,        NULL},
                {NULL, NULL, NULL, NULL}
            }, getdefArgs[] = {
                {"array",     Ns_ObjvObj,  &arrayObj,     NULL},
                {"key",       Ns_ObjvObj,  &keyObj,       NULL},
                {"args",      Ns_ObjvArgs, &nargs,        NULL},
                {NULL, NULL, NULL, NULL}
            };
            Ns_ObjvSpec getOpts[] = {
                {"-varname", Ns_ObjvObj,    &varnameObj,  NULL},
                {"--",       Ns_ObjvBreak,  NULL,         NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(((opt == CGetIdx || opt == CGetdefIdx) ? getOpts : NULL),
                              (opt == CGetdefIdx ? getdefArgs
                               : (opt == CExistsIdx || opt == CUnsetIdx) ? existsArgs
                               : getArgs), interp, 2, objc, objv) != NS_OK) {
                result = TCL_ERROR;
            } else if (opt == CGetdefIdx && nargs == 1) {
                Ns_TclPrintfResult(interp, "wrong # args: \"nsv_dict %s\" requires a key and a default",
                                   Tcl_GetString(objv[1]));
                result = TCL_ERROR;
            } else {
                result = GetArrayAndKey(interp, arrayObj, Tcl_GetString(keyObj),
                                        (opt != CUnsetIdx ? NSV_READ : NSV_WRITE),
                                        &arrayPtr, &dictObj);
                if (result == TCL_OK) {
                    if (opt == CUnsetIdx) {
                        /*
                         * dict unset
                         *
                         * "unset is silent, when dict key does not exist
                         * in the dict.
                         */
                        if (nargs == 1) {
                            result = Tcl_DictObjRemove(interp, dictObj,  objv[objc-1]);
                        } else {
                            /*
                             * Nested dict
                             */
                            result = Tcl_DictObjRemoveKeyList(interp, dictObj, nargs, &objv[objc-nargs]);
                        }
                        if (result == TCL_OK) {
                            Tcl_SetObjResult(interp, dictObj);
                        }
                    } else {
                        int lastObjc = (opt == CGetdefIdx ? objc -1 : objc);

                        if (nargs == 0) {
                            /*
                             * no keys
                             */
                            dictKeyObj = NULL;
                            dictValueObj = NULL;
                            Tcl_SetObjResult(interp, dictObj);

                        } else if (nargs == 1) {
                            /*
                             * one key
                             */
                            dictKeyObj = objv[objc-1];
                            result = Tcl_DictObjGet(interp, dictObj, dictKeyObj, &dictValueObj);
                        } else {
                            /*
                             * nested keys
                             */
                            int i;

                            for (i = objc - nargs; i < lastObjc; i++) {
                                dictKeyObj = objv[i];
                                result = Tcl_DictObjGet(interp, dictObj, dictKeyObj, &dictValueObj);
                                if (dictValueObj != NULL) {
                                    dictObj = dictValueObj;
                                } else {
                                    break;
                                }
                            }
                        }
                        if (dictValueObj != NULL) {
                            /*
                             * Dict value is available.
                             */
                            if (opt == CGetIdx || opt == CGetdefIdx) {
                                /*
                                 * dict get    dictkey:0..n
                                 * dict getdef dictkey:0..n default
                                 */
                                if (varnameObj != NULL) {
                                    Tcl_ObjSetVar2(interp, varnameObj, NULL, dictValueObj, 0);
                                    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(1));
                                } else {
                                    Tcl_SetObjResult(interp, dictValueObj);
                                }
                            } else if (opt == CExistsIdx) {
                                /*
                                 * dict exists dictkey:1..n
                                 */
                                Tcl_SetObjResult(interp, Tcl_NewBooleanObj(1));
                            } else {
                                /* should not happen */
                                assert(opt && 0);
                            }
                        } else if (nargs > 0 && result == TCL_OK) {
                            /*
                             * No dict value is available.
                             */
                            if (opt == CGetIdx) {
                                /*
                                 *  dict get dictkey:0..n
                                 */
                                if (varnameObj != NULL) {
                                    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
                                } else {
                                    Ns_TclPrintfResult(interp, "key \"%s\" not known in dictionary",
                                                       Tcl_GetString(dictKeyObj));
                                    Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "DICT",
                                                     Tcl_GetString(dictKeyObj), NULL);
                                    result = TCL_ERROR;
                                }
                            } else if (opt == CGetdefIdx) {
                                /*
                                 *  dict getdef dictkey:0..n default
                                 */
                                if (varnameObj != NULL) {
                                    Tcl_ObjSetVar2(interp, varnameObj, NULL, objv[objc-1], 0);
                                    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
                                } else {
                                    Tcl_SetObjResult(interp, objv[objc-1]);
                                }
                            } else if (opt == CExistsIdx) {
                                /*
                                 *  dict exists dictkey:1..n
                                 */
                                Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
                            } else {
                                /* should not happen */
                                assert(opt && 0);
                            }
                        }
                    }
                } else {
                    /*
                     * If array or dict does not exist, we can return in some
                     * cases ("exists" or "getdef") non-error results.
                     */
                    if (opt == CExistsIdx) {
                        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
                        result = TCL_OK;

                    } else if (opt == CGetIdx && varnameObj != NULL) {
                        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
                        result = TCL_OK;

                    } else if (opt == CGetdefIdx) {
                        if (varnameObj != NULL) {
                            Tcl_ObjSetVar2(interp, varnameObj, NULL, objv[objc-1], 0);
                            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(0));
                        } else {
                            Tcl_SetObjResult(interp, objv[objc-1]);
                        }
                        result = TCL_OK;
                    }
                }
                if (arrayPtr != NULL) {
                    UnlockArray(arrayPtr);
                }
            }
            break;
        }

        case CAppendIdx:  NS_FALL_THROUGH; /* fall through */
        case CIncrIdx:    NS_FALL_THROUGH; /* fall through */
        case CLappendIdx: NS_FALL_THROUGH; /* fall through */
        case CSetIdx: {
            /*
             * Operations on a dict key with a value
             */
            const Tcl_HashEntry *hPtr;
            int         increment = 1, nargs = 0;
            Ns_ObjvSpec setArgs[] = {
                {"array",     Ns_ObjvObj,  &arrayObj,     NULL},
                {"key",       Ns_ObjvObj,  &keyObj,       NULL},
                {"dictkey",   Ns_ObjvObj,  &dictKeyObj,   NULL},
                {"args",      Ns_ObjvArgs, &nargs,        NULL},
                {NULL, NULL, NULL, NULL}
            }, appendArgs[] = {
                {"array",     Ns_ObjvObj,  &arrayObj,     NULL},
                {"key",       Ns_ObjvObj,  &keyObj,       NULL},
                {"dictkey",   Ns_ObjvObj,  &dictKeyObj,   NULL},
                {"?args",     Ns_ObjvArgs, &nargs,        NULL},
                {NULL, NULL, NULL, NULL}
            }, incrArgs[] = {
                {"array",      Ns_ObjvObj, &arrayObj,     NULL},
                {"key",        Ns_ObjvObj, &keyObj,       NULL},
                {"dictkey",    Ns_ObjvObj, &dictKeyObj,   NULL},
                {"?increment", Ns_ObjvInt, &increment,    NULL},
                {NULL, NULL, NULL, NULL}
            }, *args;

            if (opt == CIncrIdx) {
                args = incrArgs;
            } else if (opt == CSetIdx) {
                args = setArgs;
            } else {
                /*
                 * For set, append and lappend.
                 */
                args = appendArgs;
            }
            if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
                result = TCL_ERROR;

            } else {
                const char *keyString;
                /*
                 * Create array and key if it does not exist
                 */
                arrayPtr = LockArrayObj(interp, arrayObj, NS_TRUE, NSV_WRITE);
                assert(arrayPtr != NULL);

                keyString = Tcl_GetString(keyObj);
                hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, NULL);
                if (likely(hPtr != NULL)) {
                    dictObj = Tcl_NewStringObj(Tcl_GetHashValue(hPtr), -1);
                } else {
                    dictObj = Tcl_NewDictObj();
                }
                if (opt == CSetIdx) {
                    /*
                     * dict set dictkey:1..n dictvalue
                     */
                    dictValueObj = objv[objc - 1];
                    if (nargs == 1) {
                        result = Tcl_DictObjPut(interp, dictObj, dictKeyObj, dictValueObj);
                    } else {
                        /*
                         * Nested dict
                         */
                        result = Tcl_DictObjPutKeyList(interp, dictObj, nargs,
                                                       &objv[objc-(nargs+1)], dictValueObj);
                    }
                } else {
                    Tcl_Obj *oldDictValueObj;

                    result = Tcl_DictObjGet(interp, dictObj, dictKeyObj, &oldDictValueObj);
                    if (opt == CIncrIdx) {
                        if (oldDictValueObj != NULL) {
                            int intValue;

                            result = Tcl_GetIntFromObj(interp, oldDictValueObj, &intValue);
                            if (result == TCL_OK) {
                                increment += intValue;
                            }
                        }
                        if (result == TCL_OK) {
                            result = Tcl_DictObjPut(interp, dictObj, dictKeyObj, Tcl_NewIntObj(increment));
                        }
                    } else {
                        Tcl_DString ds;
                        int         i, objLength;
                        const char *objString;

                        /*
                         * handling "append" and "lappend"
                         */
                        assert(opt == CAppendIdx || opt == CLappendIdx);

                        Tcl_DStringInit(&ds);
                        if (oldDictValueObj != NULL) {
                            objString = Tcl_GetStringFromObj(oldDictValueObj, &objLength);
                            Tcl_DStringAppend(&ds, objString, objLength);
                        }

                        for (i = objc - nargs; i < objc; i++) {
                            objString = Tcl_GetStringFromObj(objv[i], &objLength);

                            if (opt == CAppendIdx) {
                                Tcl_DStringAppend(&ds, objString, objLength);
                            } else {
                                Tcl_DStringAppendElement(&ds, objString);
                            }
                        }
                        if (result == TCL_OK) {
                            result = Tcl_DictObjPut(interp, dictObj, dictKeyObj, Tcl_NewStringObj(ds.string, ds.length));
                        }
                        Tcl_DStringFree(&ds);
                    }
                }
                if (result == TCL_OK) {
                    const char *dictString;
                    int         dictStringLength;

                    dictString = Tcl_GetStringFromObj(dictObj, &dictStringLength);
                    SetVar(arrayPtr, keyString, dictString, (size_t)dictStringLength);
                    Tcl_SetObjResult(interp, dictObj);
                } else {
                    result = TCL_ERROR;
                }
                UnlockArray(arrayPtr);
            }
            break;
        }

        default:
            /* unexpected value */
            assert(opt && 0);
            result = TCL_ERROR;
            break;
        }
    }
    return result;
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
Ns_VarGet(const char *server, const char *array, const char *keyString, Ns_DString *dsPtr)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);
    NS_NONNULL_ASSERT(dsPtr != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Array *arrayPtr = LockArray(servPtr, array, NS_FALSE, NSV_READ);
        if (likely(arrayPtr != NULL)) {
            const Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, NULL);
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
 *      Return 1 if the key exists in the given array.
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
Ns_VarExists(const char *server, const char *array, const char *keyString)
{
    const NsServer *servPtr;
    bool            exists = NS_FALSE;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Array *arrayPtr = LockArray(servPtr, array, NS_FALSE, NSV_READ);

        if (likely(arrayPtr != NULL)) {
            if (Tcl_CreateHashEntry(&arrayPtr->vars, keyString, NULL) != NULL) {
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
Ns_VarSet(const char *server, const char *array, const char *keyString,
          const char *value, ssize_t len)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Array *arrayPtr = LockArray(servPtr, array, NS_TRUE, NSV_WRITE);

        if (likely(arrayPtr != NULL)) {
            SetVar(arrayPtr, keyString, value, (len > -1) ? (size_t)len : strlen(value));
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
 *      Missing keys are initialized to 1.
 *
 *-----------------------------------------------------------------------------
 */

Tcl_WideInt
Ns_VarIncr(const char *server, const char *array, const char *keyString, int incr)
{
    const NsServer *servPtr;
    Tcl_WideInt     counter = -1;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Array *arrayPtr = LockArray(servPtr, array, NS_TRUE, NSV_WRITE);

        if (likely(arrayPtr != NULL)) {
            (void) IncrVar(arrayPtr, keyString, incr, &counter);
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
Ns_VarAppend(const char *server, const char *array, const char *keyString,
             const char *value, ssize_t len)
{
    const NsServer *servPtr;
    int             isNew;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Array  *arrayPtr = LockArray(servPtr, array, NS_TRUE, NSV_WRITE);
        if (likely(arrayPtr != NULL)) {
            Tcl_HashEntry *hPtr;
            size_t         oldLen, newLen;
            char          *oldString, *newString;

            hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, &isNew);

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
 *      Resets given key in the array, if key is NULL, flushes the whole array
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
Ns_VarUnset(const char *server, const char *array, const char *keyString)
{
    const NsServer *servPtr;
    Ns_ReturnCode   status = NS_ERROR;

    NS_NONNULL_ASSERT(array != NULL);
    NS_NONNULL_ASSERT(array != NULL);

    servPtr = NsGetServer(server);
    if (likely(servPtr != NULL)) {
        Array  *arrayPtr = LockArray(servPtr, array, NS_FALSE, NSV_WRITE);
        if (unlikely(arrayPtr == NULL)) {
            /* Error */
        } else {
            status = Unset(arrayPtr, keyString);
            if (status != NS_OK && keyString != NULL) {
                /* Error, no such key. */
            } else if (status == NS_OK && keyString == NULL) {
                /* Finish deleting the entire array, same as in NsTclNsvUnsetObjCmd(). */
                Tcl_DeleteHashTable(&arrayPtr->vars);
                Tcl_DeleteHashEntry(arrayPtr->entryPtr);
            }
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
    unsigned int idx = 0u;

    for (;;) {
        register unsigned int i = UCHAR(*(arrayName++));
        if (unlikely(i == 0u)) {
            break;
        }
        idx += (idx << 3u) + i;
    }
    return idx;
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

        hPtr = Tcl_CreateHashEntry(&bucketPtr->arrays, arrayName, NULL);
        if (unlikely(hPtr == NULL)) {
#ifdef USE_RWLOCK
            Ns_RWLockUnlock(&bucketPtr->lock);
#else
            Ns_MutexUnlock(&bucketPtr->lock);
#endif
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
LockArray(const NsServer *servPtr, const char *arrayName, bool create, NSV_LOCK rw)
{
    Bucket        *bucketPtr;
    unsigned int   idx;

    NS_NONNULL_ASSERT(servPtr != NULL);
    NS_NONNULL_ASSERT(arrayName != NULL);

    idx = BucketIndex(arrayName);
    bucketPtr = &servPtr->nsv.buckets[idx % (unsigned int)servPtr->nsv.nbuckets];
#ifdef USE_RWLOCK
    if (rw == NSV_READ) {
        Ns_RWLockRdLock(&bucketPtr->lock);
    } else {
        Ns_RWLockWrLock(&bucketPtr->lock);
    }
#else
    Ns_MutexLock(&bucketPtr->lock);
#endif

    return GetArray(bucketPtr, arrayName, create);
}

static void
UnlockArray(const Array *arrayPtr)
{
    NS_NONNULL_ASSERT(arrayPtr != NULL);
#ifdef USE_RWLOCK
    Ns_RWLockUnlock(&((arrayPtr)->bucketPtr->lock));
#else
    Ns_MutexUnlock(&((arrayPtr)->bucketPtr->lock));
#endif
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
SetVar(Array *arrayPtr, const char *keyString, const char *value, size_t len)
{
    Tcl_HashEntry *hPtr;
    int            isNew;

    NS_NONNULL_ASSERT(arrayPtr != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, &isNew);
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
IncrVar(Array *arrayPtr, const char *keyString, int incr, Tcl_WideInt *valuePtr)
{
    Tcl_HashEntry *hPtr;
    int            isNew, status;
    Tcl_WideInt    counter = -1;

    NS_NONNULL_ASSERT(arrayPtr != NULL);
    NS_NONNULL_ASSERT(keyString != NULL);
    NS_NONNULL_ASSERT(valuePtr != NULL);

    hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, &isNew);

    if (isNew != 0) {
        counter = 0;
        status = TCL_OK;
    } else {
        const char *oldString;

        oldString = Tcl_GetHashValue(hPtr);
        status = (Ns_StrToWideInt(oldString, &counter) == NS_OK)
            ? TCL_OK
            : TCL_ERROR;
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
Unset(Array *arrayPtr, const char *keyString)
{
    Ns_ReturnCode status = NS_ERROR;

    NS_NONNULL_ASSERT(arrayPtr != NULL);

    if (keyString != NULL) {
        Tcl_HashEntry *hPtr = Tcl_CreateHashEntry(&arrayPtr->vars, keyString, NULL);

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
LockArrayObj(Tcl_Interp *interp, Tcl_Obj *arrayObj, bool create, NSV_LOCK rw)
{
    Array              *arrayPtr;
    Bucket             *bucketPtr;
    static const char  *const arrayType = "nsv:array";
    const char         *arrayName;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(arrayObj != NULL);

    arrayName = Tcl_GetString(arrayObj);

    if (likely(Ns_TclGetOpaqueFromObj(arrayObj, arrayType, (void **) &bucketPtr) == TCL_OK)
        && bucketPtr != NULL) {
#ifdef USE_RWLOCK
        if (rw == NSV_READ) {
            Ns_RWLockRdLock(&bucketPtr->lock);
        } else {
            Ns_RWLockWrLock(&bucketPtr->lock);
        }
#else
        Ns_MutexLock(&bucketPtr->lock);
#endif
        arrayPtr = GetArray(bucketPtr, arrayName, create);
    } else {
        const NsInterp *itPtr = NsGetInterpData(interp);

        arrayPtr = LockArray(itPtr->servPtr, arrayName, create, rw);
        if (arrayPtr != NULL) {
            Ns_TclSetOpaqueObj(arrayObj, arrayType, arrayPtr->bucketPtr);
        }
    }

    /*
     * Both, GetArray() and LockArray() can return NULL.
     */
    if (arrayPtr == NULL && !create) {
        Ns_TclPrintfResult(interp, "no such array: %s", arrayName);
        Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "NSV", "ARRAY", arrayName, NULL);
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
 *      Implementation of nsv_bucket.
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
NsTclNsvBucketObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    const NsInterp   *itPtr = clientData;
    const NsServer   *servPtr = itPtr->servPtr;
    int               bucketNr = -1, result = TCL_OK;
    Ns_ObjvValueRange bucketRange = {0, servPtr->nsv.nbuckets};
    Ns_ObjvSpec       args[] = {
        {"?bucket-number", Ns_ObjvInt,  &bucketNr, &bucketRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_Obj *resultObj;
        int      i;

        /*
         * LOCK for servPtr->nsv ?
         */
        resultObj = Tcl_GetObjResult(interp);
        for (i = 0; i < servPtr->nsv.nbuckets; i++) {
            const Tcl_HashEntry *hPtr;
            Tcl_Obj             *listObj;
            Tcl_HashSearch       search;
            Bucket              *bucketPtr;

            if (bucketNr > -1 && i != bucketNr) {
                continue;
            }
            listObj = Tcl_NewListObj(0, NULL);
            bucketPtr = &servPtr->nsv.buckets[i];
#ifdef USE_RWLOCK
            Ns_RWLockRdLock(&bucketPtr->lock);
#else
            Ns_MutexLock(&bucketPtr->lock);
#endif
            hPtr = Tcl_FirstHashEntry(&bucketPtr->arrays, &search);
            while (hPtr != NULL) {
                const char  *keyString = Tcl_GetHashKey(&bucketPtr->arrays, hPtr);
                const Array *arrayPtr  = Tcl_GetHashValue(hPtr);
                Tcl_Obj     *elemObj   = Tcl_NewListObj(0, NULL);

                result = Tcl_ListObjAppendElement(interp, elemObj, Tcl_NewStringObj(keyString, -1));
                if (likely(result == TCL_OK)) {
                    result = Tcl_ListObjAppendElement(interp, elemObj, Tcl_NewLongObj(arrayPtr->locks));
                }
                if (likely(result == TCL_OK)) {
                    result = Tcl_ListObjAppendElement(interp, listObj, elemObj);
                }
                if (unlikely(result != TCL_OK)) {
                    break;
                }
                hPtr = Tcl_NextHashEntry(&search);
            }
#ifdef USE_RWLOCK
            Ns_RWLockUnlock(&bucketPtr->lock);
#else
            Ns_MutexUnlock(&bucketPtr->lock);
#endif
            if (likely(result == TCL_OK)) {
                result = Tcl_ListObjAppendElement(interp, resultObj, listObj);
            }
            if (unlikely(result != TCL_OK)) {
                break;
            }
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
