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


#define VALUE_SUPPLIED ((void *) NS_TRUE)


/*
 * Static functions defined in this file.
 */

static Tcl_FreeInternalRepProc FreeSpecObj;
static Tcl_DupInternalRepProc  DupSpec;
static Tcl_UpdateStringProc    UpdateStringOfSpec;
static Tcl_SetFromAnyProc      SetSpecFromAny;

Ns_ObjvProc ObjvTcl;
Ns_ObjvProc ObjvTclArgs;

static void FreeSpecs(Ns_ObjvSpec *optSpec);
static int SetValue(Tcl_Interp *interp, char *key, Tcl_Obj *valObjPtr);
static void
WrongNumArgs(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp, 
             int objc, Tcl_Obj *CONST objv[]);


/*
 * Static variables defined in this file.
 */

static Tcl_ObjType specType = {
    "ns:spec",
    FreeSpecObj,
    DupSpec,
    UpdateStringOfSpec,
    SetSpecFromAny
};



/*
 *----------------------------------------------------------------------
 *
 * NsTclInitSpecType --
 *
 *      Initialize ns:spec Tcl_Obj type.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsTclInitSpecType()
{
    Tcl_RegisterObjType(&specType);
}


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
    Ns_ObjvSpec *specPtr = NULL;
    int          optIndex, status, remain = (objc - offset);

    if (optSpec && optSpec->key) {
        while (remain > 0) {
            if (Tcl_GetIndexFromObjStruct(NULL, objv[objc - remain], optSpec,
                                          sizeof(Ns_ObjvSpec), "option", 
                                          TCL_EXACT, &optIndex) != TCL_OK) {
                break;
            }
            --remain;
            specPtr = optSpec + optIndex;
            status = specPtr->proc(specPtr, interp, &remain,
                                   objv + (objc - remain));
            if (status == TCL_BREAK) {
                break;
            } else if (status != TCL_OK) {
                return NS_ERROR;
            }
        }
    }
    if (argSpec == NULL) {
        if (remain > 0) {
        badargs:
            WrongNumArgs(optSpec, argSpec, interp, offset, objv);
            return NS_ERROR;
        }
        return NS_OK;
    }
    for (specPtr = argSpec; specPtr->key != NULL; specPtr++) {
        if (remain == 0) {
            if (specPtr->key[0] != '?') {
                goto badargs; /* Too few args. */
            }
            return NS_OK;
        }
        if (specPtr->proc(specPtr, interp, &remain, objv + (objc - remain))
            != TCL_OK) {
            return NS_ERROR;
        }
    }
    if (remain > 0) {
        goto badargs; /* Too many args. */
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvInt, Ns_ObjvLong, Ns_ObjvWideInt, Ns_ObjvDouble --
 *
 *      Consume exactly one argument, returning it's value into dest.
 *
 * Results:
 *      TCL_OK or TCL_ERROR;
 *
 * Side effects:
 *      Argument maybe converted to type specific internal rep.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvInt(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
           Tcl_Obj *CONST objv[])
{
    int *dest = spec->dest;

    if (*objcPtr > 0 && Tcl_GetIntFromObj(interp, objv[0], dest) == TCL_OK) {
        *objcPtr -= 1;
        return TCL_OK;
    }
    return TCL_ERROR;
}

int
Ns_ObjvLong(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *CONST objv[])
{
    long *dest = spec->dest;

    if (*objcPtr > 0 && Tcl_GetLongFromObj(interp, objv[0], dest) == TCL_OK) {
        *objcPtr -= 1;
        return TCL_OK;
    }
    return TCL_ERROR;
}

int
Ns_ObjvWideInt(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
               Tcl_Obj *CONST objv[])
{
    Tcl_WideInt *dest = spec->dest;

    if (*objcPtr > 0 && Tcl_GetWideIntFromObj(interp, objv[0], dest) == TCL_OK) {
        *objcPtr -= 1;
        return TCL_OK;
    }
    return TCL_ERROR;
}

int
Ns_ObjvDouble(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *CONST objv[])
{
    double *dest = spec->dest;

    if (*objcPtr > 0 && Tcl_GetDoubleFromObj(interp, objv[0], dest) == TCL_OK) {
        *objcPtr -= 1;
        return TCL_OK;
    }
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvBool --
 *
 *      If spec->arg is 0 consume exactly one argument and attempt
 *      conversion to a boolean value.  Otherwise, spec->arg is treated
 *      as an int and placed into spec->dest with zero args consumed.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	    Next Tcl object maybe converted to boolean type.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvBool(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *CONST objv[])
{
    int *dest = spec->dest;

    if (spec->arg) {
        *dest = (int) spec->arg;
        return TCL_OK;
    }
    if (*objcPtr > 0 && Tcl_GetBooleanFromObj(interp, objv[0], dest) == TCL_OK) {
        *objcPtr -= 1;
        return TCL_OK;
    }
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvString --
 *
 *      Consume exactly one argument, returning a pointer to it's
 *      cstring into *spec->dest.
 *
 *      If spec->arg is != NULL it is assumed to be a pointer to an
 *      int and the returned string length will be left in it.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvString(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *CONST objv[])
{
    char **dest = spec->dest;

    if (*objcPtr > 0) {
        if (spec->arg == NULL) {
            *dest = Tcl_GetString(objv[0]);
        } else {
            *dest = Tcl_GetStringFromObj(objv[0], (int *) spec->arg);
        }
        *objcPtr -= 1;
        return TCL_OK;
    }
    return TCL_ERROR;
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
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
           Tcl_Obj *CONST objv[])
{
    Tcl_Obj **dest = spec->dest;

    if (*objcPtr > 0) {
        *dest = objv[0];
        *objcPtr -= 1;
        return TCL_OK;
    }
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvIndex --
 *
 *      Match the next argument against the keys in the specified
 *      table, returning the value at the index of the first match into
 *      dest. It is an error for the argument to contain anything but
 *      one of the table keys.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvIndex(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
             Tcl_Obj *CONST objv[])
{
    Ns_ObjvTable   *tablePtr = spec->arg;
    int            *dest     = spec->dest;
    int             tableIdx;

    if (*objcPtr > 0) {
        if (Tcl_GetIndexFromObjStruct(interp, objv[0], tablePtr, 
                                      sizeof(Ns_ObjvTable), "option", 
                                      TCL_EXACT, &tableIdx) != TCL_OK) {
            return TCL_ERROR;
        }
        *dest = tablePtr[tableIdx].value;
        *objcPtr -= 1;
        return TCL_OK;
    }

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvFlags --
 *
 *      Treat the next argument as a list of flags and compare them
 *      all with the keys in the specified table.  As flags are matched
 *      the values are ORed togethter and the result is returned in dest.
 *      An unknown flag causes an error.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Arg may be converted to list representation.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvFlags(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
             Tcl_Obj *CONST objv[])
{
    Ns_ObjvTable   *tablePtr = spec->arg;
    int            *dest     = spec->dest;
    int             tableIdx, i, flagc;
    Tcl_Obj       **flagv;

    if (*objcPtr < 1) {
        return TCL_ERROR;
    }
    if (Tcl_ListObjGetElements(interp, objv[0], &flagc, &flagv) != TCL_OK) {
        return TCL_ERROR;
    }
    if (flagc == 0) {
        Tcl_SetResult(interp, "blank flag specification", TCL_STATIC);
        return TCL_ERROR;
    }
    for (i = 0; i < flagc; ++i) {
    	if (Tcl_GetIndexFromObjStruct(interp, flagv[i], tablePtr, 
                                      sizeof(Ns_ObjvTable), "flag", 
                                      TCL_EXACT, &tableIdx) != TCL_OK) {
            return TCL_ERROR;
    	}
        *dest |= tablePtr[tableIdx].value;
    }
    *objcPtr -= 1;

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvBreak --
 *
 *	    Handle '--' option/argument separator.
 *
 * Results:
 *      Always TCL_BREAK.
 *
 * Side effects:
 *      Option processing will end successfully, argument processing 
 *      will begin.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvBreak(Ns_ObjvSpec *spec, Tcl_Interp *interp,
             int *objcPtr, Tcl_Obj *CONST objv[])
{
    return TCL_BREAK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvArgs --
 *
 *	    Count all remaining arguments, leaving zero left unprocessed.
 *
 * Results:
 *	    Always TCL_BREAK.
 *
 * Side effects:
 *	    Argument processing will end sucessfully.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvArgs(Ns_ObjvSpec *spec, Tcl_Interp *interp,
            int *objcPtr, Tcl_Obj *CONST objv[])
{
    *((int *) spec->dest) = *objcPtr;
    *objcPtr = 0;

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclParseArgsObjCmd --
 *
 *	    Implements the ns_parseargs command.
 *
 * Results:
 *	    Tcl result.
 *
 * Side effects:
 *	    Specification may be converted to ns:spec obj type.
 *
 *----------------------------------------------------------------------
 */

int
NsTclParseArgsObjCmd(ClientData clientData, Tcl_Interp *interp, int objc,
                     Tcl_Obj *CONST objv[])
{
    Ns_ObjvSpec   *opts, *args, *specPtr;
    Tcl_Obj      **argv;
    int            argc, doneOpts = 0, status = TCL_OK;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "specification args");
        return TCL_ERROR;
    }
    if (Tcl_ListObjGetElements(interp, objv[2], &argc, &argv) != TCL_OK
        || Tcl_ConvertToType(interp, objv[1], &specType) != TCL_OK) {
        return TCL_ERROR;
    }
    opts = objv[1]->internalRep.twoPtrValue.ptr1;
    args = objv[1]->internalRep.twoPtrValue.ptr2;    
    if (Ns_ParseObjv(opts, args, interp, 0, argc, argv) != NS_OK) {
        return TCL_ERROR;
    }

    /*
     * Set defaults for args which were passed no values and reset the
     * dest pointer for subsequent calls to this command.
     */

    specPtr = opts;
    while (1) {
        if (specPtr->key == NULL) {
            if (doneOpts) {
                break;
            }
            doneOpts = 1;
            specPtr++;
            continue;
        }
        if (status == TCL_OK && specPtr->dest == NULL && specPtr->arg != NULL) {
            status = SetValue(interp, specPtr->key, specPtr->arg);
        }
        specPtr->dest = NULL;
        specPtr++;
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 * SetSpecFromAny --
 *
 *      Attempt to convert a Tcl object to ns:spec type.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
SetSpecFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
{
    Ns_ObjvSpec   *specPtr, *optSpec, *argSpec = NULL;
    Tcl_Obj      **specv, **specPair, *defObjPtr;
    int            numSpecs, specLen, keyLen, i;
    char          *key;

    if (Tcl_ListObjGetElements(interp, objPtr, &numSpecs, &specv) != TCL_OK) {
        return TCL_ERROR;
    }
    optSpec = ns_calloc((size_t) numSpecs + 2, sizeof(Ns_ObjvSpec));
    specPtr = optSpec;

    for (i = 0; i < numSpecs; ++i) {

        /*
         * Check for a default and extract the key.
         */

        if (Tcl_ListObjGetElements(interp, specv[i],
                                   &specLen, &specPair) != TCL_OK) {
            FreeSpecs(optSpec);
            return TCL_ERROR;
        }
        if (specLen == 0 || specLen > 2) {
            Tcl_AppendResult(interp, "wrong # fields in argument specifier \"",
                             Tcl_GetString(specv[i]), "\"", NULL);
            FreeSpecs(optSpec);
            return TCL_ERROR;
        }
        key = Tcl_GetStringFromObj(specPair[0], &keyLen);
        if (specLen == 2) {
            defObjPtr = specPair[1];
        } else if (i + 1 == numSpecs && STREQ(key, "args")) {
            defObjPtr = Tcl_NewListObj(0, NULL);
        } else {
            defObjPtr = NULL;
        }

        /*
         * Examine the key: is this an option or an argument?
         */

        if (key[0] == '\0' || (key[0] == '-' && key[1] == '\0')) {
            Ns_TclPrintfResult(interp,
                "argument or option in position %d has no name", i);
            FreeSpecs(optSpec);
            return TCL_ERROR;
        }
        if (key[0] == '-' && argSpec != NULL) {
            Tcl_AppendResult(interp, "expected argument \"", key, "\"", NULL);
            FreeSpecs(optSpec);
            return TCL_ERROR;
        }
        if (key[0] != '-' && argSpec == NULL) {
            /* Found the first non-option. */
            argSpec = ++specPtr;
        }

        /*
         * Arguments with default values must have their keys prepended
         * with '?' for the runtime parser. Tcl 'args' are always optional.
         */

        if ((key[0] != '-' && defObjPtr != NULL)
            || (i + 1 == numSpecs && STREQ(key, "args"))) {

            specPtr->key = ns_malloc((size_t) keyLen + 2);
            specPtr->key[0] = '?';
            strcpy(specPtr->key + 1, key);
        } else {
            specPtr->key = ns_strdup(key);
        }

        if (defObjPtr != NULL) {
            Tcl_IncrRefCount(defObjPtr);
            specPtr->arg = defObjPtr;
        }
        if (STREQ(key, "--")) {
            specPtr->proc = Ns_ObjvBreak;
        } else if (i + 1 == numSpecs && STREQ(key, "args")) {
            specPtr->proc = ObjvTclArgs;
        } else {
            specPtr->proc = ObjvTcl;
        }
        specPtr++;
    }
    if (argSpec == NULL) {
        argSpec = specPtr;
    }
    Ns_TclSetTwoPtrValue(objPtr, &specType, optSpec, argSpec);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 * FreeSpecs --
 *
 *     Free a contigious array of opt and arg specs.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
FreeSpecs(Ns_ObjvSpec *specPtr)
{
    Ns_ObjvSpec  *saveSpec = specPtr;
    int           doneOpts = 0;

    while(1) {
        if (specPtr->key == NULL) {
            if (doneOpts) {
                break;
            }
            doneOpts = 1;
            specPtr++;
            continue;
        }
        ns_free(specPtr->key);
        if (specPtr->arg != NULL) {
            Tcl_DecrRefCount((Tcl_Obj *) specPtr->arg);
        }
        specPtr++;
    }
    ns_free(saveSpec);
}


/*
 *----------------------------------------------------------------------
 * FreeSpecObj --
 *
 *     This procedure is called to delete the internal rep of a
 *     ns:spec Tcl object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The internal representation of the given object is deleted.
 *
 *----------------------------------------------------------------------
 */

static void
FreeSpecObj(Tcl_Obj *objPtr)
{
    Ns_ObjvSpec *optSpec;

    optSpec = objPtr->internalRep.twoPtrValue.ptr1;
    FreeSpecs(optSpec);

    objPtr->internalRep.twoPtrValue.ptr1 = NULL;
    objPtr->internalRep.twoPtrValue.ptr2 = NULL;
}


/*
 *----------------------------------------------------------------------
 * UpdateStringOfSpec --
 *
 *     This procedure is called to convert a Tcl object from
 *     ns:spec internal form to it's string form.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The string representation of the object is updated.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateStringOfSpec(Tcl_Obj *objPtr)
{
    Ns_ObjvSpec  *specPtr;
    Tcl_Obj      *defaultObj;
    Tcl_DString   ds;
    int           doneOpts = 0;

    Tcl_DStringInit(&ds);
    Tcl_DStringStartSublist(&ds);
    specPtr = (Ns_ObjvSpec *) objPtr->internalRep.twoPtrValue.ptr1;
    while (1) {
        if (specPtr->key == NULL) {
            if (doneOpts) {
                break;
            }
            doneOpts = 1;
            specPtr++;
            continue;
        }
        if (specPtr->arg != NULL) {
            defaultObj = (Tcl_Obj *) specPtr->arg;
            Tcl_DStringStartSublist(&ds);
            Tcl_DStringAppendElement(&ds, specPtr->key);
            Tcl_DStringAppendElement(&ds, Tcl_GetString(defaultObj));
            Tcl_DStringEndSublist(&ds);
        } else {
            Tcl_DStringAppendElement(&ds, specPtr->key);
        }
        specPtr++;
    }
    Tcl_DStringEndSublist(&ds);
    Ns_TclSetStringRep(objPtr, ds.string, ds.length);
    Tcl_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 * DupSpec --
 *
 *     This procedure is called to copy the internal rep of a
 *     ns:spec Tcl object to another object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The internal representation of the target object is updated
 *      and the type is set.
 *
 *----------------------------------------------------------------------
 */

static void
DupSpec(Tcl_Obj *srcObj, Tcl_Obj *dupObj)
{
    Ns_ObjvSpec  *oldOptSpec = srcObj->internalRep.twoPtrValue.ptr1;
    Ns_ObjvSpec  *oldArgSpec = srcObj->internalRep.twoPtrValue.ptr2;
    Ns_ObjvSpec  *optSpec, *argSpec, *specPtr;
    size_t        numSpecs = 2;

    for (specPtr = oldOptSpec; specPtr->key != NULL; specPtr++) {
        numSpecs++;
    }
    for (specPtr = oldArgSpec; specPtr->key != NULL; specPtr++) {
        numSpecs++;
    }

    optSpec = ns_malloc(numSpecs * sizeof(Ns_ObjvSpec));
    memcpy(optSpec, oldOptSpec, numSpecs * sizeof(Ns_ObjvSpec));

    specPtr = optSpec;
    argSpec = NULL;
    while (1) {
        if (specPtr->key == NULL) {
            if (argSpec) {
                break;
            }
            argSpec = ++specPtr;
            continue;
        }
        specPtr->key = ns_strdup(specPtr->key);
        if (specPtr->arg != NULL) {
            Tcl_IncrRefCount((Tcl_Obj *) specPtr->arg);
        }
        specPtr++;
    }
    Ns_TclSetTwoPtrValue(dupObj, &specType, optSpec, argSpec);
}


/*
 *----------------------------------------------------------------------
 *
 * ObjvTcl --
 *
 *      Consume exactly one argument, setting it as the value of a
 *      variable named after the key in the given interp.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

int
ObjvTcl(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *CONST objv[])
{
    if (*objcPtr > 0) {
        if (SetValue(interp, spec->key, objv[0]) != TCL_OK) {
            return TCL_ERROR;
        }
        *objcPtr -= 1;
        spec->dest = VALUE_SUPPLIED;

        return TCL_OK;
    }
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * ObjvTclArgs --
 *
 *      Consume all remaining args and reset the local variable "args"
 *      in the given interp to contain them.
 *
 * Results:
 *      TCL_OK and objcPtr set to 0, or TCL_ERROR.
 *
 * Side effects:
 *	    Value of existing Tcl variable "args" overwritten.
 *
 *----------------------------------------------------------------------
 */

int
ObjvTclArgs(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *CONST objv[])
{
    Tcl_Obj  *listObj;

    if ((listObj = Tcl_NewListObj(*objcPtr, objv)) == NULL) {
        return TCL_ERROR;
    }
    if (Tcl_SetVar2Ex(interp, "args", NULL, listObj,
                      TCL_LEAVE_ERR_MSG) == NULL) {
        return TCL_ERROR;
    }
    *objcPtr = 0;
    spec->dest = VALUE_SUPPLIED;

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * SetValue --
 *
 *      Strip any leading "-" or "?" from the key and set a variable
 *      with the resulting name.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	    None.
 *
 *----------------------------------------------------------------------
 */

static int
SetValue(Tcl_Interp *interp, char *key, Tcl_Obj *valueObj)
{
    char *name = key;

    if (name[0] == '-' || name[0] == '?') {
        name++;
    }
    if (Tcl_SetVar2Ex(interp, name, NULL, valueObj,
                      TCL_LEAVE_ERR_MSG) == NULL) {
        return TCL_ERROR;
    }
    return TCL_OK;
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
    if (optSpec != NULL) {
        for (specPtr = optSpec; specPtr->key != NULL; ++specPtr) {
            if (STREQ(specPtr->key, "--")) {
                Ns_DStringAppend(&ds, "?--? ");
            } else if (specPtr->proc == &Ns_ObjvBool && specPtr->arg != NULL) {
                Ns_DStringPrintf(&ds, "?%s? ", specPtr->key);
            } else {
                p = specPtr->key;
                if (*specPtr->key == '-') {
                    ++p;
                }
                Ns_DStringPrintf(&ds, "?%s %s? ", specPtr->key, p);
            }
        }
    }
    if (argSpec != NULL) {
        for (specPtr = argSpec; specPtr->key != NULL; ++specPtr) {
            Ns_DStringPrintf(&ds, "%s%s ", specPtr->key,
                             (*specPtr->key == '?') ? "?" : "");
        }
    }
    Ns_DStringTrunc(&ds, Ns_DStringLength(&ds) - 1);
    Tcl_WrongNumArgs(interp, objc, objv, ds.string);
    Ns_DStringFree(&ds);
}
