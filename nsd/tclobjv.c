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

#include "nsd.h"

#define VALUE_SUPPLIED (INT2PTR(NS_TRUE))


/*
 * Static functions defined in this file.
 */

static Tcl_FreeInternalRepProc FreeSpecObj;
static Tcl_DupInternalRepProc  DupSpec;
static Tcl_UpdateStringProc    UpdateStringOfSpec;
static Tcl_SetFromAnyProc      SetSpecFromAny;

static Ns_ObjvProc ObjvTcl;
static Ns_ObjvProc ObjvTclArgs;

static void FreeSpecs(Ns_ObjvSpec *specPtr) 
    NS_GNUC_NONNULL(1);

static int SetValue(Tcl_Interp *interp, const char *key, Tcl_Obj *valueObj) 
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static void WrongNumArgs(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv);

static int GetOptIndex(Tcl_Obj *obj, Ns_ObjvSpec *tablePtr, int *idxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

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
 * GetOptIndex --
 *
 *      Process options similar to Tcl_GetIndexFromObj() but allow
 *      only options (starting with a "-"), allow only exact matches
 *      and don't cache results as internal reps.
 *
 *      Background: Tcl_GetIndexFromObj() validates internal reps
 *      based on the pointer of the base string table, which works
 *      only reliably with static string tables. Since NaviServer
 *      can't use static string tables, these are allocated on the
 *      stack. This can lead to mix-ups for shared objects with the
 *      consequence the the resulting indices might be incorrect,
 *      leading to potential crashes. In order to allow caching, it
 *      should be possible to validate the entries based on other
 *      means, but this requires a different interface.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int 
GetOptIndex(Tcl_Obj *obj, Ns_ObjvSpec *tablePtr, int *idxPtr) 
{
    const Ns_ObjvSpec *entryPtr;
    const char        *key;
    int                idx;

    NS_NONNULL_ASSERT(obj != NULL);
    NS_NONNULL_ASSERT(tablePtr != NULL);    
    NS_NONNULL_ASSERT(idxPtr != NULL);

    key = Tcl_GetString(obj);
    if (*key != '-') {
	return TCL_ERROR;
    }

    for (entryPtr = tablePtr, idx = 0; entryPtr->key != NULL;  entryPtr++, idx++) {
	const char *p1, *p2;

        for (p1 = key, p2 = entryPtr->key; *p1 == *p2; p1++, p2++) {
            if (*p1 == '\0') {
		/*
		 * Both words are at their ends. Match is successful.
		 */
                *idxPtr = idx;
		return TCL_OK;
            }
        }
        if (*p1 == '\0') {
            /*
             * The value is an abbreviation for this entry.
             */
	    return TCL_ERROR;
        }
    }
 
   return TCL_ERROR;
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
Ns_ReturnCode
Ns_ParseObjv(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp,
             int offset, int objc, Tcl_Obj *CONST* objv)
{
    Ns_ObjvSpec  *specPtr = NULL;
    int           optIndex, remain = (objc - offset);

    NS_NONNULL_ASSERT(interp != NULL);

    if (likely(optSpec != NULL) && likely(optSpec->key != NULL)) {

        while (remain > 0) {
	    Tcl_Obj *obj = objv[objc - remain];
            int      result;

	    result = Tcl_IsShared(obj) ? 
		GetOptIndex(obj, optSpec, &optIndex) :
		Tcl_GetIndexFromObjStruct(NULL, obj, optSpec,
					  sizeof(Ns_ObjvSpec), "option",
					  TCL_EXACT, &optIndex);
	    if (result != TCL_OK) {
		break;
	    }

            --remain;
            specPtr = optSpec + optIndex;
            result = specPtr->proc(specPtr, interp, &remain, objv + (objc - remain));

            if (result == TCL_BREAK) {
                break;
            } else if (result != TCL_OK) {
                return NS_ERROR;
            }
        }
    }
    if (unlikely(argSpec == NULL)) {
        if (remain > 0) {
        badargs:
            WrongNumArgs(optSpec, argSpec, interp, offset, objv);
            return NS_ERROR;
        }
        return NS_OK;
    }
    for (specPtr = argSpec; specPtr != NULL && specPtr->key != NULL; specPtr++) {
	if (unlikely(remain == 0)) {
            if (unlikely(specPtr->key[0] != '?')) {
                goto badargs; /* Too few args. */
            }
            return NS_OK;
        }
        if (unlikely(specPtr->proc(specPtr, interp, &remain, objv + (objc - remain)))
            != TCL_OK) {
            return NS_ERROR;
        }
    }
    if (unlikely(remain > 0)) {
        goto badargs; /* Too many args. */
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvInt, Ns_ObjvUShort, Ns_ObjvLong, Ns_ObjvWideInt, Ns_ObjvDouble --
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
           Tcl_Obj *CONST* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        int *dest = spec->dest;
        
        result = Tcl_GetIntFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
        }
    } else {
        result = TCL_ERROR;
    }
    
    return result;
}

int
Ns_ObjvUShort(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *CONST* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        int intValue;
        
        result = Tcl_GetIntFromObj(interp, objv[0], &intValue);
        if (likely(result == TCL_OK)) {
            if (intValue > 65535 || intValue < 0) {
                Ns_TclPrintfResult(interp, "value %d out of range (0..65535)", intValue); 
                result = TCL_ERROR;
            } else {
                unsigned short *dest = spec->dest;

                *dest = (unsigned short)intValue;
                *objcPtr -= 1;
            }
        }
    } else {
        result = TCL_ERROR;
    }
    
    return result;
}
            


