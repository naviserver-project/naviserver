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
 * tclconf.c --
 *
 *      Tcl commands for reading and setting configuration values.
 */

#include "nsd.h"

static int GetBoolFromStringOrDefault(Tcl_Interp *interp, const char *value, Tcl_Obj *defObj);
static int GetIntFromStringOrDefault(Tcl_Interp *interp, const char *value, Tcl_Obj *defObj, Tcl_WideInt minValue, Tcl_WideInt maxValue);
static void ReturnAllValues(Tcl_Interp *interp, int all, Ns_DList *dlPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3);

/*
 *----------------------------------------------------------------------
 *
 * GetBoolFromDefault, GetIntFromStringOrDefault --
 *
 *      Helper functions for NsTclConfigObjCmd() for getting booleans or value
 *      ranged integer values either from string values (from the
 *      configuration) or from defaults.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Setting interpreter result.
 *
 *----------------------------------------------------------------------
 */
static int
GetBoolFromStringOrDefault(Tcl_Interp *interp, const char *value, Tcl_Obj *defObj)
{
    int i, result = TCL_OK;

    if (value != NULL) {
        if (Tcl_GetBoolean(interp, value, &i) != TCL_OK) {
            if (defObj != NULL) {
                result = Tcl_GetBooleanFromObj(interp, defObj, &i);
            } else {
                result = TCL_ERROR;
            }
        }
    } else if (defObj != NULL) {
        result = Tcl_GetBooleanFromObj(interp, defObj, &i);
    }

    if (result == TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(i));
    }
    return result;
}

