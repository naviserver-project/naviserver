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
 * tclset.c --
 *
 *      Implements the Tcl ns_set commands
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

static int LookupSet(NsInterp *itPtr, CONST char *id, bool deleteEntry, Ns_Set **setPtr)
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
 *      last argument determines the the lifespan of the Ns_Set. When the type
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
        Ns_TclPrintfResult(interp, "ns_set not supported");
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
 * NsTclSetObjCmd --
 *
 *      Implelments ns_set.
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
NsTclSetObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp            *itPtr = clientData;
    Ns_Set              *set = NULL;
    const char          *key, *val;
    Tcl_DString          ds;
    Tcl_HashTable       *tablePtr;
    const Tcl_HashEntry *hPtr;
    Tcl_HashSearch       search;
    int                  opt, result = TCL_OK;

    static const char *const opts[] = {
        "array", "cleanup", "copy", "cput", "create", "delete",
        "delkey", "find", "free", "get", "icput",
        "idelkey", "ifind", "iget", "isnull", "iunique", "key",
        "list", "merge", "move", "name", "new", "print",
        "put", "size", "split", "truncate", "unique", "update",
        "value", NULL,
    };
    enum {
        SArrayIdx, SCleanupIdx, SCopyIdx, SCPutIdx, SCreateidx,
        SDeleteIdx, SDelkeyIdx, SFindIdx, SFreeIdx, SGetIdx,
        SICPutIdx, SIDelkeyIdx, SIFindIdx, SIGetIdx,
        SIsNullIdx, SIUniqueIdx, SKeyIdx, SListIdx, SMergeIdx,
        SMoveIdx, sINameIdx, SNewIdx, SPrintIdx, SPutIdx,
        SSizeIdx, SSplitIdx, STruncateIdx, SUniqueIdx, SUpdateIdx,
        SValueIdx
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
            key = Tcl_GetHashKey(tablePtr, hPtr);
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
                Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(listKey, -1));
            }
            Tcl_SetObjResult(interp, listObj);
        }
        break;

    case SNewIdx:
    case SCopyIdx:
    case SSplitIdx: {
        int           offset = 2;
        const char   *name;

        /*
         * The following commands create new sets.
         */

        switch (opt) {
        case SNewIdx:
            name = (offset < objc) ? Tcl_GetString(objv[offset++]) : NULL;
            set = Ns_SetCreate(name);
            while (offset < objc) {
                key = Tcl_GetString(objv[offset++]);
                val = (offset < objc) ? Tcl_GetString(objv[offset++]) : NULL;
                (void)Ns_SetPut(set, key, val);
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
                int          i;

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
            case SArrayIdx:
            case SSizeIdx:
            case sINameIdx:
            case SPrintIdx:
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
                            size_t i;

                            Tcl_DStringInit(&ds);
                            for (i = 0u; i < Ns_SetSize(set); ++i) {
                                Tcl_DStringAppendElement(&ds, Ns_SetKey(set, i));
                                Tcl_DStringAppendElement(&ds, Ns_SetValue(set, i));
                            }
                            Tcl_DStringResult(interp, &ds);
                            break;
                        }

                    case SSizeIdx:
                        objPtr = Tcl_NewLongObj((long)Ns_SetSize(set));
                        Tcl_SetObjResult(interp, objPtr);
                        break;

                    case sINameIdx:
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(set->name, -1));
                        break;

                    case SPrintIdx:
                        Ns_SetPrint(set);
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

            case SGetIdx:
            case SIGetIdx:
                if (unlikely(objc < 4)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId key ?default?");
                    result = TCL_ERROR;
                } else {
                    const char *def = (objc > 4 ? Tcl_GetString(objv[4]) : NULL);

                    key = Tcl_GetString(objv[3]);

                    switch (opt) {
                    case SGetIdx:
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_SetGetValue(set, key, def), -1));
                        break;

                    case SIGetIdx:
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_SetIGetValue(set, key, def), -1));
                        break;

                    default:
                        /* unexpected value */
                        assert(opt && 0);
                        break;
                    }
                }
                break;

            case SFindIdx:
            case SIFindIdx:
            case SDelkeyIdx:
            case SIDelkeyIdx:
            case SUniqueIdx:
            case SIUniqueIdx:
                /*
                 * These commands require a set and string key.
                 */

                if (unlikely(objc != 4)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId key");
                    result = TCL_ERROR;

                } else {
                    key = Tcl_GetString(objv[3]);

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

            case SValueIdx:
            case SIsNullIdx:
            case SKeyIdx:
            case SDeleteIdx:
            case STruncateIdx: {
                /*
                 * These commands require a set and key/value index.
                 */
                int i;

                if (unlikely(objc != 4)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId index");
                    result = TCL_ERROR;

                } else if (unlikely(Tcl_GetIntFromObj(interp, objv[3], &i) != TCL_OK)) {
                    result = TCL_ERROR;

                } else if (unlikely(i < 0)) {
                    Ns_TclPrintfResult(interp, "invalid index %d: must be >= 0", i);
                    result = TCL_ERROR;

                } else if (unlikely((size_t)i >= Ns_SetSize(set))) {
                    Ns_TclPrintfResult(interp, "invalid index %d: beyond range of set fields", i);
                    result = TCL_ERROR;

                } else {
                    switch (opt) {
                    case SValueIdx:
                        val = Ns_SetValue(set, i);
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(val, -1));
                        break;

                    case SIsNullIdx:
                        val = Ns_SetValue(set, i);
                        objPtr = Tcl_NewBooleanObj((val != NULL) ? 0 : 1);
                        Tcl_SetObjResult(interp, objPtr);
                        break;

                    case SKeyIdx:
                        key = Ns_SetKey(set, i);
                        Tcl_SetObjResult(interp, Tcl_NewStringObj(key, -1));
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

            case SPutIdx:
            case SUpdateIdx:
            case SCPutIdx:
            case SICPutIdx:
                /*
                 * These commands require a set, key, and value.
                 */

                if (unlikely(objc != 5)) {
                    Tcl_WrongNumArgs(interp, 2, objv, "setId key value");
                    result = TCL_ERROR;
                } else {
                    int i;

                    key = Tcl_GetString(objv[3]);
                    val = Tcl_GetString(objv[4]);

                    switch (opt) {
                    case SUpdateIdx:
                        Ns_SetDeleteKey(set, key);
                        i = (int)Ns_SetPut(set, key, val);
                        break;

                    case SICPutIdx:
                        i = Ns_SetIFind(set, key);
                        if (i < 0) {
                            i = (int)Ns_SetPut(set, key, val);
                        }
                        break;

                    case SCPutIdx:
                        i = Ns_SetFind(set, key);
                        if (i < 0) {
                            i = (int)Ns_SetPut(set, key, val);
                        }
                        break;

                    case SPutIdx:
                        i = (int)Ns_SetPut(set, key, val);
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

            case SMergeIdx:
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
                        if (opt == SMergeIdx) {
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
 *      This wraps Ns_ParseHeader.
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
NsTclParseHeaderObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    NsInterp    *itPtr = clientData;
    int          result = TCL_OK;
    Ns_Set      *set = NULL;
    Ns_HeaderCaseDisposition disp = Preserve;
    char        *setString, *headerString, *dispositionString;
    Ns_ObjvSpec  args[] = {
        {"set", Ns_ObjvString, &setString, NULL},
        {"header", Ns_ObjvString, &headerString, NULL},
        {"?disposition", Ns_ObjvString, &dispositionString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    assert(clientData != NULL);

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (LookupSet(itPtr, setString, NS_FALSE, &set) != TCL_OK) {
        result = TCL_ERROR;

    } else if (objc < 4) {
        disp = ToLower;
    } else if (STREQ(dispositionString, "toupper")) {
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

    if (result == TCL_OK) {
        assert(set != NULL);

        if (Ns_ParseHeader(set, headerString, disp) != NS_OK) {
            Ns_TclPrintfResult(interp, "invalid header: %s", headerString);
            result = TCL_ERROR;
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
    int             next;
    char            typeChar;
    char            buf[TCL_INTEGER_SPACE + 1];

    NS_NONNULL_ASSERT(itPtr != NULL);
    NS_NONNULL_ASSERT(set != NULL);

    tablePtr = &itPtr->sets;
    typeChar = (type == NS_TCL_SET_DYNAMIC) ? SET_DYNAMIC : SET_STATIC;

    /*
     * Allocate a new set IDs until we find an unused one.
     */

    next = tablePtr->numEntries;
    do {
        snprintf(buf, sizeof(buf), "%c%d", typeChar, next);
        ++next;
        hPtr = Tcl_CreateHashEntry(tablePtr, buf, &isNew);
    } while (isNew == 0);

    Tcl_SetHashValue(hPtr, set);
    return Tcl_NewStringObj(buf, -1);
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
LookupSet(NsInterp *itPtr, CONST char *id, bool deleteEntry, Ns_Set **setPtr)
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
