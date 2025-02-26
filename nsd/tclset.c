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

static Ns_ObjvValueRange maxIdxRange = {0, LLONG_MAX};

static TCL_OBJCMDPROC_T SetArrayObjCmd;
static TCL_OBJCMDPROC_T SetCleanupObjCmd;
static TCL_OBJCMDPROC_T SetCopyObjCmd;
static TCL_OBJCMDPROC_T SetCreateObjCmd;
static TCL_OBJCMDPROC_T SetDeleteObjCmd;
static TCL_OBJCMDPROC_T SetDelkeyObjCmd;
static TCL_OBJCMDPROC_T SetFindObjCmd;
static TCL_OBJCMDPROC_T SetFreeObjCmd;
static TCL_OBJCMDPROC_T SetFormatObjCmd;
static TCL_OBJCMDPROC_T SetGetObjCmd;
static TCL_OBJCMDPROC_T SetIsnullObjCmd;
static TCL_OBJCMDPROC_T SetKeyObjCmd;
static TCL_OBJCMDPROC_T SetKeysObjCmd;
static TCL_OBJCMDPROC_T SetListObjCmd;
static TCL_OBJCMDPROC_T SetMergeObjCmd;
static TCL_OBJCMDPROC_T SetMoveObjCmd;
static TCL_OBJCMDPROC_T SetNameObjCmd;
#ifdef NS_WITH_DEPRECATED
static TCL_OBJCMDPROC_T SetPrintObjCmd;
#endif
static TCL_OBJCMDPROC_T SetPutObjCmd;
static TCL_OBJCMDPROC_T SetSizeObjCmd;
static TCL_OBJCMDPROC_T SetSplitObjCmd;
static TCL_OBJCMDPROC_T SetStatsObjCmd;
static TCL_OBJCMDPROC_T SetTruncateObjCmd;
static TCL_OBJCMDPROC_T SetUniqueObjCmd;
static TCL_OBJCMDPROC_T SetValueObjCmd;
static TCL_OBJCMDPROC_T SetValuesObjCmd;
static TCL_OBJCMDPROC_T Set_TYPE_SetidKeyValueObjCmd;

/*
 * Local functions defined in this file
 */

