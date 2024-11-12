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
static TCL_OBJCMDPROC_T SetPrintObjCmd;
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
static int LookupObjSet(NsInterp *itPtr, Tcl_Obj *idPtr, bool deleteEntry, Ns_Set **setPtr)
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

/*
 *----------------------------------------------------------------------
 *
 * ObjvSetObj --
 *
 *      objv converter for Ns_Set *.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Ns_ObjvProc ObjvSetObj;

static int
ObjvSetObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    int result = TCL_ERROR;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        NsInterp *itPtr = NsGetInterpData(interp);
        Ns_Set  **dest = spec->dest;

        //int i; for (i=0; i < *objcPtr; i++) {
        //    fprintf(stderr, "... [%d] '%s'\n", i, Tcl_GetString(objv[i]));
        //}
        if (likely(LookupObjSet(itPtr, objv[0], NS_FALSE, dest) == TCL_OK)) {
            if (*dest == NULL) {
                Ns_TclPrintfResult(interp, "ns_set: could not convert '%s' to Ns_Set object",
                                   Tcl_GetString(objv[0]));
            } else {
                *objcPtr -= 1;
                result = TCL_OK;
            }
        }
        /*fprintf(stderr, "... ObjvSetObj lookup of set <%s> -> %p ok %d\n",
          Tcl_GetString(objv[0]), (void*)*dest, result == TCL_OK);*/
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ThreadCreateObjCmd, ThreadHandlObjCmd, ThreadIdObjCmd, ThreadNameObjCmd,
 * ThreadStackinfoObjCmd, ThreadWaitObjCmd, ThreadYieldObjCmd --
 *
 *      Implements subcommands of "ns_set", i.e.,
 *         "ns_thread create"
 *         "ns_thread handle"
 *         "ns_thread id"
 *         "ns_thread name"
 *         "ns_thread stackinfo"
 *         "ns_muthreadtex wait"
 *         "ns_muthreadtex yield"
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      Depends on subcommand.
 *
 *----------------------------------------------------------------------
 */

/*
 *----------------------------------------------------------------------
 * SetArrayObjCmd --
 *
 *      Implements subcommands of "ns_set array"
 *
 *----------------------------------------------------------------------
 */
static int SetArrayObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  ObjvSetObj, &set, NULL},
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
 *      Implements subcommands of "ns_set cleanup"
 *
 *----------------------------------------------------------------------
 */
static int SetCleanupObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
 *      Implements subcommands of "ns_set copy"
 *
 *----------------------------------------------------------------------
 */
static int SetCopyObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    NsInterp   *itPtr = clientData;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  ObjvSetObj, &set, NULL},
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
 *      Implements subcommands of "ns_set create"
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
        {"?args",    Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    const char  *subcmdName = Tcl_GetString(objv[1]);

    if (*subcmdName == 'n' && strcmp(subcmdName, "new") == 0) {
        Ns_LogDeprecated(objv, 2, "ns_set create ...", NULL);
    }

    if (*subcmdName == 'n') {
        nocase = 1;
    }

    if (Ns_ParseObjv(opts, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        const char *name;
        TCL_SIZE_T offset = objc-nargs;
        Ns_Set    *set;

        //fprintf(stderr, "SetCreateObjCmd subcmd '%s' nargs %d offset %ld\n", subcmdName, nargs, offset);

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
 *      Implements subcommands of "ns_set delete"
 *
 *----------------------------------------------------------------------
 */
static int SetDeleteObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       ObjvSetObj,  &set, NULL},
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
 *      Implements subcommands of "ns_set delkey"
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
        {"setId",       ObjvSetObj,    &set, NULL},
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
 *      Implements subcommands of "ns_set format"
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
        {"setId",  ObjvSetObj, &set, NULL},
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
 *      Implements subcommands of "ns_set free"
 *
 *----------------------------------------------------------------------
 */
static int SetFreeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  ObjvSetObj, &set, NULL},
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
 *      Implements subcommands of "ns_set find"
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
        {"setId",       ObjvSetObj,    &set, NULL},
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
 *      Implements subcommands of "ns_set isnull"
 *
 *----------------------------------------------------------------------
 */
static int SetIsnullObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       ObjvSetObj,  &set, NULL},
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
 *      Implements subcommands of "ns_set get"
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
        {"setId",       ObjvSetObj,    &set, NULL},
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
        count = NsSetGetCmpDListAppend(set, keyString, all, nocase == 0 ? strcmp : strcasecmp, dlPtr);
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
 *      Implements subcommands of "ns_set key"
 *
 *----------------------------------------------------------------------
 */
static int SetKeyObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       ObjvSetObj,  &set, NULL},
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
 *      Implements subcommands of "ns_set keys"
 *
 *----------------------------------------------------------------------
 */
static int SetKeysObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *patternString = NULL;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",    ObjvSetObj, &set, NULL},
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
 *      Implements subcommands of "ns_set list"
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
 *      Implements subcommands "ns_set merge".
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
        {"setId1",  ObjvSetObj, &set1, NULL},
        {"setId2",  ObjvSetObj, &set2, NULL},
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
 *      Implements subcommands "ns_set move".
 *
 *----------------------------------------------------------------------
 */
static int SetMoveObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set1, *set2;
    Ns_ObjvSpec args[] = {
        {"setId1",  ObjvSetObj, &set1, NULL},
        {"setId2",  ObjvSetObj, &set2, NULL},
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
 *      Implements subcommands of "ns_set name"
 *
 *----------------------------------------------------------------------
 */
static int SetNameObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  ObjvSetObj, &set, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 2, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(set->name, TCL_INDEX_NONE));
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 * SetPrintObjCmd --
 *
 *      Implements subcommands of "ns_set print"
 *
 *----------------------------------------------------------------------
 */
static int SetPrintObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  ObjvSetObj, &set, NULL},
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

/*
 *----------------------------------------------------------------------
 * SetPutObjCmd --
 *
 *      Implements subcommands "ns_set put".
 *
 *----------------------------------------------------------------------
 */
static int SetPutObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    Tcl_Obj    *keyObj, *valueObj;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",  ObjvSetObj, &set, NULL},
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
 *      Implements subcommands of "ns_set size"
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
        {"setId",       ObjvSetObj,  &set, NULL},
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
 *      Implements subcommands of "ns_set split"
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
        {"setId",      ObjvSetObj,    &set,         NULL},
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
 *      Implements subcommands of "ns_set stats"
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
 *      Implements subcommands of "ns_set truncate"
 *
 *----------------------------------------------------------------------
 */
static int SetTruncateObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       ObjvSetObj,  &set, NULL},
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
 *      Implements subcommands of "ns_set unique"
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
        {"setId",       ObjvSetObj,    &set, NULL},
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
 *      Implements subcommands of "ns_set value"
 *
 *----------------------------------------------------------------------
 */
static int SetValueObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    long        idx;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",       ObjvSetObj,  &set, NULL},
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
 *      Implements subcommands of "ns_set values"
 *
 *----------------------------------------------------------------------
 */
static int SetValuesObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *patternString = NULL;
    Ns_Set     *set;
    Ns_ObjvSpec args[] = {
        {"setId",    ObjvSetObj,    &set, NULL},
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
 *      Implements subcommands "ns_set update" and "ns_set cput".
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
        {"setId",  ObjvSetObj, &set, NULL},
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
        {"new",      SetCreateObjCmd},
        {"print",    SetPrintObjCmd},
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
NsTclParseHeaderObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