int
Ns_ObjvLong(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *CONST* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);
    
    if (likely(*objcPtr > 0)) {
        long *dest = spec->dest;
        
        result = Tcl_GetLongFromObj(interp, objv[0], dest);
        if (result == TCL_OK) {
            *objcPtr -= 1;
        }
    } else {
        result = TCL_ERROR;
    }
    
    return result;
}

int
Ns_ObjvWideInt(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
               Tcl_Obj *CONST* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Tcl_WideInt *dest = spec->dest;

        result = Tcl_GetWideIntFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
        }
    } else {
        result = TCL_ERROR;
    }
    
    return result;
}

int
Ns_ObjvDouble(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *CONST* objv)
{
    int     result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        double *dest = spec->dest;

        result = Tcl_GetDoubleFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
        }
    }
    else {
        result = TCL_ERROR;
    }
    
    return result;
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
Ns_ObjvBool(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *CONST* objv)
{
    int *dest, result;

    NS_NONNULL_ASSERT(spec != NULL);

    dest = spec->dest;
    
    if (spec->arg != NULL) {
	*dest = PTR2INT(spec->arg);
        result = TCL_OK;
    } else {
        if (likely(*objcPtr > 0)) {
            result = Tcl_GetBooleanFromObj(interp, objv[0], dest);
            if (likely(result == TCL_OK)) {
                *objcPtr -= 1;
            }
        } else {
            result = TCL_ERROR;
        }
    }
    return result;
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
Ns_ObjvString(Ns_ObjvSpec *spec, Tcl_Interp *UNUSED(interp), int *objcPtr,
              Tcl_Obj *CONST* objv)
{
    int result;
    
    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
	const char **dest = spec->dest;

        *dest = Tcl_GetStringFromObj(objv[0], (int *) spec->arg);
        *objcPtr -= 1;
        result = TCL_OK;
    } else {
        result = TCL_ERROR;
    }
    
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvEval --
 *
 *      Consume exactly one argument, returning a pointer to the result
 *      of eval into *spec->dest.
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
Ns_ObjvEval(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *CONST* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);
    
    if (likely(*objcPtr > 0)) {
        const char **dest = spec->dest;
        
        result = Tcl_EvalObjEx(interp, objv[0], 0);
        if (likely(result == TCL_OK)) {
            *dest = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), (int *) spec->arg);
            *objcPtr -= 1;
        }
    } else {
        result = TCL_ERROR;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvByteArray --
 *
 *      Consume exactly one argument, returning a pointer to the
 *      raw bytes into *spec->dest.
 *
 *      If spec->arg is != NULL it is assumed to be a pointer to an
 *      int and the number of bytes will be left in it.
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
Ns_ObjvByteArray(Ns_ObjvSpec *spec, Tcl_Interp *UNUSED(interp), int *objcPtr,
              Tcl_Obj *CONST* objv)
{
    int result;
    
    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const unsigned char **dest = spec->dest;
        
        *dest = Tcl_GetByteArrayFromObj(objv[0], (int *) spec->arg);
        *objcPtr -= 1;
        result = TCL_OK;
    } else {
        result = TCL_ERROR;
    }
    return result;
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
Ns_ObjvObj(Ns_ObjvSpec *spec, Tcl_Interp *UNUSED(interp), int *objcPtr,
           Tcl_Obj *CONST* objv)
{
    int result;
    
    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
	Tcl_Obj **dest = spec->dest;

        *dest = objv[0];
        *objcPtr -= 1;
        result = TCL_OK;
    } else {
        result = TCL_ERROR;
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvTime --
 *
 *      Consume exactly one argument, returning a pointer to the
 *      Ns_Time into *spec->dest.
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
Ns_ObjvTime(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *CONST* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Ns_Time **dest = spec->dest;

        result = Ns_TclGetTimePtrFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
        }
    } else {
        result = TCL_ERROR;
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvSet --
 *
 *      Consume exactly one argument, returning a pointer to the
 *      Ns_Time into *spec->dest.
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
Ns_ObjvSet(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *CONST* objv)
{
    int result;
    
    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Ns_Set **dest = spec->dest;

        result = Ns_TclGetSet2(interp, Tcl_GetString(objv[0]), dest);
        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
        }
    } else {
        result = TCL_ERROR;
    }
    
    return result;    
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
             Tcl_Obj *CONST* objv)
{
    const Ns_ObjvTable *tablePtr;
    int                *dest, tableIdx;

    NS_NONNULL_ASSERT(spec != NULL);

    tablePtr = spec->arg;
    dest     = spec->dest;
    
    if (likely(*objcPtr > 0)) {
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
             Tcl_Obj *CONST* objv)
{
    unsigned int       *dest;
    const Ns_ObjvTable *tablePtr;
    int                 result, tableIdx = 0;
    Tcl_Obj           **flagv;

    NS_NONNULL_ASSERT(spec != NULL);
    NS_NONNULL_ASSERT(interp != NULL);

    dest = spec->dest;
    tablePtr = spec->arg;
    
    if (*objcPtr < 1) {
        result = TCL_ERROR;
    } else {
        int flagc;
        
        result = Tcl_ListObjGetElements(interp, objv[0], &flagc, &flagv);
        if (likely(result == TCL_OK)) {
            if (likely(flagc > 0)) {
                int i;
                
                for (i = 0; i < flagc; ++i) {
                    result = Tcl_GetIndexFromObjStruct(interp, flagv[i], tablePtr,
                                                       sizeof(Ns_ObjvTable), "flag",
                                                       TCL_EXACT, &tableIdx);
                    if (unlikely(result != TCL_OK)) {
                        break;
                    }
                }
            } else {
                Tcl_SetResult(interp, "blank flag specification", TCL_STATIC);
                result = TCL_ERROR;
            }
    	}
    }
    
    if (likely(result == TCL_OK)) {
        *dest |= tablePtr[tableIdx].value;
        *objcPtr -= 1;
    }

    return result;
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
Ns_ObjvBreak(Ns_ObjvSpec *UNUSED(spec), Tcl_Interp *UNUSED(interp),
	     int *UNUSED(objcPtr), Tcl_Obj *CONST* UNUSED(objv))
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
Ns_ObjvArgs(Ns_ObjvSpec *spec, Tcl_Interp *UNUSED(interp),
            int *objcPtr, Tcl_Obj *CONST* UNUSED(objv))
{
    NS_NONNULL_ASSERT(spec != NULL);

    *((int *) spec->dest) = *objcPtr;
    *objcPtr = 0;

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvServer --
 *
 *      Get server from argument, consume it, put result into "dest".
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvServer(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *CONST* objv)
{
    NsServer **dest;
    int        result = TCL_OK;

    NS_NONNULL_ASSERT(spec != NULL);
    NS_NONNULL_ASSERT(interp != NULL);

    dest = spec->dest;
    
    if (likely(*objcPtr > 0) && likely(dest != NULL)) {
        NsServer *servPtr = NsGetServer(Tcl_GetString(objv[0]));
        
        if (likely(servPtr != NULL)) {
            *dest = servPtr;
            *objcPtr -= 1;
        } else {
            Ns_TclPrintfResult(interp, "invalid server: '%s'", Tcl_GetString(objv[0]));
            result = TCL_ERROR;
        }
    } else {
        result = TCL_ERROR;
    }

    return result;
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
NsTclParseArgsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    Ns_ObjvSpec   *opts, *args, *specPtr;
    Tcl_Obj      **argv, *argsObj;
    int            argc, doneOpts = 0, status = TCL_OK;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "specification args");
        return TCL_ERROR;
    }
    /*
     * In case both arguments are shared (objv[1] == objv[2]), make
     * sure to decouple these, since Tcl_ListObjGetElements() and
     * Tcl_ConvertToType() would shimmer on the shared object.
     */
    if (objv[2] == objv[1]) {
        argsObj = Tcl_DuplicateObj(objv[2]);
        Tcl_IncrRefCount(argsObj);
    } else {
        argsObj = objv[2];
    }

    if (Tcl_ListObjGetElements(interp, argsObj, &argc, &argv) != TCL_OK
        || Tcl_ConvertToType(interp, objv[1], &specType) != TCL_OK) {
        if (argsObj != objv[2]) {
            Tcl_DecrRefCount(argsObj);
        }
        return TCL_ERROR;
    }
    opts = objv[1]->internalRep.twoPtrValue.ptr1;
    args = objv[1]->internalRep.twoPtrValue.ptr2;
    if (Ns_ParseObjv(opts, args, interp, 0, argc, argv) != NS_OK) {
        if (argsObj != objv[2]) {
            Tcl_DecrRefCount(argsObj);
        }
        return TCL_ERROR;
    }

    /*
     * Set defaults for args which were passed no values and reset the
     * dest pointer for subsequent calls to this command.
     */

    specPtr = opts;
    for (;;) {
        if (specPtr->key == NULL) {
            if (doneOpts != 0) {
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

    if (argsObj != objv[2]) {
        Tcl_DecrRefCount(argsObj);
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

    if (Tcl_ListObjGetElements(interp, objPtr, &numSpecs, &specv) != TCL_OK) {
        return TCL_ERROR;
    }
    optSpec = ns_calloc((size_t) numSpecs + 2u, sizeof(Ns_ObjvSpec));
    specPtr = optSpec;

    for (i = 0; i < numSpecs; ++i) {
	const char *key;

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

            specPtr->key = ns_malloc((size_t)keyLen + 2u);
            specPtr->key[0] = '?';
            memcpy(specPtr->key + 1, key, (size_t)keyLen + 1u);
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

    NS_NONNULL_ASSERT(specPtr != NULL);

    for (;;) {
        if (specPtr->key == NULL) {
            if (doneOpts != 0) {
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
    const Ns_ObjvSpec *specPtr;
    Tcl_Obj           *defaultObj;
    Tcl_DString        ds;
    int                doneOpts = 0;

    Tcl_DStringInit(&ds);
    Tcl_DStringStartSublist(&ds);
    specPtr = (Ns_ObjvSpec *) objPtr->internalRep.twoPtrValue.ptr1;
    for (;;) {
        if (specPtr->key == NULL) {
            if (doneOpts != 0) {
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
    size_t        numSpecs = 2u;

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
    for (;;) {
        if (specPtr->key == NULL) {
            if (argSpec != NULL) {
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

static int
ObjvTcl(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *CONST* objv)
{
    int result;
    
    if (likely(*objcPtr > 0)) {
        result = SetValue(interp, spec->key, objv[0]);
        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
            spec->dest = VALUE_SUPPLIED;
        }
    } else {
        result = TCL_ERROR;
    }
    
    return result;
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

static int
ObjvTclArgs(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *CONST* objv)
{
    Tcl_Obj  *listObj;

    listObj = Tcl_NewListObj(*objcPtr, objv);
    if (listObj == NULL) {
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
 *      If value starts with [ and ends with ] then evaluate Tcl script and assign result
 *      to the variable
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
SetValue(Tcl_Interp *interp, const char *key, Tcl_Obj *valueObj)
{
    size_t      len;
    const char *value;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(key != NULL);
    NS_NONNULL_ASSERT(valueObj != NULL);

    value = Tcl_GetString(valueObj);

    if (key[0] == '-' || key[0] == '?') {
        key++;
    }

    len = strlen(value);
    if (value[0] == '[' && value[len - 1u] == ']') {
        value++;
        len -= 2u;

        if (Tcl_EvalEx(interp, value, (int)len, 0) == TCL_ERROR) {
            return TCL_ERROR;
        }
        valueObj = Tcl_GetObjResult(interp);
    }

    if (Tcl_SetVar2Ex(interp, key, NULL, valueObj,
                      TCL_LEAVE_ERR_MSG) == NULL) {
        return TCL_ERROR;
    }
    Tcl_ResetResult(interp);
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
WrongNumArgs(Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const Ns_ObjvSpec *specPtr;
    Ns_DString         ds;

    Ns_DStringInit(&ds);
    if (optSpec != NULL) {
        for (specPtr = optSpec; specPtr->key != NULL; ++specPtr) {
            if (STREQ(specPtr->key, "--")) {
                Ns_DStringAppend(&ds, "?--? ");
            } else if (specPtr->proc == Ns_ObjvBool && specPtr->arg != NULL) {
                Ns_DStringPrintf(&ds, "?%s? ", specPtr->key);
            } else {
	        const char *p = specPtr->key;
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
    Ns_DStringSetLength(&ds, Ns_DStringLength(&ds) - 1);
    Tcl_WrongNumArgs(interp, objc, objv, ds.string);
    Ns_DStringFree(&ds);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