static int LookupSet(NsInterp *itPtr, const char *id, bool deleteEntry, Ns_Set **setPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static int LookupInterpSet(Tcl_Interp *interp, const char *id, bool deleteEntry, Ns_Set **setPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);
static Tcl_Obj *EnterSet(NsInterp *itPtr, Ns_Set *set, Ns_TclSetType type)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int StartsWithI(const char *nameString)
    NS_GNUC_NONNULL(1) NS_GNUC_PURE;



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
Ns_SetCreateFromDict(Tcl_Interp *interp, const char *name, Tcl_Obj *listObj, unsigned int flags)
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
        setPtr->flags = flags;

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
 * StartsWithI --
 *
 *      Helper function to determine whether the provided string (intended to
 *      be the name of the subcommand) starts with an i. If so, the function
 *      returns 1.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */
static int StartsWithI(const char *nameString) {
    return (*nameString == 'i');
}


/*
 *----------------------------------------------------------------------
 * SetArrayObjCmd --
 *
 *      This command implements "ns_set array". It converts the given Ns_Set
 *      object into a Tcl key-value list by extracting its content and
 *      returns this list as the command result.
 *
 * Results:
 *      The Tcl list representing the set's contents.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetArrayObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        Ns_DStringAppendSet(&ds, set);
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetCleanupObjCmd --
 *
 *      This command implements "ns_set cleanup". It scans through all the
 *      dynamically allocated ns_set objects managed by the interpreter,
 *      frees those marked as dynamic, and reinitializes the interpreter's
 *      set hash table.
 *
 * Results:
 *      TCL_OK if the cleanup succeeds, TCL_ERROR otherwise.
 *
 * Side effects:
 *      Frees memory for dynamic sets and resets the set hash table.
 *
 *----------------------------------------------------------------------
 */
int SetCleanupObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int       result = TCL_OK;
    NsInterp *itPtr = clientData;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_HashTable       *tablePtr;
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;

        tablePtr = &itPtr->sets;
        hPtr = Tcl_FirstHashEntry(tablePtr, &search);
        while (hPtr != NULL) {
            const char *key = Tcl_GetHashKey(tablePtr, hPtr);

            Ns_Log(Ns_LogNsSetDebug, "ns_set cleanup key <%s> dynamic %d",
                   key, IS_DYNAMIC(key));

            if (IS_DYNAMIC(key)) {
                Ns_Set *set = Tcl_GetHashValue(hPtr);
                Ns_SetFree(set);
            }
            hPtr = Tcl_NextHashEntry(&search);
        }
        Tcl_DeleteHashTable(tablePtr);
        Tcl_InitHashTable(tablePtr, TCL_STRING_KEYS);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetCopyObjCmd --
 *
 *      This command implements "ns_set copy". It creates an independent copy
 *      of the specified ns_set, including all its key-value pairs, and
 *      registers the new set as dynamic with the Tcl interpreter.
 *
 * Results:
 *      The handle of the newly created dynamic set.
 *
 * Side effects:
 *      Allocates memory for the new set and registers it with the interpreter.
 *
 *----------------------------------------------------------------------
 */
static int SetCopyObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    NsInterp   *itPtr = clientData;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, EnterSet(itPtr, Ns_SetCopy(set), NS_TCL_SET_DYNAMIC));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetCreateObjCmd --
 *
 *      This command implements "ns_set create". It creates a new ns_set from
 *      the provided key-value pairs. An optional set name may be specified;
 *      if omitted, a unique name is generated. The new set is registered as
 *      dynamic with the interpreter.
 *
 * Results:
 *      The handle of the newly created set.
 *
 * Side effects:
 *      Allocates and initializes a new Ns_Set structure.
 *
 *----------------------------------------------------------------------
 */