static int
GetIntFromStringOrDefault(Tcl_Interp *interp, const char *value, Tcl_Obj *defObj, Tcl_WideInt minValue, Tcl_WideInt maxValue)
{
    int result = TCL_OK;
    Tcl_WideInt v;

    if (value != NULL && (Ns_StrToWideInt(value, &v) != NS_OK)) {
        /*
         * There is no Tcl_GetWideInt so we put same error message as
         * Tcl_GetInt.
         */
        Ns_TclPrintfResult(interp, "expected integer but got \"%s\"", value);
        result = TCL_ERROR;
    } else if (defObj != NULL && Tcl_GetWideIntFromObj(interp, defObj, &v) != TCL_OK) {
        result = TCL_ERROR;
    } else if (v >= minValue && v <= maxValue) {
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(v));
    } else {
        Ns_TclPrintfResult(interp, "value '%s' out of range", value != NULL ? value : Tcl_GetString(defObj));
        result = TCL_ERROR;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ReturnAllValues --
 *
 *      Helper functions for NsTclConfigObjCmd() to set the interpreter result
 *      to the first all all values of a Ns_DList structure, depending on the
 *      "all" flag.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Setting interpreter result.
 *
 *----------------------------------------------------------------------
 */
static void
ReturnAllValues(Tcl_Interp *interp, int all, Ns_DList *dlPtr)
{
    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(dlPtr != NULL);

    if (all != 0) {
        Tcl_Obj *resultObj = Tcl_NewListObj((TCL_SIZE_T)dlPtr->size, NULL);
        size_t   i;

        for (i = 0u; i < dlPtr->size; i++) {
            Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(dlPtr->data[i], TCL_INDEX_NONE));
        }
        Tcl_SetObjResult(interp, resultObj);
    } else {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(dlPtr->data[0], TCL_INDEX_NONE));
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclConfigObjCmd --
 *
 *      Implements "ns_config".
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
NsTclConfigObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    char       *section;
    Tcl_Obj    *defObj = NULL, *keyObj;
    int         status = TCL_OK, isBool = 0, isInt = 0, exact = 0, doSet = 0, all = 0;
    Tcl_WideInt minValue = LLONG_MIN, maxValue = LLONG_MAX;

    Ns_ObjvSpec opts[] = {
        {"-all",   Ns_ObjvBool,      &all,      INT2PTR(NS_TRUE)},
        {"-bool",  Ns_ObjvBool,      &isBool,   INT2PTR(NS_TRUE)},
        {"-int",   Ns_ObjvBool,      &isInt,    INT2PTR(NS_TRUE)},
        {"-min",   Ns_ObjvWideInt,   &minValue, NULL},
        {"-max",   Ns_ObjvWideInt,   &maxValue, NULL},
        {"-exact", Ns_ObjvBool,      &exact,    INT2PTR(NS_TRUE)},
        {"-set",   Ns_ObjvBool,      &doSet,    INT2PTR(NS_TRUE)},
        {"--",     Ns_ObjvBreak,     NULL,      NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"section",  Ns_ObjvString, &section, NULL},
        {"key",      Ns_ObjvObj,    &keyObj,     NULL},
        {"?default", Ns_ObjvObj,    &defObj,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (unlikely(Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK)) {
        status = TCL_ERROR;

    } else {
        const char *value;
        char       *keyString;
        TCL_SIZE_T  keyLength;
        Ns_DList    dl, *dlPtr = &dl;
        size_t      count;
        Ns_Set     *set = Ns_ConfigGetSection(section);

        if (minValue > LLONG_MIN || maxValue < LLONG_MAX) {
            isInt = 1;
        }

        Ns_DListInit(dlPtr);
        keyString = Tcl_GetStringFromObj(keyObj, &keyLength);

        count = likely(set != NULL)
            ? NsSetGetCmpDListAppend(set, keyString, NS_TRUE, exact == 0 ? strcmp : strcasecmp, dlPtr)
            : 0u;

        if (count == 1) {
            /*
             * We got a single value
             */
            value = dlPtr->data[0];

            if (isBool != 0) {
                status = GetBoolFromStringOrDefault(interp, value, defObj);

            } else if (isInt != 0) {
                status = GetIntFromStringOrDefault(interp, value, defObj, minValue, maxValue);

            } else {
                ReturnAllValues(interp, all, dlPtr);
            }

        } else if (count > 1) {
            /*
             * We got multiple values
             */
            if (isBool != 0) {
                Ns_TclPrintfResult(interp, "ns_config: -bool flag implies a single value, but got %ld values", count);
                status = TCL_ERROR;

            } else if (isInt != 0) {
                Ns_TclPrintfResult(interp, "ns_config: -int flag implies a single value, but got %ld values", count);
                status = TCL_ERROR;

            } else if (all) {
                ReturnAllValues(interp, all, dlPtr);

            } else {
                Ns_Log(Warning, "ns_config: returns the first of %ld values (section '%s' key '%s')",
                       count, section, keyString);
                Tcl_SetObjResult(interp, Tcl_NewStringObj(dlPtr->data[0], TCL_INDEX_NONE));
            }

        } else if (defObj != NULL /* && count == 0 */) {
            /*
             * Found no values, use the default value.
             */

            if (isBool != 0) {
                status = GetBoolFromStringOrDefault(interp, NULL, defObj);

            } else if (isInt != 0) {
                status = GetIntFromStringOrDefault(interp, NULL, defObj, minValue, maxValue);
            }

            if (status == TCL_OK && doSet != 0 && !nsconf.state.started) {
                if (set == NULL) {
                    set = Ns_ConfigCreateSection(section);
                }

                /* make setting queryable */
                if (set != NULL) {
                    TCL_SIZE_T  defLength;
                    const char *defString = Tcl_GetStringFromObj(defObj, &defLength);

                    Ns_SetIUpdateSz(set, keyString, keyLength, defString, defLength);
                }
            }
            if (status == TCL_OK) {
                Tcl_SetObjResult(interp, defObj);
            }
        }
        Ns_DListFree(dlPtr);
    }
    /*
     * Either TCL_OK and "" result because matching config not found, or
     * TCL_ERROR from type conversion error above.
     */

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConfigSectionObjCmd --
 *
 *      Implements "ns_configsection".
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
NsTclConfigSectionObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, filter = 0;
    char       *section;
    static Ns_ObjvTable filterset[] = {
        {"unread",    UCHAR('u')},
        {"defaulted", UCHAR('d')},
        {"defaults",  UCHAR('s')},
        {NULL,        0u}
    };
    Ns_ObjvSpec opts[] = {
        {"-filter", Ns_ObjvIndex, &filter, filterset},
        {"--",      Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"section",  Ns_ObjvString, &section, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (unlikely(Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK)) {
        result = TCL_ERROR;
    } else {
        Ns_Set *set;

        if (filter != '\0') {
            set = NsConfigSectionGetFiltered(section, (char)filter);
        } else {
            set = Ns_ConfigGetSection(section);
        }
        if (set != NULL) {
            result = Ns_TclEnterSet(interp, set,
                                    filter == 'd' || filter == 'u' ?
                                    NS_TCL_SET_DYNAMIC : NS_TCL_SET_STATIC);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConfigSectionsObjCmd --
 *
 *      Implements "ns_configsections".
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
NsTclConfigSectionsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int result;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        result = TCL_ERROR;
    } else {
        Ns_Set **sets;
        int      i;
        Tcl_Obj *resultList = Tcl_NewListObj(0, NULL);

        result = TCL_OK;
        sets = Ns_ConfigGetSections();
        for (i = 0; sets[i] != NULL; i++) {
            result = Ns_TclEnterSet(interp, sets[i], NS_TCL_SET_STATIC);
            if (unlikely(result != TCL_OK)) {
                break;
            }
            Tcl_ListObjAppendElement(interp, resultList, Tcl_GetObjResult(interp));
        }

        Tcl_SetObjResult(interp, resultList);
        ns_free(sets);
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
