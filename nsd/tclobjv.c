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
 * tclobjv.c --
 *
 *      Routines for parsing the options and arguments passed to Tcl commands.
 *
 */

static const char *RCSID =
    "@(#) $Header$, compiled: " __DATE__ " " __TIME__;

#include "ns.h"


/*
 * Static functions defined in this file.
 */

static void 
WrongNumArgs(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp, 
             int objc, Tcl_Obj *CONST objv[]);


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseObjv --
 *
 *      Process objv acording to given option and arg specs.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      Depends on the Ns_ObjvTypeProcs which run.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ParseObjv(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp,
             int offset, int objc, Tcl_Obj *CONST objv[])
{
    Ns_ObjvSpec *specPtr;
    int          optIndex, objvIndex, remain;

    objvIndex = offset;
    objc -= offset;


    if (optSpec != NULL) {
        while (objc > 0) {
            if (Tcl_GetIndexFromObjStruct(NULL, objv[objvIndex], optSpec,
                                          sizeof(Ns_ObjvSpec), "option", 
                                          TCL_EXACT, &optIndex) != TCL_OK) {
                break;
            }
            specPtr = &optSpec[optIndex];
            ++objvIndex, --objc;
            
            remain = specPtr->proc(specPtr->dest, interp, objc, objv + objvIndex,
                                   specPtr->arg);
            if (remain == objc) {
                break;
            }
            if (remain < 0 || remain > objc) {
                return NS_ERROR;
            }
            objvIndex += (objc - remain);
            objc = remain;
        }
    }

    for (specPtr = argSpec; specPtr->key != NULL; ++specPtr) {

        /* are there more required arguments than were supplied? */
        if (objc == 0) {
            if (*specPtr->key != '?') {
                WrongNumArgs(optSpec, argSpec, interp, offset, objv);
                return NS_ERROR;
            }
            return NS_OK;
        }

        remain = specPtr->proc(specPtr->dest, interp, objc, objv + objvIndex, 
                               specPtr->arg);
        if (remain < 0 || remain > objc) {
            return NS_ERROR;
        }
        objvIndex += (objc - remain);
        objc = remain;
    }

    /* are there more supplied args than formal arguments? */
    if (specPtr->key == NULL) {
        if (objc > 0) {
            WrongNumArgs(optSpec, argSpec, interp, offset, objv);
            return NS_ERROR;
        }
    } else if (*specPtr->key != '?' && objc == 0) {
        WrongNumArgs(optSpec, argSpec, interp, offset, objv);
        return NS_ERROR;
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvBool, Ns_ObjvInt, Ns_ObjvLong, Ns_ObjvWideInt, 
 * Ns_ObjvDouble --
 *
 *      Consume exactly one argument, returning it's value into dest.
 *
 * Results:
 *      On success (objc - 1) and dest will contain the value matched
 *      On error (or no arguments supplied; objc = 0) -1 will be returned
 *
 * Side effects:
 *      Argument maybe converted to type specific internal rep.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvBool(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
            void *arg)
{
    if (objc > 0 
        && Tcl_GetBooleanFromObj(interp, objv[0], (int *) dest) == TCL_OK) {
        return --objc;
    }

    return -1;
}

int
Ns_ObjvInt(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
           void *arg)
{
    if (objc > 0 
        && Tcl_GetIntFromObj(interp, objv[0], (int *) dest) == TCL_OK) {
        return --objc;
    }

    return -1;
}

int
Ns_ObjvLong(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
            void *arg)
{
    if (objc > 0 
        && Tcl_GetLongFromObj(interp, objv[0], (long *) dest) == TCL_OK) {
        return --objc;
    }

    return -1;
}

int
Ns_ObjvWideInt(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
               void *arg)
{
    if (objc > 0 
        && Tcl_GetWideIntFromObj(interp, objv[0], 
                                 (Tcl_WideInt *) dest) == TCL_OK) {
        return --objc;
    }

    return -1;
}

int
Ns_ObjvDouble(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
              void *arg)
{
    if (objc > 0 
        && Tcl_GetDoubleFromObj(interp, objv[0], (double *) dest) == TCL_OK) {
        return --objc;
    }

    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvString --
 *
 *      Consume exactly one argument, returning a pointer to it's
 *      cstring into dest.
 *
 *      If the "arg" element is != NULL it is assumed to be a pointer 
 *      to an int and the returned string length will be left in it.
 *
 * Results:
 *      On success (objc - 1) and dest will contain the value matched
 *      On error (or no arguments supplied; objc = 0) -1 will be returned
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvString(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
              void *arg)
{
    if (objc > 0) {
        if (arg == NULL) {
            *((char **) dest) = Tcl_GetString(objv[0]);
        } else {
            *((char **) dest) = Tcl_GetStringFromObj(objv[0], (int*)arg);
        }
        return --objc;
    }

    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvObj --
 *
 *      Consume exactly one argument, returning it's pointer into dest
 *      with no conversion.
 *
 * Results:
 *      On success (objc - 1) and dest will contain the value matched
 *      On error (or no arguments supplied; objc = 0) -1 will be returned
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvObj(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
           void *arg)
{
    if (objc > 0) {
        *((Tcl_Obj **) dest) = objv[0];
        return --objc;
    }

    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvIndex --
 *
 *      Match the current argument against the keys in the specified
 *      table, returning the value at the index of the first match into
 *      dest. It is an error for the argument to contain anything but
 *      one of the table keys.
 *
 * Results:
 *      On success (objc - 1) and dest will contain the value matched
 *      On error (or no arguments supplied; objc = 0) -1 will be returned
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvIndex(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
             void *arg)
{
    Ns_ObjvTable *tablePtr = arg;
    int           tableIdx;

    if (objc > 0) {
        if (Tcl_GetIndexFromObjStruct(interp, objv[0], tablePtr, 
                                      sizeof(Ns_ObjvTable), "option", 
                                      TCL_EXACT, &tableIdx) != TCL_OK) {
            return -1;
        }
        *((int *) dest) = tablePtr[tableIdx].value;
        return --objc;
    }

    return -1;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvFlags --
 *
 *      Treat the current argument as a list of flags and compare them
 *      all with the keys in the specified table.  As flags are matched
 *      the values are ORed togethter and the result is returned in dest.
 *      An unknown flag causes an error.
 *
 * Results:
 *      On success (objc - 1) and dest will contain the value matched
 *      On error (or no arguments supplied; objc = 0) -1 will be returned
 *
 * Side effects:
 *      Arg may be converted to list representation.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvFlags(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
             void *arg)
{
    Ns_ObjvTable *tablePtr = arg;
    int           tableIdx, i, flagc;
    Tcl_Obj     **flagv;

    if (objc < 1) {
        return -1;
    }
    if (Tcl_ListObjGetElements(interp, objv[0], &flagc, &flagv) != TCL_OK) {
        return -1;
    }
    if (flagc == 0) {
        Tcl_SetStringObj(Tcl_GetObjResult(interp), 
                         "blank flag specification", -1);
        return -1;
    }
    for (i = 0; i < flagc; ++i) {
    	if (Tcl_GetIndexFromObjStruct(interp, flagv[i], tablePtr, 
                                      sizeof(Ns_ObjvTable), "flag", 
                                      TCL_EXACT, &tableIdx) != TCL_OK) {
            return -1;
    	}
        *((int *) dest) |= tablePtr[tableIdx].value;
    }

    return --objc;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvBreak --
 *
 *	    Handle '--' option/argument separator.
 *
 * Results:
 *      Exactly objc.
 *
 * Side effects:
 *      Option processing will end successfully, argument processing 
 *      will begin.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvBreak(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
             void *arg)
{
    return objc;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvArgs --
 *
 *	    Count all remaining arguments, returning zero left unprocessed.
 *
 * Results:
 *	    Exactly 0 (zero).
 *
 * Side effects:
 *	    Arguments processing will end sucessfully.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvArgs(void *dest, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[],
            void *arg)
{
    *((int *) dest) = objc;
    return 0;
}


/*
 *----------------------------------------------------------------------
 *
 * WrongNumArgs --
 *
 *	    Leave a usage message in the interpreters result.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

static void
WrongNumArgs(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp,
             int objc, Tcl_Obj *CONST objv[])
{
    Ns_ObjvSpec *specPtr;
    Ns_DString   ds;
    char        *p;

    Ns_DStringInit(&ds);
    for (specPtr = optSpec; specPtr->key != NULL; ++specPtr) {
        if (STREQ(specPtr->key, "--")) {
            Ns_DStringAppend(&ds, "?--? ");
        } else {
            p = specPtr->key;
            if (*specPtr->key == '-') {
                ++p;
            }
            Ns_DStringPrintf(&ds, "?%s %s? ", specPtr->key, p);
        }
    }
    for (specPtr = argSpec; specPtr->key != NULL; ++specPtr) {
        Ns_DStringPrintf(&ds, "%s%s ", specPtr->key,
                         (*specPtr->key == '?') ? "?" : "");
    }
    Tcl_WrongNumArgs(interp, objc, objv, ds.string);
    Ns_DStringFree(&ds);
}