static int SetCreateObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int       result = TCL_OK, nargs = 0, nocase = 0;
    NsInterp *itPtr = clientData;
    Ns_ObjvSpec opts[] = {
        {"-nocase", Ns_ObjvBool,  &nocase, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"?arg",     Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };

#ifdef NS_WITH_DEPRECATED
    const char  *subcmdName = Tcl_GetString(objv[1]);

    if (*subcmdName == 'n' && strcmp(subcmdName, "new") == 0) {
        Ns_LogDeprecated(objv, 2, "ns_set create ...", NULL);
    }
#endif

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const char *name;
        TCL_SIZE_T offset = objc-nargs;
        Ns_Set    *set;

        if (nargs % 2 == 0 || nargs < 1) {
            /*
             * No name provided.
             */
            name = NULL;
        } else {
            name = Tcl_GetString(objv[offset++]);
        }
        set = Ns_SetCreate(name);
        if (nocase != 0) {
            set->flags |= NS_SET_OPTION_NOCASE;
        }
        while (offset < objc) {
            const char *keyString, *valueString;
            TCL_SIZE_T  keyLength, valueLength;

            keyString = Tcl_GetStringFromObj(objv[offset++], &keyLength);
            valueString = Tcl_GetStringFromObj(objv[offset++], &valueLength);
            (void)Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
        }
        Tcl_SetObjResult(interp, EnterSet(itPtr, set, NS_TCL_SET_DYNAMIC));
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 * SetDeleteObjCmd --
 *
 *      This command implements "ns_set delete". It deletes the element at the
 *      specified index in the given ns_set, freeing associated data if
 *      necessary.
 *
 * Results:
 *      TCL_OK on success or TCL_ERROR on failure.
 *
 * Side effects:
 *      The specified element is removed from the set.
 *
 *----------------------------------------------------------------------
 */
static int SetDeleteObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       Ns_ObjvSet,  &set, NULL},
        {"fieldNumber", Ns_ObjvLong, &idx, &maxIdxRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (idx > (long)Ns_SetSize(set)) {
        Ns_TclPrintfResult(interp, "ns_set %s: index must be maximal the number of elements",
                           Tcl_GetString(objv[1]));
        result = TCL_ERROR;

    } else {
        Ns_SetDelete(set, idx);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * SetDelkeyObjCmd --
 *
 *      This command implements "ns_set delkey". It removes a key from the
 *      specified ns_set. The function determines whether to perform a case‐
 *      sensitive or case‐insensitive deletion based on the key prefix.
 *
 * Results:
 *      Returns a standard Tcl result indicating success (TCL_OK) or failure
 *      (TCL_ERROR).
 *
 * Side effects:
 *      The key (and its associated value) is removed from the ns_set.
 *
 *----------------------------------------------------------------------
 */
static int SetDelkeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *keyString = NULL;
    int         nocase = 0, result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec opts[] = {
        {"-nocase", Ns_ObjvBool,  &nocase, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"setId",       Ns_ObjvSet,    &set, NULL},
        {"key",         Ns_ObjvString, &keyString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    nocase = StartsWithI(Tcl_GetString(objv[1]));

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(nocase == 0
                                                   ? Ns_SetDeleteKey(set, keyString)
                                                   : Ns_SetIDeleteKey(set, keyString)));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetFormatObjCmd --
 *
 *      This command implements "ns_set format". It generates a formatted
 *      string representation of the given ns_set, using optional lead and
 *      separator strings to format each key-value pair.
 *
 * Results:
 *      Returns the formatted string as the Tcl command result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetFormatObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, noname = 0;
    char       *leadString = (char*)"  ", *separatorString = (char*)": ";
    Ns_Set     *set;
    Ns_ObjvSpec opts[] = {
        {"-noname",    Ns_ObjvBool,   &noname,          INT2PTR(NS_TRUE)},
        {"-lead",      Ns_ObjvString, &leadString,      NULL},
        {"-separator", Ns_ObjvString, &separatorString, NULL},
        {"--",         Ns_ObjvBreak,  NULL,             NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        Ns_SetFormat(&ds, set, noname == 0, leadString, separatorString);
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetFreeObjCmd --
 *
 *      This command implements "ns_set free". It frees the ns_set identified
 *      by the provided set handle. If the ns_set is dynamic, it is deleted
 *      and its resources are reclaimed.
 *
 * Results:
 *      Returns a standard Tcl result indicating success (TCL_OK) or failure
 *      (TCL_ERROR).
 *
 * Side effects:
 *      The ns_set and all its associated data are freed if they are dynamic.
 *
 *----------------------------------------------------------------------
 */
static int SetFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        (void) Ns_TclFreeSet(interp, Tcl_GetString(objv[2]));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetFindObjCmd --
 *
 *      This command implements "ns_set find". It searches for a key within
 *      the specified ns_set and returns the index of the key if found.
 *
 * Results:
 *      Returns the index of the found key, or TCL_ERROR if the key is not
 *      present.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetFindObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *keyString = NULL;
    int         nocase = 0, result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec opts[] = {
        {"-nocase", Ns_ObjvBool,  &nocase, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"setId",       Ns_ObjvSet,    &set, NULL},
        {"key",         Ns_ObjvString, &keyString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    nocase = StartsWithI(Tcl_GetString(objv[1]));

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp,
                         Tcl_NewIntObj(Ns_SetFindCmp(set, keyString, nocase == 0 ? strcmp : strcasecmp)));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetIsnullObjCmd --
 *
 *      This command implements "ns_set isnull". It checks whether the value
 *      at a given index in the specified ns_set is NULL.
 *
 * Results:
 *      Returns a boolean value (1 for true, 0 for false) as the Tcl command
 *      result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetIsnullObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       Ns_ObjvSet,  &set, NULL},
        {"fieldNumber", Ns_ObjvLong, &idx, &maxIdxRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (idx > (long)Ns_SetSize(set)) {
        Ns_TclPrintfResult(interp, "ns_set %s: index must be maximal the number of elements",
                           Tcl_GetString(objv[1]));
        result = TCL_ERROR;

    } else {
        const char *val = Ns_SetValue(set, idx);
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj((val != NULL) ? 0 : 1));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * SetGetObjCmd --
 *
 *      This command implements "ns_set get". It retrieves the value associated
 *      with a given key index from the specified ns_set. If the key is not found,
 *      an optional default value may be returned.
 *
 * Results:
 *      Returns the value of the key or the default value as the Tcl command result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetGetObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *keyString = NULL, *defaultString = NULL;
    int         all = 0, nocase = 0, result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec opts[] = {
        {"-all",    Ns_ObjvBool,  &all,    INT2PTR(NS_TRUE)},
        {"-nocase", Ns_ObjvBool,  &nocase, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"setId",       Ns_ObjvSet,    &set, NULL},
        {"key",         Ns_ObjvString, &keyString, NULL},
        {"?default",    Ns_ObjvString, &defaultString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    nocase = StartsWithI(Tcl_GetString(objv[1]));

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_DList    dl, *dlPtr = &dl;
        size_t      count;

        Ns_DListInit(dlPtr);
        count = NsSetGetCmpDListAppend(set, keyString, all, nocase == 0 ? strcmp : strcasecmp, dlPtr, NS_FALSE);
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
    return result;
}



/*
 *----------------------------------------------------------------------
 * SetKeyObjCmd --
 *
 *      This command implements "ns_set key". It returns the key (field name)
 *      at the specified index in the given ns_set.
 *
 * Results:
 *      Returns the key as a string in the Tcl command result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetKeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       Ns_ObjvSet,  &set, NULL},
        {"fieldNumber", Ns_ObjvLong, &idx, &maxIdxRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (idx > (long)Ns_SetSize(set)) {
        Ns_TclPrintfResult(interp, "ns_set %s: index must be maximal the number of elements",
                           Tcl_GetString(objv[1]));
        result = TCL_ERROR;

    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj( Ns_SetKey(set, idx), TCL_INDEX_NONE));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetKeysObjCmd --
 *
 *      This command implements "ns_set keys". It returns a Tcl list of all
 *      keys (field names) present in the specified ns_set. An optional pattern
 *      can be provided to filter the keys.
 *
 * Results:
 *      Returns a Tcl list of matching keys.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetKeysObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *patternString = NULL;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",    Ns_ObjvSet, &set, NULL},
        {"?pattern", Ns_ObjvString, &patternString, NULL},

        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_DString ds;
        size_t      i;

        Tcl_DStringInit(&ds);
        for (i = 0u; i < set->size; ++i) {
            const char *value = (set->fields[i].name != NULL ? set->fields[i].name : "");
            if (patternString == NULL || (Tcl_StringMatch(value, patternString) != 0)) {
                Tcl_DStringAppendElement(&ds, value);
            }
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetListObjCmd --
 *
 *      This command implements "ns_set list". It returns a Tcl list
 *      of all keys (i.e. field names) present in the specified ns_set.
 *
 * Results:
 *      Returns TCL_OK on success with the list of keys set in the
 *      interpreter's result, or TCL_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetListObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int       result = TCL_OK;
    NsInterp *itPtr = clientData;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_Obj             *listObj = Tcl_NewListObj(0, NULL);
        Tcl_HashTable       *tablePtr = &itPtr->sets;
        const Tcl_HashEntry *hPtr;
        Tcl_HashSearch       search;

        for (hPtr = Tcl_FirstHashEntry(tablePtr, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search)
             ) {
            const char *listKey = Tcl_GetHashKey(tablePtr, hPtr);
            Tcl_ListObjAppendElement(interp, listObj, Tcl_NewStringObj(listKey, TCL_INDEX_NONE));
        }
        Tcl_SetObjResult(interp, listObj);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetMergeObjCmd --
 *
 *      This command implements "ns_set merge". It merges two ns_set objects.
 *      The function takes two ns_set handles and combines the key-value pairs
 *      from the second set into the first.
 *
 * Results:
 *      Returns TCL_OK on success (with the merged set handle as result) or
 *      TCL_ERROR on failure.
 *
 * Side effects:
 *      The first ns_set is modified to include entries from the second ns_set.
 *
 *----------------------------------------------------------------------
 */

static int SetMergeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, nocase = 0;
    Ns_Set     *set1, *set2;
    Ns_ObjvSpec opts[] = {
        {"-nocase", Ns_ObjvBool,  &nocase, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"setId1",  Ns_ObjvSet, &set1, NULL},
        {"setId2",  Ns_ObjvSet, &set2, NULL},
        {NULL, NULL, NULL, NULL}
    };

    nocase = StartsWithI(Tcl_GetString(objv[1]));
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (nocase == 1) {
        Ns_SetIMerge(set1, set2);
    } else {
        Ns_SetMerge(set1, set2);
        Tcl_SetObjResult(interp, objv[2]);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * SetMoveObjCmd --
 *
 *      This command implements "ns_set move". It moves all key-value pairs
 *      from the first ns_set to the second ns_set.
 *
 * Results:
 *      Returns TCL_OK on success (with the destination set handle as result)
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      The source ns_set is emptied and its data transferred to the destination ns_set.
 *
 *----------------------------------------------------------------------
 */

static int SetMoveObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set1, *set2;
    Ns_ObjvSpec args[] = {
        {"setId1",  Ns_ObjvSet, &set1, NULL},
        {"setId2",  Ns_ObjvSet, &set2, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_SetMove(set1, set2);
        Tcl_SetObjResult(interp, objv[2]);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetNameObjCmd --
 *
 *      This command implements "ns_set name". It retrieves and returns the name
 *      of the specified ns_set.
 *
 * Results:
 *      Returns TCL_OK on success with the ns_set's name as the interpreter result,
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetNameObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(set->name, TCL_INDEX_NONE));
    }
    return result;
}

#ifdef NS_WITH_DEPRECATED

/*
 *----------------------------------------------------------------------
 * SetPrintObjCmd --
 *
 *      This command implements "ns_set print" (deprecated). It prints the
 *      content of the specified ns_set to the log for debugging purposes.
 *
 * Results:
 *      Returns TCL_OK on success or TCL_ERROR on failure.
 *
 * Side effects:
 *      Outputs information to the system log.
 *
 *----------------------------------------------------------------------
 */
static int SetPrintObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_LogDeprecated(objv, 2, "ns_set format ...", NULL);
        Ns_SetPrint(NULL, set);
    }
    return result;
}
#endif

/*
 *----------------------------------------------------------------------
 * SetPutObjCmd --
 *
 *      This command implements "ns_set put". It inserts or updates a
 *      key-value pair in the specified ns_set. If the key already exists, its
 *      value is updated; otherwise, a new key-value pair is added.
 *
 * Results:
 *      Returns TCL_OK on success with the index of the inserted/updated element,
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      The ns_set is modified with the new key-value pair.
 *
 *----------------------------------------------------------------------
 */
static int SetPutObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Tcl_Obj    *keyObj, *valueObj;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {"key",    Ns_ObjvObj, &keyObj,   NULL},
        {"value",  Ns_ObjvObj, &valueObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const char *keyString, *valueString;
        TCL_SIZE_T  keyLength, valueLength;
        ssize_t     idx;

        keyString = Tcl_GetStringFromObj(keyObj, &keyLength);
        valueString = Tcl_GetStringFromObj(valueObj, &valueLength);

        idx = (int)Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);

        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)idx));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetSizeObjCmd --
 *
 *      This command implements "ns_set size". It returns the number of
 *      elements in the specified ns_set. Optionally, it can also resize the
 *      set based on provided parameters.
 *
 * Results:
 *      Returns TCL_OK on success with the size as the interpreter's result,
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      May modify the ns_set's allocation if resizing is requested.
 *
 *----------------------------------------------------------------------
 */
static int SetSizeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        bufferSize = -1, nrElements = -1;
    Ns_Set     *set;
    Ns_ObjvValueRange posintRange = {0, LLONG_MAX};
    Ns_ObjvValueRange posintRange200 = {TCL_DSTRING_STATIC_SIZE, LLONG_MAX};
    Ns_ObjvSpec args[] = {
        {"setId",       Ns_ObjvSet,  &set, NULL},
        {"?nrElements", Ns_ObjvLong, &nrElements, &posintRange},
        {"?bufferSize", Ns_ObjvLong, &bufferSize, &posintRange200},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        if (bufferSize == -1 && nrElements == -1) {
            Tcl_SetObjResult(interp, Tcl_NewLongObj((long)Ns_SetSize(set)));

        } else {
            NsSetResize(set, (size_t)nrElements, (int)bufferSize);
            Tcl_SetObjResult(interp, Tcl_NewLongObj((long)Ns_SetSize(set)));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetSplitObjCmd --
 *
 *      This command implements "ns_set split". It splits the specified ns_set
 *      into multiple ns_set objects based on a provided split character.
 *
 * Results:
 *      Returns TCL_OK on success with a Tcl list of new ns_set handles,
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      The original ns_set is split into several new ns_set objects.
 *
 *----------------------------------------------------------------------
 */
static int SetSplitObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    NsInterp   *itPtr = clientData;
    char       *splitString = (char *)".";
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",      Ns_ObjvSet,    &set,         NULL},
        {"?splitChar", Ns_ObjvString, &splitString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (strlen(splitString) != 1) {
        Ns_TclPrintfResult(interp, "ns_set split: the split character must be one"
                           " character long, provided '%s'", splitString);
        result = TCL_ERROR;

    } else {
        Tcl_Obj     *listObj = Tcl_NewListObj(0, NULL);
        Ns_Set     **sets;
        TCL_SIZE_T   i;

        sets = Ns_SetSplit(set, *splitString);
        for (i = 0; sets[i] != NULL; i++) {
            Tcl_ListObjAppendElement(interp, listObj,
                                     EnterSet(itPtr, sets[i], NS_TCL_SET_DYNAMIC));
        }
        Tcl_SetObjResult(interp, listObj);
        ns_free(sets);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * SetStatsObjCmd --
 *
 *      This command implements "ns_set stats". It returns statistics about
 *      the specified ns_set, such as the number and size of dynamic and
 *      static entries.
 *
 * Results:
 *      Returns TCL_OK on success with a Tcl dictionary of statistics,
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetStatsObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int       result = TCL_OK;
    NsInterp *itPtr = clientData;

    if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const Tcl_HashEntry *hPtr;
        Tcl_HashTable       *tablePtr = &itPtr->sets;
        Tcl_HashSearch       search;
        Tcl_Obj *resultObj = Tcl_NewListObj(0, NULL);
        size_t nr_dynamic = 0, size_dynamic = 0, allocated_dynamic = 0;
        size_t nr_static = 0,  size_static = 0,  allocated_static = 0;

        tablePtr = &itPtr->sets;
        for (hPtr = Tcl_FirstHashEntry(tablePtr, &search);
             hPtr != NULL;
             hPtr = Tcl_NextHashEntry(&search)
             ) {
            const char *key = Tcl_GetHashKey(tablePtr, hPtr);
            Ns_Set *set     = (Ns_Set *) Tcl_GetHashValue(hPtr);

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
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetTruncateObjCmd --
 *
 *      This command implements "ns_set truncate". It truncates the specified
 *      ns_set to a given number of elements.
 *
 * Results:
 *      Returns TCL_OK on success or TCL_ERROR on failure.
 *
 * Side effects:
 *      The ns_set is modified (reduced in size).
 *
 *----------------------------------------------------------------------
 */
static int SetTruncateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       Ns_ObjvSet,  &set, NULL},
        {"fieldNumber", Ns_ObjvLong, &idx, &maxIdxRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (idx > (long)Ns_SetSize(set)) {
        Ns_TclPrintfResult(interp, "ns_set %s: index must be maximal the number of elements",
                           Tcl_GetString(objv[1]));
        result = TCL_ERROR;

    } else {
         Ns_SetTrunc(set, (size_t)idx);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetUniqueObjCmd --
 *
 *      This command implements "ns_set unique". It ensures that a given key
 *      appears uniquely within the specified ns_set. Depending on the
 *      case-sensitivity option, it may use case-insensitive matching.
 *
 * Results:
 *      Returns TCL_OK on success with a boolean result indicating whether the
 *      key was unique, or TCL_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetUniqueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *keyString = NULL;
    int         nocase = 0, result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec opts[] = {
        {"-nocase", Ns_ObjvBool,  &nocase, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"setId",       Ns_ObjvSet,    &set, NULL},
        {"key",         Ns_ObjvString, &keyString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    nocase = StartsWithI(Tcl_GetString(objv[1]));

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(nocase == 0
                                               ? Ns_SetUnique(set, keyString)
                                               : Ns_SetIUnique(set, keyString)));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * SetValueObjCmd --
 *
 *      This command implements "ns_set value". It retrieves the value associated
 *      with a specified key index from the ns_set.
 *
 * Results:
 *      Returns TCL_OK on success with the value as a string result, or
 *      TCL_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int SetValueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       Ns_ObjvSet,  &set, NULL},
        {"fieldNumber", Ns_ObjvLong, &idx, &maxIdxRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else if (idx > (long)Ns_SetSize(set)) {
        Ns_TclPrintfResult(interp, "ns_set %s: index must be maximal the number of elements",
                           Tcl_GetString(objv[1]));
        result = TCL_ERROR;

    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_SetValue(set, idx), TCL_INDEX_NONE));
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * SetValuesObjCmd --
 *
 *      This command implements "ns_set values". It returns a Tcl list of all
 *      values in the specified ns_set that match an optional pattern.
 *
 * Results:
 *      Returns TCL_OK on success with the list of values as the interpreter's result,
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int SetValuesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *patternString = NULL;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",    Ns_ObjvSet,    &set, NULL},
        {"?pattern", Ns_ObjvString, &patternString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_DString ds;
        size_t      i;

        Tcl_DStringInit(&ds);
        for (i = 0u; i < set->size; ++i) {
            const char *value = set->fields[i].value;
            if (value == NULL) {
                value = "";
            }
            if (patternString == NULL || (Tcl_StringMatch(value, patternString) != 0)) {
                Tcl_DStringAppendElement(&ds, value);
            }
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 * Set_TYPE_SetidKeyValueObjCmd --
 *
 *      This command implements "ns_set update" and "ns_set cput". It updates
 *      an existing key-value pair in the specified ns_set, or inserts it if
 *      the key does not exist.  The behavior is determined by the command
 *      prefix (e.g., "update" vs. "cput") and the case-sensitivity inferred
 *      from the key.
 *
 * Results:
 *      Returns TCL_OK on success with the index of the updated/inserted element,
 *      or TCL_ERROR on failure.
 *
 * Side effects:
 *      The ns_set is modified with the new or updated key-value pair.
 *
 *----------------------------------------------------------------------
 */
static int Set_TYPE_SetidKeyValueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, nocase = 0;
    const char *subcmdString;
    Tcl_Obj    *keyObj, *valueObj;
    Ns_Set     *set;
    Ns_ObjvSpec opts[] = {
        {"-nocase", Ns_ObjvBool,  &nocase, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"setId",  Ns_ObjvSet, &set, NULL},
        {"key",    Ns_ObjvObj, &keyObj,   NULL},
        {"value",  Ns_ObjvObj, &valueObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    subcmdString = Tcl_GetString(objv[1]);
    nocase = StartsWithI(subcmdString);
    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const char *keyString, *valueString;
        char        firstChar;
        TCL_SIZE_T  keyLength, valueLength;
        ssize_t     idx = -1;

        keyString = Tcl_GetStringFromObj(keyObj, &keyLength);
        valueString = Tcl_GetStringFromObj(valueObj, &valueLength);

        firstChar = nocase ? subcmdString[1] : subcmdString[0];
        /*
         * Possible methods:
         *    (i)update
         *    (i)cput
         */
        if (firstChar == 'u') {
            idx = nocase == 0
                ? (ssize_t)Ns_SetUpdateSz(set, keyString, keyLength, valueString, valueLength)
                : (ssize_t)Ns_SetIUpdateSz(set, keyString, keyLength, valueString, valueLength);

        } else if (firstChar == 'c') {
            idx = Ns_SetFindCmp(set, keyString, nocase == 0 ? strcmp : strcasecmp);
            if (idx < 0) {
                idx = (int)Ns_SetPutSz(set, keyString, keyLength, valueString, valueLength);
            }

        }
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)idx));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclSetObjCmd --
 *
 *      This command implements "ns_set". It provides a Tcl interface for
 *      performing various operations on ns_set objects, such as creation,
 *      deletion, update, merge, and lookup of key/value pairs.
 *
 * Results:
 *      Returns a standard Tcl result (TCL_OK on success, TCL_ERROR on failure).
 *
 * Side effects:
 *      Depends on the subcommand invoked; may modify ns_set objects, allocate
 *      memory, and update the interpreter's result.
 *
 *----------------------------------------------------------------------
 */
int
NsTclSetObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_SubCmdSpec subcmds[] = {
        {"array",    SetArrayObjCmd},
        {"cleanup",  SetCleanupObjCmd},
        {"copy",     SetCopyObjCmd},
        {"cput",     Set_TYPE_SetidKeyValueObjCmd},
        {"create",   SetCreateObjCmd},
        {"delete",   SetDeleteObjCmd},
        {"delkey",   SetDelkeyObjCmd},
        {"find",     SetFindObjCmd},
        {"format",   SetFormatObjCmd},
        {"free",     SetFreeObjCmd},
        {"get",      SetGetObjCmd},
        {"icput",    Set_TYPE_SetidKeyValueObjCmd},
        {"idelkey",  SetDelkeyObjCmd},
        {"ifind",    SetFindObjCmd},
        {"iget",     SetGetObjCmd},
        {"imerge",   SetMergeObjCmd},
        {"isnull",   SetIsnullObjCmd},
        {"iunique",  SetUniqueObjCmd},
        {"iupdate",  Set_TYPE_SetidKeyValueObjCmd},
        {"key",      SetKeyObjCmd},
        {"keys",     SetKeysObjCmd},
        {"list",     SetListObjCmd},
        {"merge",    SetMergeObjCmd},
        {"move",     SetMoveObjCmd},
        {"name",     SetNameObjCmd},
#ifdef NS_WITH_DEPRECATED
        {"new",      SetCreateObjCmd},
        {"print",    SetPrintObjCmd},
#endif
        {"put",      SetPutObjCmd},
        {"size",     SetSizeObjCmd},
        {"split",    SetSplitObjCmd},
        {"stats",    SetStatsObjCmd},
        {"truncate", SetTruncateObjCmd},
        {"unique",   SetUniqueObjCmd},
        {"update",   Set_TYPE_SetidKeyValueObjCmd},
        {"value",    SetValueObjCmd},
        {"values",   SetValuesObjCmd},
        {NULL, NULL}
    };

    return Ns_SubcmdObjv(subcmds, clientData, interp, objc, objv);
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
 * LookupInterpSet --
 *
 *      Take an ns_set name, look it up in the interpreter, and return it in
 *      the last argument. The function is similar to LookupSet(), but handles
 *      the case, where the Tcl Interp is not a NaviServer interpreter
 *      (NsInterp).
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      If deleteEntry is set, then the hash entry will be removed.  Set will
 *      be returned in given setPtr. In case of error, the error message is
 *      left in the interpreter result.
 *
 *----------------------------------------------------------------------
 */

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

/*
 *----------------------------------------------------------------------
 *
 * LookupSet --
 *
 *      Take an ns_set name, look it up in the interpreter, and return it in
 *      the last argument.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      If deleteEntry is set, then the hash entry will be removed.  Set will
 *      be returned in given setPtr. In case of error, the error message is
 *      left in the interpreter result.
 *
 *----------------------------------------------------------------------
 */

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
        Ns_TclPrintfResult(itPtr->interp, "no such set: '%s'", id);
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
