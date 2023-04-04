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
 * tclconf.c --
 *
 *      Tcl commands for reading and setting configuration values.
 */

#include "nsd.h"


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
NsTclConfigObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    char       *section;
    Tcl_Obj    *defObj = NULL, *keyObj;
    int         status, isBool = 0, isInt = 0, exact = 0, doSet = 0;
    Tcl_WideInt minValue = LLONG_MIN, maxValue = LLONG_MAX;

    Ns_ObjvSpec opts[] = {
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
        bool        done = NS_FALSE;
        char       *keyString;
        int         keyLength;

        if (minValue > LLONG_MIN || maxValue < LLONG_MAX) {
            isInt = 1;
        }

        keyString = Tcl_GetStringFromObj(keyObj, &keyLength);
        value = (exact != 0) ?
            Ns_ConfigGetValueExact(section, keyString) :
            Ns_ConfigGetValue(section, keyString);

        /*
         * Handle type checking of config value.
         */

        status = TCL_OK;

        if (isBool != 0) {
            if (value != NULL) {
                int i;
                status = Tcl_GetBoolean(interp, value, &i);
                if (status == TCL_OK) {
                    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(i));
                    done = NS_TRUE;
                }
            }
        } else if (isInt != 0) {
            if (value != NULL) {
                Tcl_WideInt v;

                /*
                 * There is no Tcl_GetWideInt so we put same error message as
                 * Tcl_GetInt.
                 */

                if (unlikely(Ns_StrToWideInt(value, &v) != NS_OK)) {
                    Ns_TclPrintfResult(interp, "expected integer but got \"%s\"", value);
                    done = NS_TRUE;
                    status = TCL_ERROR;
                }  else if (v >= minValue && v <= maxValue) {
                    Tcl_SetObjResult(interp, Tcl_NewWideIntObj(v));
                    done = NS_TRUE;
                } else {
                    Ns_TclPrintfResult(interp, "value '%s' out of range", value);
                    status = TCL_ERROR;
                }
            }
        } else if (value != NULL) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(value, -1));
            done = NS_TRUE;
        }

        /*
         * Handle default value.
         */

        if (!done && defObj != NULL) {
            status = TCL_OK;

            if (isBool != 0) {
                int i;
                if (unlikely(Tcl_GetBooleanFromObj(interp, defObj, &i) != TCL_OK)) {
                    status = TCL_ERROR;
                } else {
                    defObj = Tcl_NewIntObj(i);
                }
            } else if (isInt != 0) {
                Tcl_WideInt v;

                if (Tcl_GetWideIntFromObj(interp, defObj, &v) != TCL_OK) {
                    status = TCL_ERROR;
                } else if (v < minValue || v > maxValue) {
                    Ns_TclPrintfResult(interp, "value '%s' out of range", Tcl_GetString(defObj));
                    status = TCL_ERROR;
                }
            }

            if (status == TCL_OK && doSet != 0 && !nsconf.state.started) {
                Ns_Set *set = Ns_ConfigCreateSection(section);

                /* make setting queryable */
                if (set != NULL) {
                    int         defLength;
                    const char *defString = Tcl_GetStringFromObj(defObj, &defLength);

                    Ns_SetUpdateSz(set, keyString, keyLength, defString, defLength);
                }
            }
            if (status == TCL_OK) {
                Tcl_SetObjResult(interp, defObj);
            }
        }
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
NsTclConfigSectionObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
NsTclConfigSectionsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
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
