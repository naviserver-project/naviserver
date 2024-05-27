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
 * tclset.c --
 *
 *      Tcl API for NaviServer shared varianles via the "ns_set" command.
 */

#include "nsd.h"

/*
 * The following represent the valid combinations of
 * NS_TCL_SET flags
 */

#define SET_DYNAMIC   'd'
#define SET_STATIC    't'

#define IS_DYNAMIC(id) (*(id) == SET_DYNAMIC)

/*
 * Local functions defined in this file
 */

static int LookupSet(NsInterp *itPtr, const char *id, bool deleteEntry, Ns_Set **setPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static int LookupObjSet(NsInterp *itPtr, Tcl_Obj *idPtr, bool deleteEntry, Ns_Set **setPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static int LookupInterpSet(Tcl_Interp *interp, const char *id, bool deleteEntry, Ns_Set **setPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static Tcl_Obj *EnterSet(NsInterp *itPtr, Ns_Set *set, Ns_TclSetType type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclEnterSet --
 *
 *      Let this Tcl interpreter manage lifecycle of an existing Ns_Set.  The
 *      last argument determines the lifespan of the Ns_Set. When the type
 *      is NS_TCL_SET_STATIC, the Ns_Set is deleted, when the interp is
 *      freed. When the value is NS_TCL_SET_DYNAMIC, it is deleted via "ns_set
 *      free|cleanup". Effectively, this means that a "dynamic" ns_set is
 *      freed at the end a request, since ns_cleanup issues "ns_set cleanup".
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      A pointer to the Ns_Set is added to the interpreter's list of
 *      sets; a new handle is generated and appended to interp result.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclEnterSet(Tcl_Interp *interp, Ns_Set *set, Ns_TclSetType type)
{
    NsInterp *itPtr;
    int       result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    itPtr = NsGetInterpData(interp);
    if (unlikely(itPtr == NULL)) {
        Ns_TclPrintfResult(interp, "ns_set requires an interpreter");
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, EnterSet(itPtr, set, type));
        result = TCL_OK;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetSet --
 *
 *      Given a Tcl ns_set handle, return a pointer to the Ns_Set.
 *
 * Results:
 *      An Ns_Set pointer, or NULL if error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_Set *
Ns_TclGetSet(Tcl_Interp *interp, const char *setId)
{
    Ns_Set *set = NULL;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(setId != NULL);

    if (LookupInterpSet(interp, setId, NS_FALSE, &set) != TCL_OK) {
        set = NULL;
    }
    return set;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetSet2 --
 *
 *      Like Ns_TclGetSet, but sends errors to the Tcl interp.
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
Ns_TclGetSet2(Tcl_Interp *interp, const char *setId, Ns_Set **setPtr)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(setId != NULL);
    NS_NONNULL_ASSERT(setPtr != NULL);

    return LookupInterpSet(interp, setId, NS_FALSE, setPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclFreeSet --
 *
 *      Free a set id, and if own by Tcl, the underlying Ns_Set.
 *
 * Results:
 *      NS_OK/NS_ERROR.
 *
 * Side effects:
 *      Will free the set matching the passed-in set id, and all of
 *      its associated data.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclFreeSet(Tcl_Interp *interp, const char *setId)
{
    Ns_Set  *set = NULL;
    int      result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(setId != NULL);

    if (LookupInterpSet(interp, setId, NS_TRUE, &set) != TCL_OK) {
        result = TCL_ERROR;
    } else {
        result = TCL_OK;
        if (IS_DYNAMIC(setId)) {
            Ns_SetFree(set);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetCreateFromDict --
 *
 *      Create a set based on the data provided in form of a Tcl dict (flat
 *      list of attribute value pairs).
 *
 * Results:
 *      Created set or NULL on errors
 *
 * Side effects:
 *      When an interpreter is provided and an error occurs, the error message
 *      is set in the interpreter.
 *
 *----------------------------------------------------------------------
 */
Ns_Set *
Ns_SetCreateFromDict(Tcl_Interp *interp, const char *name, Tcl_Obj *listObj)
{
    int        result;
    TCL_SIZE_T objc;
    Tcl_Obj  **objv;
    Ns_Set    *setPtr;

    NS_NONNULL_ASSERT(listObj != NULL);

    result = Tcl_ListObjGetElements(interp, listObj, &objc, &objv);

    if (result != TCL_OK) {
        /*
         * Assume, that Tcl has provided an error msg.
         */
        setPtr = NULL;

    } else if (objc % 2 != 0) {
        /*
         * Set an error, if we can.
         */
        if (interp != NULL) {
            Ns_TclPrintfResult(interp, "list '%s' has to consist of an even number of elements",
                               Tcl_GetString(listObj));
        }
        setPtr = NULL;

    } else {
        TCL_SIZE_T i;

        setPtr = Ns_SetCreate(name);
        for (i = 0; i < objc; i += 2) {
            const char *keyString, *valueString;
            TCL_SIZE_T  keyLength, valueLength;

            keyString = Tcl_GetStringFromObj(objv[i], &keyLength);
            valueString = Tcl_GetStringFromObj(objv[i+1], &valueLength);
            Ns_SetPutSz(setPtr, keyString, keyLength, valueString, valueLength);
        }
    }

    return setPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSetObjCmd --
 *
 *      Implements "ns_set".
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
NsTclSetObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    NsInterp            *itPtr = clientData;
    Ns_Set              *set = NULL;
    Tcl_DString          ds;
    Tcl_HashTable       *tablePtr;
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;
    int                  opt, result = TCL_OK;

    static const char *const opts[] = {
        "array", "cleanup", "copy", "cput", "create",
        "delete", "delkey", "find", "free", "get",
        "icput", "idelkey", "ifind", "iget", "imerge",
        "isnull", "iunique", "iupdate", "key", "keys", "list",
        "merge", "move", "name", "new", "print", "put",
        "size", "split", "stats", "truncate", "unique", "update",
        "value", "values", NULL,
    };
    enum {
        SArrayIdx, SCleanupIdx, SCopyIdx, SCPutIdx, SCreateidx,
        SDeleteIdx, SDelkeyIdx, SFindIdx, SFreeIdx, SGetIdx,
        SICPutIdx, SIDelkeyIdx, SIFindIdx, SIGetIdx, SIMergeIdx,
        SIsNullIdx, SIUniqueIdx, SIUpdateIdx, SKeyIdx, SKeysIdx, SListIdx,
        SMergeIdx, SMoveIdx, sINameIdx, SNewIdx, SPrintIdx, SPutIdx,
        SSizeIdx, SSplitIdx, SStatsIdx, STruncateIdx, SUniqueIdx, SUpdateIdx,
        SValueIdx, SValuesIdx
    };

    if (unlikely(objc < 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "option ?arg ...?");
        return TCL_ERROR;
    }
    if (unlikely(Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
                                     &opt) != TCL_OK)) {
        return TCL_ERROR;
    }
    if (unlikely(opt == SCreateidx)) {
        opt = SNewIdx;
    }

    switch (opt) {
    case SCleanupIdx:
        tablePtr = &itPtr->sets;
        hPtr = Tcl_FirstHashEntry(tablePtr, &search);
        while (hPtr != NULL) {
            const char *key = Tcl_GetHashKey(tablePtr, hPtr);

            Ns_Log(Ns_LogNsSetDebug, "ns_set cleanup key <%s> dynamic %d",
                   key, IS_DYNAMIC(key));

            if (IS_DYNAMIC(key)) {
                set = Tcl_GetHashValue(hPtr);
                Ns_SetFree(set);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(tablePtr);
        Tcl_InitHashTable(tablePtr, TCL_STRING_KEYS);
        break;

    case SListIdx:
        {
            Tcl_Obj *listObj = Tcl_NewListObj(0, NULL);

            tablePtr = &itPtr->sets;
            for (hPtr = Tcl_FirstHashEntry(tablePtr, &search);
                 hPtr != NULL;
                 hPtr = Tcl_NextHashEntry(&search)
                 ) {
                const char *listKey = Tcl_GetHashKey(tablePtr, hPtr);
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(listKey, TCL_INDEX_NONE));
            }
            Tcl_SetObjResult(interp, listObj);
        }
        break;

    case SStatsIdx:
        {
            Tcl_Obj *resultObj = Tcl_NewListObj(0, NULL);
            size_t nr_dynamic = 0, size_dynamic = 0, allocated_dynamic = 0;
            size_t nr_static = 0,  size_static = 0,  allocated_static = 0;

            tablePtr = &itPtr->sets;
            for (hPtr = Tcl_FirstHashEntry(tablePtr, &search);
                 hPtr != NULL;
                 hPtr = Tcl_NextHashEntry(&search)
                 ) {
                const char *key = Tcl_GetHashKey(tablePtr, hPtr);
                set = (Ns_Set *) Tcl_GetHashValue(hPtr);
                if (IS_DYNAMIC(key)) {
                    nr_dynamic ++;
#ifdef NS_SET_DSTRING
                    allocated_dynamic += (size_t)set->data.spaceAvl;
                    size_dynamic += (size_t)set->data.length;
#endif
                } else {
                    nr_static++;
#ifdef NS_SET_DSTRING
                    allocated_static += (size_t)set->data.spaceAvl;
                    size_static += (size_t)set->data.length;
#endif
                }
            }

            Tcl_DictObjPut(NULL, resultObj,
                        Tcl_NewStringObj("nr_dynamic", 10),
                        Tcl_NewWideIntObj((Tcl_WideInt)nr_dynamic));
            Tcl_DictObjPut(NULL, resultObj,
                        Tcl_NewStringObj("size_dynamic", 12),
                        Tcl_NewWideIntObj((Tcl_WideInt)size_dynamic));
            Tcl_DictObjPut(NULL, resultObj,
                        Tcl_NewStringObj("allocated_dynamic", 17),
                        Tcl_NewWideIntObj((Tcl_WideInt)allocated_dynamic));

            Tcl_DictObjPut(NULL, resultObj,
                        Tcl_NewStringObj("nr_static", 9),
                        Tcl_NewWideIntObj((Tcl_WideInt)nr_static));
            Tcl_DictObjPut(NULL, resultObj,
                        Tcl_NewStringObj("size_static", 11),
                        Tcl_NewWideIntObj((Tcl_WideInt)size_static));
            Tcl_DictObjPut(NULL, resultObj,
                        Tcl_NewStringObj("allocated_static", 16),
                        Tcl_NewWideIntObj((Tcl_WideInt)allocated_static));

            Tcl_SetObjResult(interp, resultObj);
        }
        break;




    case SNewIdx:   NS_FALL_THROUGH; /* fall through */
    case SCopyIdx:  NS_FALL_THROUGH; /* fall through */
    case SSplitIdx: {
        TCL_OBJC_T    offset = 2;
        const char   *name;

        /*
         * The following commands create new sets.
         */

        switch (opt) {
        case SNewIdx:
            if (objc % 2 == 0) {
                /*
                 * No name provided.
                 */
                name = NULL;
            } else {
                name = Tcl_GetString(objv[offset++]);
            }
            set = Ns_SetCreate(name);
            while (offset < objc) {
                const char *keyString, *valueString;
                TCL_SIZE_T  keyLength, valueLength;

                keyString = Tcl_GetStringFromObj(objv[offset++], &keyLength);
                valueString = Tcl_GetStringFromObj(objv[offset++], &valueLength);
                (void)Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
            }
            Tcl_SetObjResult(interp, EnterSet(itPtr, set, NS_TCL_SET_DYNAMIC));
            break;

        case SCopyIdx:
            if (unlikely(offset >= objc)) {
                Tcl_WrongNumArgs(interp, 2, objv, "setId");
                result = TCL_ERROR;
            } else if (LookupObjSet(itPtr, objv[offset], NS_FALSE, &set) != TCL_OK) {
                result = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, EnterSet(itPtr, Ns_SetCopy(set), NS_TCL_SET_DYNAMIC));
            }
            break;

        case SSplitIdx: {
            if (unlikely((objc - offset) < 1)) {
                Tcl_WrongNumArgs(interp, 2, objv, "setId ?splitChar");
                result = TCL_ERROR;

            } else if (LookupObjSet(itPtr, objv[offset++], NS_FALSE, &set) != TCL_OK) {
                result = TCL_ERROR;

            } else {
                Tcl_Obj     *listObj = Tcl_NewListObj(0, NULL);
                Ns_Set     **sets;
                const char  *split;
                TCL_OBJC_T   i;

                split = (offset < objc) ? Tcl_GetString(objv[offset]) : ".";
                sets = Ns_SetSplit(set, *split);
                for (i = 0; sets[i] != NULL; i++) {
                    Tcl_ListObjAppendElement(interp, listObj,
                                             EnterSet(itPtr, sets[i], NS_TCL_SET_DYNAMIC));
                }
                Tcl_SetObjResult(interp, listObj);
                ns_free(sets);
            }
            break;
        }

        default:
            /* unexpected value */
            assert(opt && 0);
            break;
        }
        break;
    }

    default:
        /*
         * All further commands require a valid set.
         */

        if (unlikely(objc < 3)) {
            Tcl_WrongNumArgs(interp, 2, objv, "setId ?args?");
            result = TCL_ERROR;

        } else if (unlikely(LookupObjSet(itPtr, objv[2], NS_FALSE, &set) != TCL_OK)) {
            result = TCL_ERROR;

        } else {
            Tcl_Obj *objPtr;

            switch (opt) {
            case SArrayIdx:  NS_FALL_THROUGH; /* fall through */
            case sINameIdx:  NS_FALL_THROUGH; /* fall through */
            case SPrintIdx:  NS_FALL_THROUGH; /* fall through */
            case SFreeIdx:
                /*
                 * These commands require only the set.
                 */

                if (unlikely(objc != 3)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId");
                    result = TCL_ERROR;

                } else {

                    switch (opt) {
                    case SArrayIdx:
                        {
                            Tcl_DStringInit(&ds);
                            Ns_DStringAppendSet(&ds, set);
                            Tcl_DStringResult(interp, &ds);
                            break;
                        }

                    case sINameIdx:
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(set->name, TCL_INDEX_NONE));
                        break;

                    case SKeysIdx: {
                        if (unlikely(objc > 5)) {
                            Tcl_WrongNumArgs(interp, 2, objv, "setId ?pattern?");
                            result = TCL_ERROR;
                        } else {
                            size_t i;
                            const char *pattern = (objc == 4 ? Tcl_GetString(objv[3]) : NULL);

                            Tcl_DStringInit(&ds);
                            for (i = 0u; i < set->size; ++i) {
                                const char *value = (set->fields[i].name != NULL ? set->fields[i].name : "");
                                if (pattern == NULL || (Tcl_StringMatch(value, pattern) != 0)) {
                                    Tcl_DStringAppendElement(&ds, value);
                                }
                            }
                            Tcl_DStringResult(interp, &ds);
                        }
                        break;
                    }

                    case SPrintIdx:
                        Ns_SetPrint(NULL, set);
                        break;

                    case SFreeIdx:
                        (void) Ns_TclFreeSet(interp, Tcl_GetString(objv[2]));
                        break;

                    default:
                        /* unexpected value */
                        assert(opt && 0);
                        break;
                    }
                }
                break;

            case SKeysIdx:   NS_FALL_THROUGH; /* fall through */
            case SValuesIdx: {
                if (unlikely(objc > 5)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId ?pattern?");
                    result = TCL_ERROR;
                } else {
                    size_t i;
                    const char *pattern = (objc == 4 ? Tcl_GetString(objv[3]) : NULL);

                    Tcl_DStringInit(&ds);
                    for (i = 0u; i < set->size; ++i) {
                        const char *value = (opt == SKeysIdx ? set->fields[i].name : set->fields[i].value);
                        if (value == NULL) {
                            value = "";
                        }
                        if (pattern == NULL || (Tcl_StringMatch(value, pattern) != 0)) {
                            Tcl_DStringAppendElement(&ds, value);
                        }
                    }
                    Tcl_DStringResult(interp, &ds);
                }
                break;
            }

            case SSizeIdx: {
                long elements = 0, bufferSize = 0;

                if (unlikely(objc < 3 || objc > 5)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId ?elements? ?bufferSize?");
                    result = TCL_ERROR;

                } else if (objc == 3) {
                    objPtr = Tcl_NewLongObj((long)Ns_SetSize(set));
                    Tcl_SetObjResult(interp, objPtr);

                } else {
                    if (Tcl_GetLongFromObj(interp, objv[3], &elements) != TCL_OK
                        || elements < 1) {
                        Ns_TclPrintfResult(interp, "invalid integer value for number of elements '%s'",
                                           Tcl_GetString(objv[3]));
                        result = TCL_ERROR;

                    } else if (objc == 5
                               && (Tcl_GetLongFromObj(interp, objv[4], &bufferSize) != TCL_OK
                                   || bufferSize < 1)) {
                        Ns_TclPrintfResult(interp, "invalid integer value for buffer size '%s'",
                                           Tcl_GetString(objv[4]));
                        result = TCL_ERROR;

                    } else {
                        NsSetResize(set, (size_t)elements, (int)bufferSize);
                        objPtr = Tcl_NewLongObj((long)Ns_SetSize(set));
                        Tcl_SetObjResult(interp, objPtr);
                    }
                }
                break;
            }

            case SGetIdx:  NS_FALL_THROUGH; /* fall through */
            case SIGetIdx: {
                char *keyString = NULL, *defaultString = NULL;
                int   all = 0;
                Ns_ObjvSpec argOpts[] = {
                    {"-all",  Ns_ObjvBool,  &all,  INT2PTR(NS_TRUE)},
                    {NULL, NULL, NULL, NULL}
                };
                Ns_ObjvSpec  args[] = {
                    {"setId",       Ns_ObjvString, &keyString, NULL},
                    {"?default",    Ns_ObjvString, &defaultString, NULL},
                    {NULL, NULL, NULL, NULL}
                };

                if (Ns_ParseObjv2(argOpts, args, interp, 2, 1, objc, objv) != NS_OK) {
                    //NsWrongNumArgs(argOpts, args, interp, 2, objv, objc-1, objv+1);
                    result = TCL_ERROR;
                } else {
                    Ns_DList    dl, *dlPtr = &dl;
                    size_t      count;

                    Ns_DListInit(dlPtr);

                    count = NsSetGetCmpDListAppend(set, keyString, all, opt == SGetIdx ? strcmp : strcasecmp, dlPtr);

                    if (count == 0) {
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(defaultString, TCL_INDEX_NONE));
                    } else if (all == NS_FALSE) {
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(dlPtr->data[0], TCL_INDEX_NONE));
                    } else {
                        Tcl_Obj *resultObj = Tcl_NewListObj((TCL_SIZE_T)count, NULL);
                        size_t   i;

                        for (i = 0u; i<count; i++) {
                            Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(dlPtr->data[i], TCL_INDEX_NONE));
                        }
                        Tcl_SetObjResult(interp, resultObj);
                    }

                    Ns_DListFree(dlPtr);
                }
                break;
            }

            case SFindIdx:    NS_FALL_THROUGH; /* fall through */
            case SIFindIdx:   NS_FALL_THROUGH; /* fall through */
            case SDelkeyIdx:  NS_FALL_THROUGH; /* fall through */
            case SIDelkeyIdx: NS_FALL_THROUGH; /* fall through */
            case SUniqueIdx:  NS_FALL_THROUGH; /* fall through */
            case SIUniqueIdx:
                /*
                 * These commands require a set and string key.
                 */

                if (unlikely(objc != 4)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId key");
                    result = TCL_ERROR;

                } else {
                    const char *key = Tcl_GetString(objv[3]);

                    switch (opt) {
                    case SIFindIdx:
                        objPtr = Tcl_NewIntObj(Ns_SetIFind(set, key));
                        Tcl_SetObjResult(interp, objPtr);
                        break;

                    case SFindIdx:
                        objPtr = Tcl_NewIntObj(Ns_SetFind(set, key));
                        Tcl_SetObjResult(interp, objPtr);
                        break;

                    case SIDelkeyIdx:
                        Ns_SetIDeleteKey(set, key);
                        break;

                    case SDelkeyIdx:
                        Ns_SetDeleteKey(set, key);
                        break;

                    case SUniqueIdx:
                        objPtr = Tcl_NewBooleanObj(Ns_SetUnique(set, key));
                        Tcl_SetObjResult(interp, objPtr);
                        break;

                    case SIUniqueIdx:
                        objPtr = Tcl_NewBooleanObj(Ns_SetIUnique(set, key));
                        Tcl_SetObjResult(interp, objPtr);
                        break;

                    default:
                        /* unexpected value */
                        assert(opt && 0);
                        break;
                    }
                }
                break;

            case SValueIdx:   NS_FALL_THROUGH; /* fall through */
            case SIsNullIdx:  NS_FALL_THROUGH; /* fall through */
            case SKeyIdx:     NS_FALL_THROUGH; /* fall through */
            case SDeleteIdx:  NS_FALL_THROUGH; /* fall through */
            case STruncateIdx: {
                /*
                 * These commands require a set and key/value index.
                 */
                Ns_ObjvValueRange idxRange = {0, (Tcl_WideInt)Ns_SetSize(set)};
                int               i;
                TCL_SIZE_T        oc = 1;
                Ns_ObjvSpec       spec = {"?idx", Ns_ObjvInt, &i, &idxRange};

                if (unlikely(objc != 4)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId index");
                    result = TCL_ERROR;

                } else if (Ns_ObjvInt(&spec, interp, &oc, &objv[3]) != TCL_OK) {
                    result = TCL_ERROR;

                } else {
                    const char *val;

                    switch (opt) {
                    case SValueIdx:
                        val = Ns_SetValue(set, i);
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(val, TCL_INDEX_NONE));
                        break;

                    case SIsNullIdx:
                        val = Ns_SetValue(set, i);
                        objPtr = Tcl_NewBooleanObj((val != NULL) ? 0 : 1);
                        Tcl_SetObjResult(interp, objPtr);
                        break;

                    case SKeyIdx:
                        val = Ns_SetKey(set, i);
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(val, TCL_INDEX_NONE));
                        break;

                    case SDeleteIdx:
                        Ns_SetDelete(set, i);
                        break;

                    case STruncateIdx:
                        Ns_SetTrunc(set, (size_t)i);
                        break;

                    default:
                        /* unexpected value */
                        assert(opt && 0);
                        break;
                    }
                }
                break;
            }

            case SPutIdx:     NS_FALL_THROUGH; /* fall through */
            case SUpdateIdx:  NS_FALL_THROUGH; /* fall through */
            case SIUpdateIdx: NS_FALL_THROUGH; /* fall through */
            case SCPutIdx:    NS_FALL_THROUGH; /* fall through */
            case SICPutIdx:
                /*
                 * These commands require a set, key, and value.
                 */

                if (unlikely(objc != 5)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId key value");
                    result = TCL_ERROR;
                } else {
                    int         i;
                    const char *keyString, *valueString;
                    TCL_SIZE_T  keyLength, valueLength;

                    keyString = Tcl_GetStringFromObj(objv[3], &keyLength);
                    valueString = Tcl_GetStringFromObj(objv[4], &valueLength);

                    switch (opt) {
                    case SUpdateIdx:
                        i = (int)Ns_SetUpdateSz(set, keyString, keyLength, valueString, valueLength);
                        break;

                    case SIUpdateIdx:
                        i = (int)Ns_SetIUpdateSz(set, keyString, keyLength, valueString, valueLength);
                        break;

                    case SICPutIdx:
                        i = Ns_SetIFind(set, keyString);
                        if (i < 0) {
                            i = (int)Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
                        }
                        break;

                    case SCPutIdx:
                        i = Ns_SetFind(set, keyString);
                        if (i < 0) {
                            i = (int)Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
                        }
                        break;

                    case SPutIdx:
                        i = (int)Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
                        break;

                    default:
                        /* should not happen */
                        assert(opt && 0);
                        i = 0;
                        break;
                    }
                    objPtr = Tcl_NewIntObj(i);
                    Tcl_SetObjResult(interp, objPtr);
                }
                break;

            case SIMergeIdx: NS_FALL_THROUGH; /* fall through */
            case SMergeIdx:  NS_FALL_THROUGH; /* fall through */
            case SMoveIdx:
                /*
                 * These commands require two sets.
                 */

                if (unlikely(objc != 4)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setTo setFrom");
                    result = TCL_ERROR;
                } else {
                    Ns_Set *set2Ptr = NULL;

                    if (unlikely(LookupObjSet(itPtr, objv[3], NS_FALSE, &set2Ptr) != TCL_OK)) {
                        result = TCL_ERROR;
                    } else {
                        assert (set2Ptr != NULL);
                        if (opt == SIMergeIdx) {
                            Ns_SetIMerge(set, set2Ptr);
                        } else if (opt == SMergeIdx) {
                            Ns_SetMerge(set, set2Ptr);
                        } else {
                            Ns_SetMove(set, set2Ptr);
                        }
                        Tcl_SetObjResult(interp, objv[2]);
                    }
                }
                break;

            default:
                /* unexpected value */
                assert(opt && 0);
                break;
            }
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclParseHeaderObjCmd --
 *
 *      Implements "ns_parseheader". Consume a header line, handling header
 *      continuation, placing results in given set.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Parse an HTTP header and add it to an existing set; see
 *      Ns_ParseHeader.
 *
 *----------------------------------------------------------------------
 */

int
NsTclParseHeaderObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    NsInterp    *itPtr = clientData;
    int          result = TCL_OK;
    Ns_Set      *set = NULL;
    Ns_HeaderCaseDisposition disp = Preserve;
    char        *setString = (char *)NS_EMPTY_STRING,
                *headerString = (char *)NS_EMPTY_STRING,
                *dispositionString = NULL,
                *prefix = NULL;
    Ns_ObjvSpec opts[] = {
        {"-prefix",  Ns_ObjvString,  &prefix,  NULL},
        {NULL, NULL, NULL, NULL}
    };

    Ns_ObjvSpec  args[] = {
        {"set",          Ns_ObjvString, &setString, NULL},
        {"headerline",   Ns_ObjvString, &headerString, NULL},
        {"?disposition", Ns_ObjvString, &dispositionString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    assert(clientData != NULL);

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (LookupSet(itPtr, setString, NS_FALSE, &set) != TCL_OK) {
        result = TCL_ERROR;

    } else if (objc < 4) {
        disp = ToLower;
    } else if (dispositionString != NULL) {
        if (STREQ(dispositionString, "toupper")) {
            disp = ToUpper;
        } else if (STREQ(dispositionString, "tolower")) {
            disp = ToLower;
        } else if (STREQ(dispositionString, "preserve")) {
            disp = Preserve;
        } else {
            Ns_TclPrintfResult(interp, "invalid disposition \"%s\": should be toupper, tolower, or preserve",
                               dispositionString);
            result = TCL_ERROR;
        }
    } else {
        Ns_Fatal("error in argument parser: dispositionString should never be NULL");
    }

    if (result == TCL_OK) {
        size_t fieldNumber;

        assert(set != NULL);
        if (Ns_ParseHeader(set, headerString, prefix, disp, &fieldNumber) != NS_OK) {
            Ns_TclPrintfResult(interp, "invalid header: %s", headerString);
            result = TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)fieldNumber));
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * EnterSet --
 *
 *      Add an Ns_Set to an interp, creating a new unique id.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
EnterSet(NsInterp *itPtr, Ns_Set *set, Ns_TclSetType type)
{
    Tcl_HashTable  *tablePtr;
    Tcl_HashEntry  *hPtr;
    int             isNew;
    TCL_SIZE_T      len;
    uint32_t        next;
    char            buf[TCL_INTEGER_SPACE + 1];

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    tablePtr = &itPtr->sets;
    buf[0] = (type == NS_TCL_SET_DYNAMIC) ? SET_DYNAMIC : SET_STATIC;

    /*
     * Allocate a new set IDs until we find an unused one.
     */
    for (next = (uint32_t)tablePtr->numEntries; ; ++ next) {
        len = (TCL_SIZE_T)ns_uint32toa(buf+1, next);
        hPtr = Tcl_CreateHashEntry(tablePtr, buf, &isNew);
        if (isNew != 0) {
            break;
        }
    }

    Tcl_SetHashValue(hPtr, set);

    Ns_Log(Ns_LogNsSetDebug, "EnterSet %p with name '%s'",
           (void*)set, buf);

    return Tcl_NewStringObj(buf, len+1);
}


/*
 *----------------------------------------------------------------------
 *
 * LookupSet --
 *
 *      Take a Tcl set handle and return a matching Set.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      If deleteEntry is set, then the hash entry will be removed.
 *      Set will be returned in given setPtr.
 *
 *----------------------------------------------------------------------
 */

static int
LookupObjSet(NsInterp *itPtr, Tcl_Obj *idPtr, bool deleteEntry, Ns_Set **setPtr)
{
    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(idPtr != NULL);
    NS_NONNULL_ASSERT(setPtr != NULL);

    return LookupSet(itPtr, Tcl_GetString(idPtr), deleteEntry, setPtr);
}

static int
LookupInterpSet(Tcl_Interp *interp, const char *id, bool deleteEntry, Ns_Set **setPtr)
{
    NsInterp *itPtr;
    int       result;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(id != NULL);
    NS_NONNULL_ASSERT(setPtr != NULL);

    itPtr = NsGetInterpData(interp);
    if (unlikely(itPtr == NULL)) {
        Ns_TclPrintfResult(interp, "ns_set not supported");
        result = TCL_ERROR;
    } else {
        result = LookupSet(itPtr, id, deleteEntry, setPtr);
    }

    return result;
}

static int
LookupSet(NsInterp *itPtr, const char *id, bool deleteEntry, Ns_Set **setPtr)
{
    Tcl_HashEntry *hPtr;
    Ns_Set        *set = NULL;
    int            result;

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(id != NULL);
    NS_NONNULL_ASSERT(setPtr != NULL);

    hPtr = Tcl_FindHashEntry(&itPtr->sets, id);
    if (likely(hPtr != NULL)) {
        set = (Ns_Set *) Tcl_GetHashValue(hPtr);
        if (unlikely(deleteEntry)) {
            Tcl_DeleteHashEntry(hPtr);
        }
    }
    if (unlikely(set == NULL)) {
        Ns_TclPrintfResult(itPtr->interp, "no such set: %s", id);
        result = TCL_ERROR;
    } else {
        *setPtr = set;
        result = TCL_OK;
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
