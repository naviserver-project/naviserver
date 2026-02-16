/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 */


/*
 * tclobjv.c --
 *
 *      Routines for parsing the options and arguments passed to Tcl commands.
 *
 */

#include "nsd.h"

#define VALUE_SUPPLIED (INT2PTR(NS_TRUE))
#define OPT_REQUIRED 0x01u
#define OPT_SEEN     0x02u

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
                         const unsigned char *optIsRequired, Tcl_Interp *interp,
                         TCL_SIZE_T preObjc, TCL_SIZE_T objc, Tcl_Obj *const* objv);

static int GetOptIndexObjvSpec(Tcl_Obj *obj, const Ns_ObjvSpec *tablePtr, int *idxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static int GetOptIndexSubcmdSpec(Tcl_Interp *interp, Tcl_Obj *obj, const char *msg, const Ns_SubCmdSpec *tablePtr, int *idxPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static void UpdateStringOfMemUnit(Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1);

static int SetMemUnitFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void AppendRange(Tcl_DString *dsPtr, const Ns_ObjvValueRange *r)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void AppendLiteral(Tcl_DString *dsPtr, const Ns_ObjvSpec *specPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void AppendParameter(Tcl_DString *dsPtr, const char *separator, TCL_SIZE_T separatorLength,
                            bool withRange, bool withDots, const Ns_ObjvSpec *specPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(6);

static char *GetOptEnumeration(Tcl_DString *dsPtr, const Ns_SubCmdSpec *tablePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static char *ObjvTablePrintSentence(Tcl_DString *dsPtr, const Ns_ObjvTable *values)
    NS_GNUC_NONNULL(1, 2);

static Ns_ReturnCode ObjvPreprocess(Ns_ObjvSpec **optSpecPtr, Ns_ObjvSpec *argSpec,
                                    Tcl_Interp *interp,
                                    Tcl_Obj *const* parseObjv, TCL_SIZE_T parseObjc, TCL_SIZE_T parseOffset,
                                    TCL_SIZE_T *requiredArgsPtr,
                                    TCL_SIZE_T *noptsPtr, TCL_SIZE_T *nreqoptsPtr,
                                    Tcl_DString *optDsPtr, unsigned char **optFlagsPtr)
    NS_GNUC_NONNULL(1, 3, 7, 8, 9, 10, 11);

static const char *GetStringIfPure(Tcl_Obj *obj)
    NS_GNUC_NONNULL(1);

/*
 * Static variables defined in this file.
 */
static CONST86 Tcl_ObjType specType = {
    "ns:spec",
    FreeSpecObj,
    DupSpec,
    UpdateStringOfSpec,
    SetSpecFromAny
#ifdef TCL_OBJTYPE_V0
   ,TCL_OBJTYPE_V0
#endif
};

static CONST86 Tcl_ObjType memUnitType = {
    "ns:mem_unit",
    NULL,
    NULL,
    UpdateStringOfMemUnit,
    SetMemUnitFromAny
#ifdef TCL_OBJTYPE_V0
   ,TCL_OBJTYPE_V0
#endif
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
 * GetStringIfPure --
 *
 *      Return the string representation of a Tcl_Obj, but avoid creating
 *      a stringrep for "pure" bytearrays on Tcl < 8.7 (obj->bytes == NULL).
 *
 * Results:
 *      Pointer to string representation (Tcl_GetString()), or NULL when
 *      the object is a pure/proper bytearray on Tcl < 8.7.
 *
 * Side Effects:
 *      May create a string representation (as Tcl_GetString does),
 *      except in the guarded Tcl < 8.7 case where NULL is returned.
 *
 *----------------------------------------------------------------------
 */
static const char *
GetStringIfPure(Tcl_Obj *obj)
{
    NS_NONNULL_ASSERT(obj != NULL);

    if (obj->bytes == NULL && NsTclObjIsByteArray(obj)) {
        return NULL;
    }

    return Tcl_GetString(obj);
}




/*
 *----------------------------------------------------------------------
 *
 * ObjvPreprocess --
 *
 *      Preprocess option/argument specs:
 *
 *      - Required non-positional options are denoted by "!-foo" in optSpec.
 *        Keys are normalized in-place ("!-foo" -> "-foo") and a bitmap is
 *        allocated (optFlags) marking required options.
 *
 *      - Count required positional arguments in argSpec (up to first "?arg").
 *
 *      - Preserve historic behavior: when actual args match the number of
 *        required positional args exactly, skip option parsing and treat the
 *        remaining token(s) as positional values, so e.g. "ns_md5 --" or
 *        "ns_md5 -binary" computes the checksum of the literal argument.
 *
 *        Exception: when the first remaining token is a known option that
 *        expects a value (e.g. "-encoding"), do not skip option parsing, so
 *        missing option arguments are detected.
 *
 * Results:
 *      NS_OK or NS_ERROR (only on internal/preprocessing failure).
 *
 * Side Effects:
 *      May initialize/resize optDs and normalize optSpec keys in-place.
 *
 *----------------------------------------------------------------------
 */
static Ns_ReturnCode
ObjvPreprocess(Ns_ObjvSpec **optSpecPtr, Ns_ObjvSpec *argSpec,
               Tcl_Interp *interp,
               Tcl_Obj *const* parseObjv, TCL_SIZE_T parseObjc, TCL_SIZE_T parseOffset,
               TCL_SIZE_T *requiredArgsPtr,
               TCL_SIZE_T *noptsPtr, TCL_SIZE_T *nreqoptsPtr,
               Tcl_DString *optDsPtr, unsigned char **optFlagsPtr)
{
    Ns_ObjvSpec   *optSpec, *specPtr;
    TCL_SIZE_T     requiredArgs = 0, nopts = 0, nreqopts = 0;

    NS_NONNULL_ASSERT(optSpecPtr != NULL);
    NS_NONNULL_ASSERT(requiredArgsPtr != NULL);
    NS_NONNULL_ASSERT(noptsPtr != NULL);
    NS_NONNULL_ASSERT(nreqoptsPtr != NULL);
    NS_NONNULL_ASSERT(optDsPtr != NULL);
    NS_NONNULL_ASSERT(optFlagsPtr != NULL);

    optSpec = *optSpecPtr;

    *requiredArgsPtr = 0;
    *noptsPtr = 0;
    *nreqoptsPtr = 0;
    *optFlagsPtr = NULL;
    Tcl_DStringInit(optDsPtr);

    /*
     * Count required positional args.
     */
    if (argSpec != NULL) {
        for (specPtr = argSpec; specPtr->key != NULL; specPtr++) {
            if (specPtr->key[0] == '?') {
                break;
            }
            requiredArgs++;
        }
    }
    *requiredArgsPtr = requiredArgs;

    /*
     * Preprocess required non-positional options ("!-foo").
     */
    if (optSpec != NULL && optSpec->key != NULL) {
        TCL_SIZE_T i;

        for (specPtr = optSpec; specPtr->key != NULL; specPtr++) {
            const char *k = specPtr->key;
            nopts++;
            if (k[0] == '!' && k[1] == '-') {
                nreqopts++;
            }
        }
        *noptsPtr = nopts;
        *nreqoptsPtr = nreqopts;

        if (nreqopts > 0) {
#ifdef NS_TCL_PRE9
            if (unlikely(nopts > (TCL_SIZE_T)INT_MAX)) {
                Ns_TclPrintfResult(interp, "too many options");
                return NS_ERROR;
            }
#else
            (void)interp;
#endif
            Tcl_DStringSetLength(optDsPtr, (int)nopts);
            *optFlagsPtr = (unsigned char *)optDsPtr->string;
            memset(*optFlagsPtr, 0, (size_t)nopts);

            for (i = 0; i < nopts; i++) {
                const char *k = optSpec[i].key;
                if (k[0] == '!' && k[1] == '-') {
                    (*optFlagsPtr)[i] |= OPT_REQUIRED;
                    optSpec[i].key = k + 1;  /* strip '!' */
                }
            }
        }
    }

    /*
     * Historic optimization: if argc matches required positional args exactly,
     * then typically there are no options and we can skip option parsing.
     *
     * Do not do this when required non-positional options exist.
     */
    if (optSpec != NULL && nreqopts == 0 && requiredArgs + parseOffset == parseObjc) {
        int skip = 1;

        if (parseObjv != NULL && parseObjc > parseOffset) {
            Tcl_Obj    *obj = parseObjv[parseOffset];
            const char *s   = GetStringIfPure(obj);

            if (s != NULL && s[0] == '-') {

                /* exact "--" keeps the special literal behavior */
                if (!(s[1] == '-' && s[2] == '\0')) {
                    int optIndex;

                    /*
                     * If it is a known option, do not skip option parsing
                     * when the option expects a value. For flag options,
                     * keep skipping so they can be used as literal values.
                     */
                    if (Tcl_IsShared(obj)) {
                        if (GetOptIndexObjvSpec(obj, optSpec, &optIndex) == TCL_OK) {
                            specPtr = optSpec + optIndex;
                            if (specPtr->proc != Ns_ObjvBool && specPtr->proc != Ns_ObjvBreak) {
                                skip = 0;
                            }
                        }
                    } else {
                        if (Tcl_GetIndexFromObjStruct(NULL, obj, optSpec,
                                                      sizeof(Ns_ObjvSpec), "option",
                                                      TCL_EXACT, &optIndex) == TCL_OK) {
                            specPtr = optSpec + optIndex;
                            if (specPtr->proc != Ns_ObjvBool && specPtr->proc != Ns_ObjvBreak) {
                                skip = 0;
                            }
                        }
                    }
                }
            }
        }

        if (skip) {
            *optSpecPtr = NULL;
        }
    }

    return NS_OK;
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
             TCL_SIZE_T parseOffset, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Ns_ObjvSpec    *specPtr;
    int             optIndex;
    Tcl_Obj *const* parseObjv;
    TCL_SIZE_T      parseObjc, remain;
    TCL_SIZE_T      leadOffset = 0;
    TCL_SIZE_T      requiredArgs = 0, nopts = 0, nreqopts = 0;
    unsigned char  *optFlags = NULL;      /* bit0: required, bit1: seen */
    Tcl_DString     optDs;

    NS_NONNULL_ASSERT(interp != NULL);

    parseObjc = objc - leadOffset;
    parseObjv = objv + leadOffset;
    remain = (TCL_SIZE_T)(parseObjc - parseOffset);

    if (ObjvPreprocess(&optSpec, argSpec, interp,
                       parseObjv, parseObjc, parseOffset,
                       &requiredArgs, &nopts, &nreqopts,
                       &optDs, &optFlags) != NS_OK) {
        goto returnerr;
    }

    if (likely(optSpec != NULL) && likely(optSpec->key != NULL)) {

        while (remain > 0) {
            Tcl_Obj *obj = parseObjv[parseObjc - (TCL_SIZE_T)remain];
            int      result;
            const char *s = GetStringIfPure(obj);

            if (s == NULL) {
                break;
            }

            result = Tcl_IsShared(obj)
                ? GetOptIndexObjvSpec(obj, optSpec, &optIndex)
                : Tcl_GetIndexFromObjStruct(NULL, obj, optSpec,
                                            sizeof(Ns_ObjvSpec), "option",
                                            TCL_EXACT, &optIndex);
            if (result != TCL_OK) {
                break;
            }

            --remain;
            specPtr = optSpec + optIndex;
            if (optFlags != NULL) {
                optFlags[optIndex] |= OPT_SEEN;
            }
            result = specPtr->proc(specPtr, interp, &remain, parseObjv + ((TCL_SIZE_T)parseObjc - remain));

            if (result == TCL_BREAK) {
                break;
            } else if (result != TCL_OK) {
                goto returnerr;
            }
        }

        if (nreqopts > 0) {
            TCL_SIZE_T i;

            for (i = 0; i < nopts; i++) {
                if ((optFlags[i] & OPT_REQUIRED) != 0u && (optFlags[i] & OPT_SEEN) == 0u) {
                    goto badargs;  /* produce "wrong # args" usage */
                }
            }
        }
    }
    if (unlikely(argSpec == NULL)) {
        if (remain > 0) {
        badargs:
            WrongNumArgs(optSpec, argSpec, optFlags, interp, leadOffset, parseOffset-leadOffset, objv);
            goto returnerr;
        }
        goto returnok;
    }

    for (specPtr = argSpec; specPtr != NULL && specPtr->key != NULL; specPtr++) {
        if (unlikely(remain == 0)) {
            if (unlikely(specPtr->key[0] != '?')) {
                goto badargs; /* Too few args. */
            }
            goto returnok;
        }
        if (unlikely(specPtr->proc(specPtr, interp, &remain, parseObjv + ((TCL_SIZE_T)parseObjc - remain)))
            != TCL_OK) {
            goto returnerr;
        }
    }
    if (unlikely(remain > 0)) {
        goto badargs; /* Too many args. */
    }
 returnok:
    if (optFlags != NULL) {Tcl_DStringFree(&optDs);}
    return NS_OK;
 returnerr:
    if (optFlags != NULL) {Tcl_DStringFree(&optDs);}
    return NS_ERROR;
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
        Tcl_DStringAppend(dsPtr, "expected time value in range [", TCL_INDEX_NONE);
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
Ns_ObjvInt(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
Ns_ObjvUShort(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
Ns_ObjvLong(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
Ns_ObjvWideInt(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
Ns_ObjvDouble(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
Ns_ObjvBool(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
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
 *      TCL_SIZE_T and the returned string length will be left in it.
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
Ns_ObjvString(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
              Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const char **dest = spec->dest;

        *dest = Tcl_GetStringFromObj(objv[0], (TCL_SIZE_T *) spec->arg);
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
 *      TCL_SIZE_T and the returned string length will be left in it.
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
Ns_ObjvEval(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
              Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const char **dest = spec->dest;

        result = Tcl_EvalObjEx(interp, objv[0], 0);
        if (likely(result == TCL_OK)) {
            *dest = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), (TCL_SIZE_T *) spec->arg);
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
Ns_ObjvByteArray(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
              Tcl_Obj *const* objv)
{
    int result;

    NS_NONNULL_ASSERT(spec != NULL);

    if (likely(*objcPtr > 0)) {
        const unsigned char **dest = spec->dest;

        *dest = Tcl_GetByteArrayFromObj(objv[0], (TCL_SIZE_T *) spec->arg);
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
Ns_ObjvObj(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
 *      convertred Ns_Time* into *spec->dest.
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
Ns_ObjvTime(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
    Tcl_RegisterObjType(&memUnitType);
}

static void
UpdateStringOfMemUnit(Tcl_Obj *objPtr)
{
    long       memUnit;
    TCL_SIZE_T len;
    char       buf[(TCL_INTEGER_SPACE) + 1];

    NS_NONNULL_ASSERT(objPtr != NULL);

    memUnit = PTR2INT((void *) &objPtr->internalRep);
    len = (TCL_SIZE_T)ns_uint64toa(buf, (uint64_t)memUnit);
    Ns_TclSetStringRep(objPtr, buf, len);
}

static int
SetMemUnitFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
{
    Tcl_WideInt memUnit = 0;
    int         result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);

    if (objPtr->typePtr == NS_intTypePtr) {
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
    if (objPtr->typePtr == NS_intTypePtr) {
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
Ns_ObjvMemUnit(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
Ns_ObjvSet(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
Ns_ObjvIndex(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
             Tcl_Obj *const* objv)
{
    const Ns_ObjvTable *tablePtr;
    int                *dest, tableIdx, result;

    NS_NONNULL_ASSERT(spec != NULL);

    tablePtr = spec->arg;
    dest     = spec->dest;

    if (likely(*objcPtr > 0)) {
        result = Tcl_GetIndexFromObjStruct(interp, objv[0], tablePtr,
                                           sizeof(Ns_ObjvTable), "optioXXXXXXXXn",
                                           TCL_EXACT, &tableIdx);
        if (result == TCL_OK) {
            *dest = (int)tablePtr[tableIdx].value;
            *objcPtr -= 1;
        } else {
            Tcl_DString errDs;

            Tcl_DStringInit(&errDs);
            Ns_DStringPrintf(&errDs, "bad option \"%s\": must be ", Tcl_GetString(objv[0]));
            ObjvTablePrintSentence(&errDs, tablePtr);
            Tcl_DStringResult(interp, &errDs);
        }
    } else {
        Ns_TclPrintfResult(interp, "missing argument to %s", spec->key);
        result = TCL_ERROR;
    }

    return result;
}

#ifdef NS_WITH_DEPRECATED_5_0
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
Ns_ObjvFlags(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr,
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
        TCL_SIZE_T flagc;

        result = Tcl_ListObjGetElements(interp, objv[0], &flagc, &flagv);
        if (likely(result == TCL_OK)) {
            if (likely(flagc > 0)) {
                TCL_SIZE_T i;

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
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvBreak --
 *
 *          Handle "--" option/argument separator.
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
             TCL_SIZE_T *UNUSED(objcPtr), Tcl_Obj *const* UNUSED(objv))
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
            TCL_SIZE_T *objcPtr, Tcl_Obj *const* UNUSED(objv))
{
    NS_NONNULL_ASSERT(spec != NULL);

    *((TCL_SIZE_T *) spec->dest) = *(TCL_SIZE_T *)objcPtr;
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
Ns_ObjvServer(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
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
 * Ns_ObjvUrlspaceSpec --
 *
 *      Get Urlspace spec from argument, consume it, put result into "dest".
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
Ns_ObjvUrlspaceSpec(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    NsUrlSpaceContextSpec **dest;
    int                     result = TCL_OK;

    NS_NONNULL_ASSERT(spec != NULL);
    NS_NONNULL_ASSERT(interp != NULL);

    dest = spec->dest;

    if (likely(*objcPtr > 0) && likely(dest != NULL)) {
        NsUrlSpaceContextSpec *specPtr = NsObjToUrlSpaceContextSpec(interp, objv[0]);

        if (likely(specPtr != NULL)) {
            *dest = specPtr;
            *objcPtr -= 1;
        } else {
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
NsTclParseArgsObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    Tcl_Obj  **argv, *argsObj;
    TCL_SIZE_T argc;
    int        status = TCL_OK;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "/argspec/ /arg .../");
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
        if (Ns_ParseObjv(opts, args, interp, 0, (TCL_SIZE_T)argc, argv) != NS_OK) {
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
    TCL_SIZE_T     numSpecs, specLen, keyLen, i;

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
                               "argument or option in position %" PRITcl_Size
                               " has no name", i);
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
        ns_free_const(specPtr->key);
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
ObjvTcl(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
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
ObjvTclArgs(Ns_ObjvSpec *spec, Tcl_Interp *interp, TCL_SIZE_T *objcPtr, Tcl_Obj *const* objv)
{
    Tcl_Obj  *listObj;
    int       result;

    listObj = Tcl_NewListObj((TCL_SIZE_T)*objcPtr, objv);
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

        result = Tcl_EvalEx(interp, value, (TCL_SIZE_T)len, 0);
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
 * AppendRange --
 *
 *      Append Range notation to Tcl_DString.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updating Tcl_DString.
 *
 *----------------------------------------------------------------------
 */

static void
AppendRange(Tcl_DString *dsPtr, const Ns_ObjvValueRange *r)
{
    /* lower bound */
    if (r->minValue == LLONG_MIN) {
        Tcl_DStringAppend(dsPtr, "[MIN,", 5);
    } else {
        Ns_DStringPrintf(dsPtr, "[%" TCL_LL_MODIFIER "d,", r->minValue);
    }

    /* upper bound */
    {
        const long long v     = r->maxValue;
        const long long tsMax = (long long)TCL_SIZE_MAX;  /* Tcl 9: == LLONG_MAX */
        bool isMax = false;

        isMax = (v == tsMax);
        if (!isMax && v == LLONG_MAX) {
            isMax = true;
        }
        if (!isMax && v == (long long)INT_MAX) {
            isMax = true;
        }

        if (isMax) {
            Tcl_DStringAppend(dsPtr, "MAX]", 4);
        } else {
            Ns_DStringPrintf(dsPtr, "%" TCL_LL_MODIFIER "d]", (Tcl_WideInt)v);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ObjvTablePrint --
 *
 *      Append enumeration strings from Ns_ObjvTable to Tcl_DString.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updating Tcl_DString.
 *
 *----------------------------------------------------------------------
 */
char *Ns_ObjvTablePrint(Tcl_DString *dsPtr, Ns_ObjvTable *values)
{
    const char  *key;
    unsigned int lastValue = 0;
    bool         first = NS_TRUE;
    for (key = values->key; key != NULL; key = (++values)->key) {
        if (!first && lastValue == values->value) {
            continue;
        }
        Tcl_DStringAppend(dsPtr, key, TCL_INDEX_NONE);
        Tcl_DStringAppend(dsPtr, "|", 1);
        lastValue = values->value;
        first = NS_FALSE;
    }
    Tcl_DStringSetLength(dsPtr, dsPtr->length - 1);
    return dsPtr->string;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjvTablePrintSentence --
 *
 *      Append enumeration strings from Ns_ObjvTable to Tcl_DString
 *      in style of an sentence (i.e., with comma, and "or")
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updating Tcl_DString.
 *
 *----------------------------------------------------------------------
 */
static char *
ObjvTablePrintSentence(Tcl_DString *dsPtr, const Ns_ObjvTable *values)
{
    size_t       n = 0u;
    bool         first = NS_TRUE;
    unsigned int lastValue = 0u;
    const Ns_ObjvTable *v;

    for (v = values; v->key != NULL; ++v) {
        if (!first && v->value == lastValue) {
            continue;
        }
        ++n;
        lastValue = v->value;
        first = NS_FALSE;
    }

    if (n == 0u) {
        return dsPtr->string;
    } else {
        size_t i;

        first = NS_TRUE;
        lastValue = 0u;

        for (v = values, i = 0u; v->key != NULL; ++v) {
            if (!first && v->value == lastValue) {
                continue;
            }
            ++i;

            if (i == n) {
                Tcl_DStringAppend(dsPtr, ", or ", 5);
            } else if (i > 1u) {
                Tcl_DStringAppend(dsPtr, ", ", 2);
            }

            Tcl_DStringAppend(dsPtr, v->key, TCL_INDEX_NONE);

            lastValue = v->value;
            first = NS_FALSE;
        }
    }

    return dsPtr->string;
}

/*
 *----------------------------------------------------------------------
 *
 * AppendLiteral --
 *
 *      Append literal value to Tcl_DString.
 *      Supported are Booleans and enumeration types.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updating Tcl_DString.
 *
 *----------------------------------------------------------------------
 */
static void
AppendLiteral(Tcl_DString *dsPtr, const Ns_ObjvSpec *specPtr)
{
    if (specPtr->proc == Ns_ObjvBool) {
        Tcl_DStringAppend(dsPtr, "true|false", 10);
    } else if (specPtr->proc == Ns_ObjvIndex) {
        assert(specPtr->arg);
        Ns_ObjvTablePrint(dsPtr, specPtr->arg);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * AppendParameter --
 *
 *      Append a single parameter Tcl_DString.  Supported are
 *      positional and non-positional, optional and non-optional
 *      parameters.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updating Tcl_DString.
 *
 *----------------------------------------------------------------------
 */

static void
AppendParameter(Tcl_DString *dsPtr, const char *separator, TCL_SIZE_T separatorLength,
                bool withRange, bool withDots, const Ns_ObjvSpec *specPtr)
{
    const char  *name = specPtr->key;
    const char   firstChar = *name;
    TCL_SIZE_T   nameLength = (TCL_SIZE_T)strlen(name);
    bool         isLiteral = (specPtr->proc == Ns_ObjvBool || specPtr->proc == Ns_ObjvIndex);

    /*
     * For optional parameter, use always the question mark as
     * separator.
     */
    if (firstChar == '?') {
        separator = "?";
        separatorLength = 1;
    }

    if (firstChar == '-') {
        Tcl_DStringAppend(dsPtr, separator, separatorLength);
        Tcl_DStringAppend(dsPtr, name, nameLength);

        /*
         * Is this a Boolean switch? If not, output the argument of the
         * non positional parameter.
         */
        if (specPtr->proc != Ns_ObjvBool || specPtr->arg == NULL) {
            Ns_ObjvProc *objvProc = specPtr->proc;

            Tcl_DStringAppend(dsPtr, " ", 1);
            if (isLiteral) {
                AppendLiteral(dsPtr, specPtr);
            } else {
                /*
                 * Non-literal cases. Use placeholder syntax.
                 */
                Tcl_DStringAppend(dsPtr, "/", 1);

                if (objvProc == Ns_ObjvString || objvProc == Ns_ObjvObj) {
                    Tcl_DStringAppend(dsPtr, "value", 5);
                } else if (objvProc == Ns_ObjvByteArray) {
                    Tcl_DStringAppend(dsPtr, "data", 4);
                } else if (objvProc == Ns_ObjvMemUnit) {
                    Tcl_DStringAppend(dsPtr, "memory-size", 11);
                } else if (objvProc == Ns_ObjvTime) {
                    Tcl_DStringAppend(dsPtr, "time", 4);
                } else if (objvProc == Ns_ObjvSet) {
                    Tcl_DStringAppend(dsPtr, "setId", 5);
                } else if (objvProc == Ns_ObjvServer) {
                    Tcl_DStringAppend(dsPtr, "server", 6);
                } else if (objvProc == Ns_ObjvWideInt || objvProc == Ns_ObjvInt || objvProc == Ns_ObjvLong) {
                    Tcl_DStringAppend(dsPtr, "integer", 7);
                } else if (objvProc == Ns_ObjvUShort) {
                    Tcl_DStringAppend(dsPtr, "port", 4);
                } else {
                    /*
                     * No type information, repeat the variable name
                     */
                    /*fprintf(stderr, "PARAMETER PRINT: fall back to <%s> %s\n", name + 1,
                      objvProc == Ns_ObjvObj ? "(generic object)" : "");*/
                    Tcl_DStringAppend(dsPtr, name + 1, nameLength - 1);
                }
                if (withRange) {
                    AppendRange(dsPtr, specPtr->arg);
                } else if (withDots) {
                    Tcl_DStringAppend(dsPtr, " ...", 4);
                }
                Tcl_DStringAppend(dsPtr, "/", 1);
            }
        }
        Tcl_DStringAppend(dsPtr, separator, separatorLength);

    } else if (firstChar == '?') {
        /*
         * Optional positional parameter, just placeholder are supported.
         */
        if (isLiteral) {
            /*
             * We have to provide question marks in the literal case
             * manually.
             */
            Tcl_DStringAppend(dsPtr, "?", 1);
            AppendLiteral(dsPtr, specPtr);
            Tcl_DStringAppend(dsPtr, "?", 1);
        } else {
            /*
             * Placeholder notation
             */
            Tcl_DStringAppend(dsPtr, separator, separatorLength);
            Tcl_DStringAppend(dsPtr, "/", 1);
            Tcl_DStringAppend(dsPtr, name + 1, nameLength - 1);
            if (withRange) {
                AppendRange(dsPtr, specPtr->arg);
            } else if (withDots) {
                Tcl_DStringAppend(dsPtr, " ...", 4);
            }
            Tcl_DStringAppend(dsPtr, "/", 1);
            Tcl_DStringAppend(dsPtr, separator, separatorLength);
        }
    } else {
        /*
         * Required positional parameter.
         */
        if (isLiteral) {
            AppendLiteral(dsPtr, specPtr);
        } else {
            /*
             * Placeholder notation
             */
            Tcl_DStringAppend(dsPtr, separator, separatorLength);
            Tcl_DStringAppend(dsPtr, name, nameLength);
            if (withRange) {
                AppendRange(dsPtr, specPtr->arg);
            } else if (withDots) {
                Tcl_DStringAppend(dsPtr, " ...", 4);
            }
            Tcl_DStringAppend(dsPtr, separator, separatorLength);
        }
    }

    /*
     * Append space at the end.
     */
    Tcl_DStringAppend(dsPtr, " ", 1);
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

static void
WrongNumArgs(const Ns_ObjvSpec *optSpec, Ns_ObjvSpec *argSpec,
             const unsigned char *optFlags, Tcl_Interp *interp,
             TCL_SIZE_T preObjc, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const Ns_ObjvSpec *specPtr;
    Tcl_DString        ds;

    Tcl_DStringInit(&ds);

    if (optSpec != NULL) {
        TCL_SIZE_T idx = 0;

        for (specPtr = optSpec; specPtr->key != NULL; ++specPtr, ++idx) {
            int required = (optFlags != NULL && (optFlags[idx] & OPT_REQUIRED) != 0u);

            if (STREQ(specPtr->key, "--")) {
                Tcl_DStringAppend(&ds, "?--? ", 5);
            } else {
                const char *wrap = required ? "" : "?";
                TCL_SIZE_T  wlen = required ? 0  : 1;

                AppendParameter(&ds, wrap, wlen,
                                ((specPtr->proc == Ns_ObjvInt
                                  || specPtr->proc == Ns_ObjvLong
                                  || specPtr->proc == Ns_ObjvWideInt
                                  ) && specPtr->arg != NULL),
                                (specPtr->proc == Ns_ObjvArgs),
                                specPtr);
            }
        }
    }
    if (argSpec != NULL) {
        for (specPtr = argSpec; specPtr->key != NULL; ++specPtr) {
            AppendParameter(&ds, "/", 1,
                            ((specPtr->proc == Ns_ObjvInt
                              || specPtr->proc == Ns_ObjvLong
                              || specPtr->proc == Ns_ObjvWideInt
                              ) && specPtr->arg != NULL),
                            (specPtr->proc == Ns_ObjvArgs),
                            specPtr);
        }
    }

    if (ds.length > 0) {
        /*
         * Strip last blank character.
         */
        Tcl_DStringSetLength(&ds, ds.length - 1);
        /*Ns_Log(Notice, ".... call tclwrongnumargs %d size %lu <%s>", objc+preObjc, sizeof(objc), ds.string);*/
        Tcl_WrongNumArgs(interp, (TCL_SIZE_T)objc+preObjc, objv, ds.string);
    } else {
        Tcl_WrongNumArgs(interp, (TCL_SIZE_T)objc, objv, NULL);
    }

    Tcl_DStringFree(&ds);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SubcmdObjvGetOptEnumeration --
 *
 *      Get an enumeration string containing the key items of the input
 *      table separated by vertical bars into the provided Tcl_DString.
 *
 * Results:
 *      string
 *
 * Side effects:
 *      None
 *
 *----------------------------------------------------------------------
 */
static char *
GetOptEnumeration(Tcl_DString *dsPtr, const Ns_SubCmdSpec *tablePtr) {
    const Ns_SubCmdSpec *entryPtr;

    for (entryPtr = tablePtr; entryPtr->key != NULL;  entryPtr++) {
        Tcl_DStringAppend(dsPtr, entryPtr->key, TCL_INDEX_NONE);
        Tcl_DStringAppend(dsPtr, "|", 1);
    }
    Tcl_DStringSetLength(dsPtr, dsPtr->length - 1);
    return dsPtr->string;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SubsubcmdObjv --
 *
 *      Dispatch to a subcommand implementation selected from the
 *      provided subcommand table, while supporting nested subcommand
 *      hierarchies via an offset.
 *
 *      The offset specifies how many leading words belong to the
 *      already-processed command prefix. This is used to:
 *
 *        - locate the selector word at objv[1 + offset]
 *        - generate "wrong # args" usage messages with the proper
 *          prefix (Tcl_WrongNumArgs() uses 1 + offset)
 *        - prefix "bad subcommand" errors with the main command words
 *          (objv[0..offset])
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      Sets the Tcl interpreter result on errors and calls the selected
 *      subcommand procedure, which may have additional side effects.
 *
 *----------------------------------------------------------------------
 */
int
Ns_SubsubcmdObjv(const Ns_SubCmdSpec *subcmdSpec, ClientData clientData,
                 Tcl_Interp *interp, TCL_SIZE_T offset,
                 TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    const TCL_SIZE_T prefixc       = offset + 1;   /* words before selector */
    const TCL_SIZE_T selectorIndex = prefixc;      /* objv index of selector */
    Tcl_Obj         *selectorObj;
    int opt = 0, result;

    if (objc - offset < 2) {
        Tcl_DString ds;
        Tcl_DStringInit(&ds);
        (void) GetOptEnumeration(&ds, subcmdSpec);
        Tcl_DStringAppend(&ds, " ?/arg .../?", 11);

        Tcl_WrongNumArgs(interp, prefixc, objv, ds.string);

        Tcl_DStringFree(&ds);
        return TCL_ERROR;
    }

    selectorObj = objv[selectorIndex];

    result = Tcl_IsShared(selectorObj)
        ? GetOptIndexSubcmdSpec(interp, selectorObj, "subcommand", subcmdSpec, &opt)
        : Tcl_GetIndexFromObjStruct(interp, selectorObj, subcmdSpec, sizeof(Ns_SubCmdSpec),
                                    "subcommand", TCL_EXACT, &opt);

    if (likely(result == TCL_OK)) {
        return (*subcmdSpec[opt].proc)(clientData, interp, objc, objv);
    }

    /* Include prefix words (objv[0..prefixc-1]) in error message. */
    {
        Tcl_DString ds;
        TCL_SIZE_T  i;

        Tcl_DStringInit(&ds);
        for (i = 0; i < prefixc; i++) {
            TCL_SIZE_T len;
            const char *s = Tcl_GetStringFromObj(objv[i], &len);
            Tcl_DStringAppend(&ds, s, len);
            Tcl_DStringAppend(&ds, " ", 1);
        }
        Tcl_DStringSetLength(&ds, ds.length - 1);

        Ns_TclPrintfResult(interp, "%s: %s",
                           ds.string,
                           Tcl_GetString(Tcl_GetObjResult(interp)));
        Tcl_DStringFree(&ds);
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_SubcmdObjv --
 *
 *      Backward-compatible shim for Ns_SubsubcmdObjv() for commands
 *      where the subcommand selector is the second word (offset 0).
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      See Ns_SubsubcmdObjv().
 *
 *----------------------------------------------------------------------
 */
int
Ns_SubcmdObjv(const Ns_SubCmdSpec *subcmdSpec, ClientData clientData, Tcl_Interp *interp,
              TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return Ns_SubsubcmdObjv(subcmdSpec, clientData, interp, 0, objc, objv);
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
        Tcl_AppendStringsToObj(resultPtr, "bad ", msg, " \"", key, NS_SENTINEL);

        entryPtr = tablePtr;
        if (entryPtr->key == NULL) {
            /*
             * The table is empty
             */
            Tcl_AppendStringsToObj(resultPtr, "\": no valid options", NS_SENTINEL);
        } else {
            int count = 0;
            /*
             * The table has keys
             */
            Tcl_AppendStringsToObj(resultPtr, "\": must be ", entryPtr->key, NS_SENTINEL);
            entryPtr++;
            while (entryPtr->key != NULL) {
                if ((entryPtr+1)->key == NULL) {
                    Tcl_AppendStringsToObj(resultPtr, (count > 0 ? "," : NS_EMPTY_STRING),
                                           " or ", entryPtr->key, NS_SENTINEL);
                } else {
                    Tcl_AppendStringsToObj(resultPtr, ", ", entryPtr->key, NS_SENTINEL);
                    count++;
                }
                entryPtr++;
            }
        }
        Tcl_SetObjResult(interp, resultPtr);
        Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "INDEX", msg, key, NS_SENTINEL);
    }

   return result;
}

/*
 *----------------------------------------------------------------------
 *
 * EncodedObj --
 *
 *      Helper function result encodings.
 *
 * Results:
 *      Tcl result code
 *
 * Side effects:
 *      Interp result Obj is updated in case of error.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj*
NsEncodedObj(unsigned char *octets, size_t octetLength,
             char *outputBuffer, Ns_BinaryEncoding encoding) {
    char    *origOutputBuffer = outputBuffer;
    Tcl_Obj *resultObj = NULL; /* enumeration is complete, quiet some older compilers */

    NS_NONNULL_ASSERT(octets != NULL);

    if (outputBuffer == NULL && encoding != NS_OBJ_ENCODING_BINARY) {
        /*
         * It is a safe assumption to double the size, since the hex
         * encoding needs the most space.
         */
        outputBuffer = ns_malloc(octetLength * 2u + 1u);
    }

    switch (encoding) {
    case NS_OBJ_ENCODING_BINARY:
        resultObj = Tcl_NewByteArrayObj(octets, (TCL_SIZE_T)octetLength);
        break;

    case NS_OBJ_ENCODING_BASE64URL:
        (void)Ns_HtuuEncode2(octets, octetLength, outputBuffer, 1);
        resultObj = Tcl_NewStringObj(outputBuffer, (TCL_SIZE_T)strlen(outputBuffer));
        break;

    case NS_OBJ_ENCODING_BASE64:
        (void)Ns_HtuuEncode2(octets, octetLength, outputBuffer, 0);
        resultObj = Tcl_NewStringObj(outputBuffer, (TCL_SIZE_T)strlen(outputBuffer));
        break;

    case NS_OBJ_ENCODING_HEX:
        Ns_HexString(octets, outputBuffer, (TCL_SIZE_T)octetLength, NS_FALSE);
        resultObj = Tcl_NewStringObj(outputBuffer, (TCL_SIZE_T)octetLength*2);
        break;
    }

    if (outputBuffer != origOutputBuffer) {
        ns_free(outputBuffer);
    }

    return resultObj;
}

TCL_SIZE_T
NsEncodedObjScratchSize(Ns_BinaryEncoding encoding, size_t octetLength)
{
    switch (encoding) {
    case NS_OBJ_ENCODING_BINARY:
        return (TCL_SIZE_T)octetLength;

    case NS_OBJ_ENCODING_BASE64URL:
    case NS_OBJ_ENCODING_BASE64:
        return (TCL_SIZE_T)(4*((octetLength + 2)/3) + 1);

    case NS_OBJ_ENCODING_HEX:
        return (TCL_SIZE_T)(2*octetLength + 1);
        break;
    }

    Ns_Log(Error, "NsEncodedObjScratchSize called with invalid encoding: %d", (int)encoding);
    return 0;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
