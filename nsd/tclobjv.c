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

static void WrongNumArgs(const Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec,
                         Tcl_Interp *interp, int objc, Tcl_Obj *const* objv);

static int GetOptIndexObjvSpec(Tcl_Obj *obj, const Ns_ObjvSpec *tablePtr, int *idxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static int GetOptIndexSubcmdSpec(Tcl_Interp *interp, Tcl_Obj *obj, const char *msg, const Ns_SubCmdSpec *tablePtr, int *idxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static void UpdateStringOfMemUnit(Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1);

static int SetMemUnitFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void AppendRange(Ns_DString *dsPtr, const Ns_ObjvValueRange *r)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * Static variables defined in this file.
 */
static const Tcl_ObjType *intTypePtr;

static const Tcl_ObjType specType = {
    "ns:spec",
    FreeSpecObj,
    DupSpec,
    UpdateStringOfSpec,
    SetSpecFromAny
};

static const Tcl_ObjType memUnitType = {
    "ns:mem_unit",
    NULL,
    NULL,
    UpdateStringOfMemUnit,
    SetMemUnitFromAny
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
NsTclInitSpecType(void)
{
    Tcl_RegisterObjType(&specType);
}


/*
 *----------------------------------------------------------------------
 *
 * GetOptIndexObjvSpec --
 *
 *      Process options similar to Tcl_GetIndexFromObj() but allow
 *      only options (starting with a "-"), allow only exact matches
 *      and don't cache results as internal reps.
 *
 *      Background: Tcl_GetIndexFromObj() validates internal reps
 *      based on the pointer of the base string table, which works
 *      only reliably with *static* string tables. Since NaviServer
 *      can't use static string tables, these tables are allocated on
 *      the stack. This can lead to mix-ups for shared objects with
 *      the consequence the resulting indices might be incorrect,
 *      leading to potential crashes. In order to allow caching, it
 *      should be possible to validate the entries based on other
 *      means, but this requires a different interface.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
GetOptIndexObjvSpec(Tcl_Obj *obj, const Ns_ObjvSpec *tablePtr, int *idxPtr)
{
    const char *key;
    int         result = TCL_ERROR;

    NS_NONNULL_ASSERT(obj != NULL);
    NS_NONNULL_ASSERT(tablePtr != NULL);
    NS_NONNULL_ASSERT(idxPtr != NULL);

    key = Tcl_GetString(obj);
    if (*key == '-') {
        const Ns_ObjvSpec *entryPtr;
        int                idx;

        for (entryPtr = tablePtr, idx = 0; entryPtr->key != NULL;  entryPtr++, idx++) {
            const char *p1, *p2;

            for (p1 = key, p2 = entryPtr->key; *p1 == *p2; p1++, p2++) {
                if (*p1 == '\0') {
                    /*
                     * Both words are at their ends. Match is successful.
                     */
                    *idxPtr = idx;
                    result = TCL_OK;
                    break;
                }
            }
            if (*p1 == '\0') {
                /*
                 * The value is an abbreviation for this entry or was
                 * matched in the inner loop.
                 */
                break;
            }
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseObjv --
 *
 *      Process objv according to given option and arg specs.
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
             int offset, int objc, Tcl_Obj *const* objv)
{
    Ns_ObjvSpec  *specPtr;
    int           optIndex, requiredArgs = 0, remain = (objc - offset);

    NS_NONNULL_ASSERT(interp != NULL);

    /*
     * In case, the number of actual arguments is equal to the number
     * of required arguments, skip option processing and use the
     * provided argument for the required arguments. This way,
     * e.g. "ns_md5 --" will compute the checksum of "--" instead of
     * spitting out an error message about a missing input string.
     */
    if (likely(argSpec != NULL) && likely(optSpec != NULL)) {
        /*
         * Count required args.
         */
        for (specPtr = argSpec; specPtr != NULL && specPtr->key != NULL; specPtr++) {
            if (unlikely(specPtr->key[0] == '?')) {
                break;
            }
            requiredArgs++;
        }
        if (requiredArgs+offset == objc) {
            /*
             * No need to process optional parameters.
             */
            optSpec = NULL;
        }
    }

    if (likely(optSpec != NULL) && likely(optSpec->key != NULL)) {

        while (remain > 0) {
            Tcl_Obj *obj = objv[objc - remain];
            int      result;

#ifdef NS_TCL_PRE87
            /*
             * In case a Tcl_Obj has no stringrep (e.g. a pure/proper
             * byte array), it is assumed that this cannot be an
             * option flag (starting with a '-'). Since
             * GetOptIndexObjvSpec() and Tcl_GetIndexFromObjStruct()
             * create on demand string representations, the "pure"
             * property will be lost and Tcl cannot distinguish later
             * whether it can use the string representation as byte
             * array or not. Fortunately, this dangerous fragility is
             * gone in Tcl 8.7.
             */
            if (obj->bytes == NULL) {
                break;
            }
#endif
            result = Tcl_IsShared(obj) ?
                GetOptIndexObjvSpec(obj, optSpec, &optIndex) :
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
 * Ns_CheckWideRange --
 *
 *      Helper function for range checking based on Tcl_WideInt specification.
 *      In error cases, the function leaves an error message in the interpreter.
 *
 * Results:
 *      TCL_OK or TCL_ERROR;
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Ns_CheckWideRange(Tcl_Interp *interp, const char *name, const Ns_ObjvValueRange *r, Tcl_WideInt value)
{
    int result;

    if (r == NULL || (value >= r->minValue && value <= r->maxValue)) {
        /*
         * No range or valid range.
         */
        result = TCL_OK;
    } else {
        /*
         * Invalid range.
         */
        Tcl_DString ds, *dsPtr = &ds;

        Tcl_DStringInit(dsPtr);
        Tcl_DStringAppend(dsPtr, "expected integer in range ", 26);
        AppendRange(dsPtr, r);
        Ns_DStringPrintf(dsPtr, " for '%s', but got %" TCL_LL_MODIFIER "d", name, value);
        Tcl_DStringResult(interp, dsPtr);

        result = TCL_ERROR;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_CheckTimeRange --
 *
 *      Helper function for time range checking based on Ns_Times.
 *      In error cases, the function leaves an error message in the interpreter.
 *
 * Results:
 *      TCL_OK or TCL_ERROR;
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Ns_CheckTimeRange(Tcl_Interp *interp, const char *name, const Ns_ObjvTimeRange *r, Ns_Time *value)
{
    int result;
    Tcl_DString ds0;

    Tcl_DStringInit(&ds0);
    //fprintf(stderr, "check time range '%s'\n", Ns_DStringAppendTime(&ds0, value));

    if (r == NULL
        || (Ns_DiffTime(value, &r->minValue, NULL) >= 0
            && Ns_DiffTime(value, &r->maxValue, NULL) <= 0) ) {
        /*
         * No range or valid range.
         */
        result = TCL_OK;
    } else {
        /*
         * Invalid range.
         */
        Tcl_DString ds, *dsPtr = &ds;

        Tcl_DStringInit(dsPtr);
        Tcl_DStringAppend(dsPtr, "expected time value in range [", -1);
        if (r->maxValue.sec == LONG_MAX) {
            Ns_DStringAppendTime(dsPtr, &r->minValue);
            Tcl_DStringAppend(dsPtr, "s, MAX],", 8);
        } else {
            Ns_DStringAppendTime(dsPtr, &r->minValue);
            Tcl_DStringAppend(dsPtr, "s , ", 5);
            Ns_DStringAppendTime(dsPtr, &r->maxValue);
            Tcl_DStringAppend(dsPtr, "],", 2);
        }
        Ns_DStringPrintf(dsPtr, " for '%s', but got ", name);
        (void)Ns_DStringAppendTime(dsPtr, value);

        Tcl_DStringResult(interp, dsPtr);

        result = TCL_ERROR;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvInt,
 * Ns_ObjvUShort,
 * Ns_ObjvLong,
 * Ns_ObjvWideInt,
 * Ns_ObjvDouble
 *
 *      Consume exactly one argument, returning its value into dest.
 *      A typical use case for Ns_ObjvUShort is for ports.
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
           Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        int *dest = spec->dest;

        result = Tcl_GetIntFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            if (Ns_CheckWideRange(interp, spec->key, spec->arg, (Tcl_WideInt)*dest) == TCL_OK) {
                *objcPtr -= 1;
            } else {
                result = TCL_ERROR;
            }
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}

int
Ns_ObjvUShort(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        int intValue;

        result = Tcl_GetIntFromObj(interp, objv[0], &intValue);
        if (likely(result == TCL_OK)) {
            /*
             * Check permissible values (USHRT_MAX)
             */
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
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}

int
Ns_ObjvLong(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        long *dest = spec->dest;

        result = Tcl_GetLongFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            if (Ns_CheckWideRange(interp, spec->key, spec->arg, (Tcl_WideInt)*dest) == TCL_OK) {
                *objcPtr -= 1;
            } else {
                result = TCL_ERROR;
            }
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}

int
Ns_ObjvWideInt(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
               Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Tcl_WideInt *dest = spec->dest;

        result = Tcl_GetWideIntFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            if (Ns_CheckWideRange(interp, spec->key, spec->arg, *dest) == TCL_OK) {
                *objcPtr -= 1;
            } else {
                result = TCL_ERROR;
            }
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}

int
Ns_ObjvDouble(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *const* objv)
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
 *      If spec->arg is NULL consume exactly one argument and attempt
 *      conversion to a boolean value.  Otherwise, spec->arg is
 *      treated as an int and placed into spec->dest with zero args
 *      consumed.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *      Tcl_Obj maybe converted to boolean type.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvBool(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *const* objv)
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
            Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
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
 *      Consume exactly one argument, returning a pointer to its
 *      string representation into *spec->dest.
 *
 *      If spec->arg is != NULL it is assumed to be a pointer to an
 *      int and the returned string length will be left in it.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvString(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const char **dest = spec->dest;

        *dest = Tcl_GetStringFromObj(objv[0], (int *) spec->arg);
        *objcPtr -= 1;
        result = TCL_OK;
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvEval --
 *
 *      Consume exactly one argument, returning a pointer to the
 *      result of eval into *spec->dest.
 *
 *      If spec->arg is != NULL it is assumed to be a pointer to an
 *      int and the returned string length will be left in it.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvEval(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *const* objv)
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
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
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
 *          None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvByteArray(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
              Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const unsigned char **dest = spec->dest;

        *dest = Tcl_GetByteArrayFromObj(objv[0], (int *) spec->arg);
        *objcPtr -= 1;
        result = TCL_OK;
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvObj --
 *
 *      Consume exactly one argument, returning its pointer into dest
 *      with no conversion.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
           Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Tcl_Obj **dest = spec->dest;

        *dest = objv[0];
        *objcPtr -= 1;
        result = TCL_OK;
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
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
 *          None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvTime(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Ns_Time **dest = spec->dest;

        result = Ns_TclGetTimePtrFromObj(interp, objv[0], dest);
        if (likely(result == TCL_OK)) {
            result = Ns_CheckTimeRange(interp, spec->key, spec->arg, *dest);
        }

        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetTimeFromObj --
 *
 *      Return the internal value of an Ns_Time Tcl_Obj.  If the value is
 *      specified as integer, the value is interpreted as seconds.
 *
 * Results:
 *      TCL_OK or TCL_ERROR if not a valid Ns_Time.
 *
 * Side effects:
 *      Object is converted to Ns_Time type if necessary.
 *
 *----------------------------------------------------------------------
 */
void
NsTclInitMemUnitType(void)
{
    intTypePtr = Tcl_GetObjType("int");
    if (intTypePtr == NULL) {
        Tcl_Panic("NsTclInitObjs: no int type");
    }
    Tcl_RegisterObjType(&memUnitType);
}

static void
UpdateStringOfMemUnit(Tcl_Obj *objPtr)
{
    long     memUnit;
    int      len;
    char     buf[(TCL_INTEGER_SPACE) + 1];

    NS_NONNULL_ASSERT(objPtr != NULL);

    memUnit = PTR2INT((void *) &objPtr->internalRep);
    len = ns_uint64toa(buf, (uint64_t)memUnit);
    Ns_TclSetStringRep(objPtr, buf, len);
}

static int
SetMemUnitFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
{
    Tcl_WideInt memUnit = 0;
    int         result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);

    if (objPtr->typePtr == intTypePtr) {
        long longValue;
        /*
         * When the type is "int", the memory unit is in bytes.
         */
        if (Tcl_GetLongFromObj(interp, objPtr, &longValue) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            memUnit = longValue;
        }
    } else {
        Ns_ReturnCode status;

        status = Ns_StrToMemUnit(Tcl_GetString(objPtr), &memUnit);
        if (status == NS_ERROR) {
            result = TCL_ERROR;
        }
    }

    if (result == TCL_OK) {
        Ns_TclSetTwoPtrValue(objPtr, &memUnitType, INT2PTR(memUnit), NULL);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetMemUnitFromObj --
 *
 *      Convert a Tcl_Obj with a string value of a memory unit into a Tcl_WideInt.
 *      It has the same interface as e.g. Tcl_GetWideIntFromObj().
 *
 * Results:
 *      TCL_OK or TCL_ERROR if not a valid memory unit string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_TclGetMemUnitFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Tcl_WideInt *memUnitPtr)
{
    int  result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(memUnitPtr != NULL);

    /*
     * Many values come in already as int values. No need to convert
     * these to memUnitType.
     */
    if (objPtr->typePtr == intTypePtr) {
        int intValue;

        if (likely(Tcl_GetIntFromObj(interp, objPtr, &intValue) != TCL_OK)) {
            result = TCL_ERROR;
        } else {
            *memUnitPtr = (Tcl_WideInt)intValue;
        }
    } else {
        /*
         * When the values are already in memUnitType, get the value
         * directly, otherwise convert.
         */
        if (objPtr->typePtr != &memUnitType) {

            if (unlikely(Tcl_ConvertToType(interp, objPtr, &memUnitType) != TCL_OK)) {
                Ns_TclPrintfResult(interp, "invalid memory unit '%s'; "
                                   "valid units kB, MB, GB, KiB, MiB, and GiB",
                                   Tcl_GetString(objPtr));
                result = TCL_ERROR;
            }
        }
        if (likely(objPtr->typePtr == &memUnitType)) {
            *memUnitPtr =  (Tcl_WideInt)objPtr->internalRep.twoPtrValue.ptr1;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvMemUnit --
 *
 *      Consume exactly one argument, returning a pointer to the
 *      memUnit (Tcl_WideInt) into *spec->dest.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvMemUnit(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        Tcl_WideInt *dest = spec->dest;

        result = Ns_TclGetMemUnitFromObj(interp, objv[0], dest);

        if (likely(result == TCL_OK)) {
            if (Ns_CheckWideRange(interp, spec->key, spec->arg, (Tcl_WideInt)*dest) == TCL_OK) {
                *objcPtr -= 1;
            } else {
                result = TCL_ERROR;
            }
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
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
 *      Ns_Set into *spec->dest.
 *
 * Results:
 *      TCL_OK or TCL_ERROR.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvSet(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr,
            Tcl_Obj *const* objv)
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
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
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
 *      table, returning the value at the index of the first match
 *      into dest. It is an error for the argument to contain anything
 *      but one of the table keys.
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
             Tcl_Obj *const* objv)
{
    const Ns_ObjvTable *tablePtr;
    int                *dest, tableIdx, result;

    NS_NONNULL_ASSERT(spec != NULL);

    tablePtr = spec->arg;
    dest     = spec->dest;

    if (likely(*objcPtr > 0)) {
        result = Tcl_GetIndexFromObjStruct(interp, objv[0], tablePtr,
                                           sizeof(Ns_ObjvTable), "option",
                                           TCL_EXACT, &tableIdx);
        if (result == TCL_OK) {
            *dest = (int)tablePtr[tableIdx].value;
            *objcPtr -= 1;
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvFlags --
 *
 *      Treat the next argument as a list of flags and compare them
 *      all with the keys in the specified table.  As flags are
 *      matched the values are ORed togethter and the result is
 *      returned in dest.  An unknown flag causes an error.
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
             Tcl_Obj *const* objv)
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
                Ns_TclPrintfResult(interp, "blank flag specification");
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
 *          Handle '--' option/argument separator.
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
             int *UNUSED(objcPtr), Tcl_Obj *const* UNUSED(objv))
{
    return TCL_BREAK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvArgs --
 *
 *          Count all remaining arguments, leaving zero left
 *          unprocessed.
 *
 * Results:
 *          Always TCL_OK.
 *
 * Side effects:
 *          Argument processing will end successfully.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvArgs(Ns_ObjvSpec *spec, Tcl_Interp *UNUSED(interp),
            int *objcPtr, Tcl_Obj *const* UNUSED(objv))
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
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_ObjvServer(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *const* objv)
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
 *          Implements "ns_parseargs".
 *
 * Results:
 *          Tcl result.
 *
 * Side effects:
 *          Specification may be converted to ns:spec obj type.
 *
 *----------------------------------------------------------------------
 */

int
NsTclParseArgsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    Tcl_Obj  **argv, *argsObj;
    int        argc, status = TCL_OK;

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
        status = TCL_ERROR;

    } else {
        Ns_ObjvSpec   *opts, *args;

        opts = objv[1]->internalRep.twoPtrValue.ptr1;
        args = objv[1]->internalRep.twoPtrValue.ptr2;
        if (Ns_ParseObjv(opts, args, interp, 0, argc, argv) != NS_OK) {
            status = TCL_ERROR;

        } else {
            bool         doneOpts = NS_FALSE;
            Ns_ObjvSpec *specPtr = opts;

            /*
             * Set defaults for args which were passed no values and
             * reset the dest pointer for subsequent calls to this
             * command.
             */

            for (;;) {
                if (specPtr->key == NULL) {
                    if (doneOpts) {
                        break;
                    }
                    doneOpts = NS_TRUE;
                    specPtr++;
                    continue;
                }
                if (status == TCL_OK && specPtr->dest == NULL && specPtr->arg != NULL) {
                    status = SetValue(interp, specPtr->key, specPtr->arg);
                }
                specPtr->dest = NULL;
                specPtr++;
            }

        }
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
 *      Attempt to convert a Tcl_Obj to ns:spec type.
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
            Ns_TclPrintfResult(interp, "wrong # fields in argument specifier \"%s\"",
                               Tcl_GetString(specv[i]));
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
            Ns_TclPrintfResult(interp, "expected argument \"%s\"", key);
            FreeSpecs(optSpec);
            return TCL_ERROR;
        }
        if (key[0] != '-' && argSpec == NULL) {
            /*
             * Found the first non-option.
             */
            argSpec = ++specPtr;
        }

        /*
         * Arguments with default values must have their keys
         * prepended with '?' for the run time parser. Tcl 'args' are
         * always optional.
         */

        if ((key[0] != '-' && defObjPtr != NULL)
            || (i + 1 == numSpecs && STREQ(key, "args"))) {
            char *rewrittenKey  = ns_malloc((size_t)keyLen + 2u);

            *rewrittenKey = '?';
            memcpy(rewrittenKey + 1, key, (size_t)keyLen + 1u);
            specPtr->key = rewrittenKey;
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
 *     Free array of opt and arg specs.
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
    bool          doneOpts = NS_FALSE;

    NS_NONNULL_ASSERT(specPtr != NULL);

    for (;;) {
        if (specPtr->key == NULL) {
            if (doneOpts) {
                break;
            }
            doneOpts = NS_TRUE;
            specPtr++;
            continue;
        }
        ns_free((char *)specPtr->key);
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
 *     ns:spec Tcl_Obj.
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
 *     This procedure is called to convert a Tcl_Obj from
 *     ns:spec internal form to its string form.
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
    bool               doneOpts = NS_FALSE;

    Tcl_DStringInit(&ds);
    Tcl_DStringStartSublist(&ds);
    specPtr = (Ns_ObjvSpec *) objPtr->internalRep.twoPtrValue.ptr1;
    for (;;) {
        if (specPtr->key == NULL) {
            if (doneOpts) {
                break;
            }
            doneOpts = NS_TRUE;
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
 *     ns:spec Tcl_Obj to another object.
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
 *          None.
 *
 *----------------------------------------------------------------------
 */

static int
ObjvTcl(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *const* objv)
{
    int result;

    if (likely(*objcPtr > 0)) {
        result = SetValue(interp, spec->key, objv[0]);
        if (likely(result == TCL_OK)) {
            *objcPtr -= 1;
            spec->dest = VALUE_SUPPLIED;
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
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
 *          Value of existing Tcl variable "args" overwritten.
 *
 *----------------------------------------------------------------------
 */

static int
ObjvTclArgs(Ns_ObjvSpec *spec, Tcl_Interp *interp, int *objcPtr, Tcl_Obj *const* objv)
{
    Tcl_Obj  *listObj;
    int       result;

    listObj = Tcl_NewListObj(*objcPtr, objv);
    if (listObj == NULL) {
        result = TCL_ERROR;
    } else {
        if (Tcl_SetVar2Ex(interp, "args", NULL, listObj,
                          TCL_LEAVE_ERR_MSG) == NULL) {
            result = TCL_ERROR;
        } else {
            *objcPtr = 0;
            spec->dest = VALUE_SUPPLIED;
            result = TCL_OK;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SetValue --
 *
 *      Strip any leading "-" or "?" from the key and set a variable
 *      with the resulting name.  If value starts with "[" and ends
 *      with "]" then evaluate Tcl script and assign result to the
 *      variable.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

static int
SetValue(Tcl_Interp *interp, const char *key, Tcl_Obj *valueObj)
{
    size_t      len;
    const char *value;
    int         result = TCL_OK;

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

        result = Tcl_EvalEx(interp, value, (int)len, 0);
        if (result == TCL_OK) {
            valueObj = Tcl_GetObjResult(interp);
        }
    }

    if (likely(result == TCL_OK)) {
        if (Tcl_SetVar2Ex(interp, key, NULL, valueObj,
                          TCL_LEAVE_ERR_MSG) == NULL) {
            result = TCL_ERROR;
        } else {
            Tcl_ResetResult(interp);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * WrongNumArgs --
 *
 *          Leave a usage message in the interpreters result.
 *
 * Results:
 *          None.
 *
 * Side effects:
 *          None.
 *
 *----------------------------------------------------------------------
 */

static void AppendRange(Ns_DString *dsPtr, const Ns_ObjvValueRange *r)
{
    if (r->minValue == LLONG_MIN) {
        Tcl_DStringAppend(dsPtr, "[MIN,", 5);
    } else {
        Ns_DStringPrintf(dsPtr, "[%" TCL_LL_MODIFIER "d,", r->minValue);
    }

    if (r->maxValue == LLONG_MAX) {
        Tcl_DStringAppend(dsPtr, "MAX]", 4);
    } else {
        Ns_DStringPrintf(dsPtr, "%" TCL_LL_MODIFIER "d]", r->maxValue);
    }
}

static void
WrongNumArgs(const Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec, Tcl_Interp *interp,
             int objc, Tcl_Obj *const* objv)
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
                Ns_DStringPrintf(&ds, "?%s %s", specPtr->key, p);

                if ((specPtr->proc == Ns_ObjvInt
                     || specPtr->proc == Ns_ObjvLong
                     || specPtr->proc == Ns_ObjvWideInt
                     ) && specPtr->arg != NULL) {
                    AppendRange(&ds, specPtr->arg);
                }
                Tcl_DStringAppend(&ds, "? ", 2);
            }
        }
    }
    if (argSpec != NULL) {
        for (specPtr = argSpec; specPtr->key != NULL; ++specPtr) {
            Tcl_DStringAppend(&ds, specPtr->key, -1);

            if ((specPtr->proc == Ns_ObjvInt
                 || specPtr->proc == Ns_ObjvLong
                 || specPtr->proc == Ns_ObjvWideInt
                 ) && specPtr->arg != NULL) {
                AppendRange(&ds, specPtr->arg);
            }
            if (*specPtr->key == '?') {
                Tcl_DStringAppend(&ds, "?", 1);
            }
            Tcl_DStringAppend(&ds, " ", 1);
        }
    }
    if (ds.length > 0) {
        Ns_DStringSetLength(&ds, ds.length - 1);
        Tcl_WrongNumArgs(interp, objc, objv, ds.string);
    } else {
        Tcl_WrongNumArgs(interp, objc, objv, NULL);
    }

    Ns_DStringFree(&ds);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SubcmdObjv --
 *
 *      Call subcommand based on the provided name and associated
 *      functions.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      Depends on the Ns_ObjvTypeProcs which run.
 *
 *----------------------------------------------------------------------
 */
int
Ns_SubcmdObjv(const Ns_SubCmdSpec *subcmdSpec, ClientData clientData, Tcl_Interp *interp,
              int objc, Tcl_Obj *const* objv)
{
    int opt = 0, result;

    if (objc < 2) {
        /*
         * The command was called without selector for the
         * subcmd. With out own machinery (as used in
         * GetOptIndexSubcmdSpec()) we could list the available
         * options, but that is just used for shared objects.
         */
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args?");
        result = TCL_ERROR;
    } else {
        Tcl_Obj *selectorObj = objv[1];
        /*
         * If the obj is shared, don't trust its internal representation.
         */
        result = Tcl_IsShared(selectorObj)
            ? GetOptIndexSubcmdSpec(interp, selectorObj, "subcmd", subcmdSpec, &opt)
            : Tcl_GetIndexFromObjStruct(interp, objv[1], subcmdSpec, sizeof(Ns_SubCmdSpec), "subcmd",
                                        TCL_EXACT, &opt);
        if (likely(result == TCL_OK)) {
            result = (*subcmdSpec[opt].proc)(clientData, interp, objc, objv);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GetOptIndexSubcmdSpec --
 *
 *      Process options similar to Tcl_GetIndexFromObj() but don't
 *      cache results as internal reps.
 *
 *      Background: see GetOptIndexObjvSpec()
 *
 * Results:
 *      Tcl result
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static int
GetOptIndexSubcmdSpec(Tcl_Interp *interp, Tcl_Obj *obj, const char *msg, const Ns_SubCmdSpec *tablePtr, int *idxPtr)
{
    const Ns_SubCmdSpec *entryPtr;
    const char          *key;
    int                  idx, result = TCL_ERROR;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(obj != NULL);
    NS_NONNULL_ASSERT(msg != NULL);
    NS_NONNULL_ASSERT(tablePtr != NULL);
    NS_NONNULL_ASSERT(idxPtr != NULL);

    key = Tcl_GetString(obj);

    for (entryPtr = tablePtr, idx = 0; entryPtr->key != NULL;  entryPtr++, idx++) {
        const char *p1, *p2;

        for (p1 = key, p2 = entryPtr->key; *p1 == *p2; p1++, p2++) {
            if (*p1 == '\0') {
                /*
                 * Both words are at their ends. Match is successful.
                 */
                *idxPtr = idx;
                result = TCL_OK;
                break;
            }
        }
        if (*p1 == '\0') {
            /*
             * The value is an abbreviation for this entry.
             */
            break;
        }
    }

    if (result == TCL_ERROR) {
        Tcl_Obj *resultPtr;

        /*
         * Produce a fancy error message.
         */
        resultPtr = Tcl_NewObj();
        Tcl_AppendStringsToObj(resultPtr, "bad ", msg, " \"", key, (char *)0L);

        entryPtr = tablePtr;
        if (entryPtr->key == NULL) {
            /*
             * The table is empty
             */
            Tcl_AppendStringsToObj(resultPtr, "\": no valid options", (char *)0L);
        } else {
            int count = 0;
            /*
             * The table has keys
             */
            Tcl_AppendStringsToObj(resultPtr, "\": must be ", entryPtr->key, (char *)0L);
            entryPtr++;
            while (entryPtr->key != NULL) {
                if ((entryPtr+1)->key == NULL) {
                    Tcl_AppendStringsToObj(resultPtr, (count > 0 ? "," : NS_EMPTY_STRING),
                                           " or ", entryPtr->key, (char *)0L);
                } else {
                    Tcl_AppendStringsToObj(resultPtr, ", ", entryPtr->key, (char *)0L);
                    count++;
                }
                entryPtr++;
            }
        }
        Tcl_SetObjResult(interp, resultPtr);
        Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "INDEX", msg, key, (char *)0L);
    }

   return result;
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
