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
 *      Tcl commands for reading and setting config file info. 
 */

#include "nsd.h"


/*
 *----------------------------------------------------------------------
 *
 * NsTclConfigObjCmd --
 *
 *      Implements ns_config. 
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
NsTclConfigObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    char       *section, *key;
    CONST char *value;
    Tcl_Obj    *defObj = NULL;
    int         status, i, isbool = 0, isint = 0, exact = 0;
    Tcl_WideInt v, min = LLONG_MIN, max = LLONG_MAX;

    Ns_ObjvSpec opts[] = {
        {"-bool",  Ns_ObjvBool,      &isbool, (void *) NS_TRUE},
        {"-int",   Ns_ObjvBool,      &isint,  (void *) NS_TRUE},
        {"-min",   Ns_ObjvWideInt,   &min,    NULL},
        {"-max",   Ns_ObjvWideInt,   &max,    NULL},
        {"-exact", Ns_ObjvBool,      &exact,  (void *) NS_TRUE},
        {"--",     Ns_ObjvBreak,     NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"section",  Ns_ObjvString, &section, NULL},
        {"key",      Ns_ObjvString, &key,     NULL},
        {"?default", Ns_ObjvObj,    &defObj,  NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }
    if (min > LLONG_MIN || max < LLONG_MAX) {
        isint = 1;
    }

    value = exact ?
        Ns_ConfigGetValueExact(section, key) :
        Ns_ConfigGetValue(section, key);

    /*
     * Handle type checking of config value.
     */

    status = TCL_OK;

    if (isbool) {
        if (value && ((status = Tcl_GetBoolean(interp, value, &i)) == TCL_OK)) {
            Tcl_SetObjResult(interp, Tcl_NewBooleanObj(i));
            return TCL_OK;
        }
    } else if (isint) {
        if (value != NULL) { 
            /*
             * There is no Tcl_GetWideInt so we put same error message as Tcl_GetInt
             */

            if (Ns_StrToWideInt(value, &v) != NS_OK) {
                Tcl_AppendResult(interp, "expected integer but got \"", value, "\"", NULL);
                return TCL_ERROR;
            } 
            if (v >= min && v <= max) {
                Tcl_SetObjResult(interp, Tcl_NewWideIntObj(v));
                return TCL_OK;
            } else {
                Tcl_SetResult(interp, "value out of range", TCL_STATIC);
                status = TCL_ERROR;
            }
        }
    } else if (value != NULL) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj(value, -1));
        return TCL_OK;
    }

    /*
     * Handle default value.
     */

    if (defObj != NULL) {
        if (isbool) {
            if (Tcl_GetBooleanFromObj(interp, defObj, &i) != TCL_OK) {
                return TCL_ERROR;
            }
            defObj = Tcl_NewIntObj(i);
        } else if (isint) {
            if (Tcl_GetWideIntFromObj(interp, defObj, &v) != TCL_OK) {
                return TCL_ERROR;
            }
            if (v < min || v > max) {
                Tcl_SetResult(interp, "value out of range", TCL_STATIC);
                return TCL_ERROR;
            }
        }
        Tcl_SetObjResult(interp, defObj);
        return TCL_OK;
    }

    /*
     * Either TCL_OK and "" result because matching config not found,
     * or TCL_ERROR from type conversion error above.
     */

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConfigSectionObjCmd --
 *
 *      Implements ns_configsection.
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
NsTclConfigSectionObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Set *set;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "section");
        return TCL_ERROR;
    }
    set = Ns_ConfigGetSection(Tcl_GetString(objv[1]));
    if (set != NULL) {
        Ns_TclEnterSet(interp, set, NS_TCL_SET_STATIC);
    }
    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclConfigSectionsObjCmd --
 *
 *      Implements ns_configsections. 
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
NsTclConfigSectionsObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_Set **sets;
    int      i;

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    sets = Ns_ConfigGetSections();
    for (i = 0; sets[i] != NULL; i++) {
        Ns_TclEnterSet(interp, sets[i], NS_TCL_SET_STATIC);
    }
    ns_free(sets);

    return TCL_OK;
}
