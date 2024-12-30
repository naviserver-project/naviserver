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
 * tcltime.c --
 *
 *      A Tcl interface to the Ns_Time microsecond resolution time
 *      routines and some time formatting commands.
 */

#include "nsd.h"

/*
 * math.h is only needed for round()
 *
 * But older Microsoft Windows compilers do not include round() in math.h!  So
 * for them, use the hack below:
 */
#if defined(_MSC_VER) && _MSC_VER <= 1600
static double round(double val) { return floor(val + 0.5); }
#else
#include <math.h>
#endif

/*
 * Local functions defined in this file
 */

static void SetTimeInternalRep(Tcl_Obj *objPtr, const Ns_Time *timePtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int SetTimeFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static void UpdateStringOfTime(Tcl_Obj *objPtr)
    NS_GNUC_NONNULL(1);

static int TmObjCmd(ClientData isGmt, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
    NS_GNUC_NONNULL(2);

static int GetTimeFromString(Tcl_Interp *interp, const char *str, char separator, Ns_Time *tPtr)
    NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(4);

static void DblValueToNstime(Ns_Time *timePtr, double dblValue)
    NS_GNUC_NONNULL(1);

static double ParseTimeUnit(const char *str)
    NS_GNUC_NONNULL(1);

/*
 * Local variables defined in this file.
 */

static CONST86 Tcl_ObjType timeType = {
    "ns:time",
    NULL,
    NULL,
    UpdateStringOfTime,
    SetTimeFromAny
#ifdef TCL_OBJTYPE_V0
   ,TCL_OBJTYPE_V0
#endif
};

static Ns_ObjvValueRange poslongRange0 = {0, LONG_MAX};
static Ns_ObjvTimeRange nonnegTimeRange = {{0, 0}, {LONG_MAX, 0}};


/*
 *----------------------------------------------------------------------
 *
 * NsTclInitTimeType --
 *
 *      Initialize Ns_Time Tcl_Obj type.
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
NsTclInitTimeType(void)
{
#ifndef _WIN32
    Tcl_Obj obj;
    if (sizeof(obj.internalRep) < sizeof(Ns_Time)) {
        Tcl_Panic("NsTclInitTimeType: sizeof(obj.internalRep) < sizeof(Ns_Time)");
    }
#endif
    if (NS_intTypePtr == NULL) {
        Tcl_Panic("tcltime: no tclIntType");
    }
    Tcl_RegisterObjType(&timeType);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclNewTimeObj --
 *
 *      Creates new time object.
 *
 * Results:
 *      Pointer to new time object.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
Ns_TclNewTimeObj(const Ns_Time *timePtr)
{
    Tcl_Obj *objPtr = Tcl_NewObj();

    NS_NONNULL_ASSERT(timePtr != NULL);

    Tcl_InvalidateStringRep(objPtr);
    SetTimeInternalRep(objPtr, timePtr);

    return objPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_TclSetTimeObj --
 *
 *      Set a Tcl_Obj to an Ns_Time object.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      String rep is invalidated and internal rep is set.
 *
 *----------------------------------------------------------------------
 */

void
Ns_TclSetTimeObj(Tcl_Obj *objPtr, const Ns_Time *timePtr)
{

    NS_NONNULL_ASSERT(timePtr != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);

    if (Tcl_IsShared(objPtr)) {
        Tcl_Panic("Ns_TclSetTimeObj called with shared object");
    }
    Tcl_InvalidateStringRep(objPtr);
    SetTimeInternalRep(objPtr, timePtr);
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
int
Ns_TclGetTimeFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Ns_Time *timePtr)
{
    long sec;
    int  result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(timePtr != NULL);

    if (objPtr->typePtr == NS_intTypePtr) {
        if (likely(Tcl_GetLongFromObj(interp, objPtr, &sec) == TCL_OK)) {
            timePtr->sec = sec;
            timePtr->usec = 0;
        } else {
            result = TCL_ERROR;
        }
    } else {
        if (objPtr->typePtr != &timeType) {
            if (unlikely(Tcl_ConvertToType(interp, objPtr, &timeType) != TCL_OK)) {
                result = TCL_ERROR;
            }
        }
        if (likely(objPtr->typePtr == &timeType)) {
            timePtr->sec =  (time_t)objPtr->internalRep.twoPtrValue.ptr1;
            timePtr->usec = PTR2LONG(objPtr->internalRep.twoPtrValue.ptr2);
        }
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_TclGetTimePtrFromObj --
 *
 *      Convert the Tcl_Obj to an Ns_Time type and return a pointer to
 *      its internal representation.
 *
 * Results:
 *      TCL_OK or TCL_ERROR if not a valid Ns_Time.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
int
Ns_TclGetTimePtrFromObj(Tcl_Interp *interp, Tcl_Obj *objPtr, Ns_Time **timePtrPtr)
{
    int result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(timePtrPtr != NULL);

    if (objPtr->typePtr != &timeType) {
        if (unlikely(Tcl_ConvertToType(interp, objPtr, &timeType) != TCL_OK)) {
            result = TCL_ERROR;
        }
    }
    if (likely(objPtr->typePtr == &timeType)) {
        *timePtrPtr = ((Ns_Time *) (void *) &objPtr->internalRep);
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclTimeObjCmd --
 *
 *      Implements "ns_time".
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
NsTclTimeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int     opt, rc = TCL_OK;
    Ns_Time resultTime;

    static const char *const opts[] = {
        "adjust", "diff", "format", "get", "incr", "make",
        "seconds", "microseconds", NULL
    };
    enum {
        TAdjustIdx, TDiffIdx, TFormatIdx, TGetIdx, TIncrIdx, TMakeIdx,
        TSecondsIdx, TMicroSecondsIdx
    };

    if (objc < 2) {
        Tcl_SetObjResult(interp, Tcl_NewLongObj((long)time(NULL)));
        return TCL_OK;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], opts, "subcommand", 0,
                            &opt) != TCL_OK) {
        return TCL_ERROR;
    }

    switch (opt) {
    case TGetIdx:
        {
            if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
                rc = TCL_ERROR;
            } else {
                Ns_GetTime(&resultTime);
                Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&resultTime));
            }
        }
        break;

    case TMakeIdx:
        {
            Ns_ObjvSpec   largs[] = {
                {"sec",   Ns_ObjvLong, &resultTime.sec,  &poslongRange0},
                {"?usec", Ns_ObjvLong, &resultTime.usec, &poslongRange0},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, largs, interp, 2, objc, objv) != NS_OK) {
                rc = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&resultTime));
            }
        }
        break;

    case TIncrIdx:
        {
            Ns_Time        t2 = {0, 0};
            Ns_Time       *tPtr;
            Ns_ObjvSpec    largs[] = {
                {"time",  Ns_ObjvTime, &tPtr,    &nonnegTimeRange},
                {"sec",   Ns_ObjvLong, &t2.sec,  &poslongRange0},
                {"?usec", Ns_ObjvLong, &t2.usec, &poslongRange0},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, largs, interp, 2, objc, objv) != NS_OK) {
                rc = TCL_ERROR;
            } else {
                resultTime = *tPtr;
                Ns_IncrTime(&resultTime, t2.sec, t2.usec);
                Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&resultTime));
            }
        }
        break;

    case TDiffIdx:
        {
            Ns_Time       *tPtr1, *tPtr2;
            Ns_ObjvSpec    largs[] = {
                {"time1",  Ns_ObjvTime, &tPtr1,  NULL},
                {"time2",  Ns_ObjvTime, &tPtr2,  NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, largs, interp, 2, objc, objv) != NS_OK) {
                rc = TCL_ERROR;
            } else {
                (void)Ns_DiffTime(tPtr1, tPtr2, &resultTime);
                Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&resultTime));
            }
        }
        break;

    case TAdjustIdx:
        {
            Ns_Time     *tPtr;
            Ns_ObjvSpec  largs[] = {
                {"time",  Ns_ObjvTime, &tPtr,  NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, largs, interp, 2, objc, objv) != NS_OK) {
                rc = TCL_ERROR;
            } else {
                resultTime = *tPtr;
                Ns_AdjTime(&resultTime);
                Tcl_SetObjResult(interp, Ns_TclNewTimeObj(&resultTime));
            }
        }
        break;

    case TSecondsIdx:
    case TMicroSecondsIdx:
        {
            Ns_Time     *tPtr;
            Ns_ObjvSpec  largs[] = {
                {"time",  Ns_ObjvTime, &tPtr,  NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, largs, interp, 2, objc, objv) != NS_OK) {
                rc = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, Tcl_NewLongObj((long)(opt == TSecondsIdx ?
                                                               tPtr->sec : tPtr->usec)));
            }
        }
        break;

    case TFormatIdx:
        {
            Ns_Time     *tPtr;
            Ns_ObjvSpec  largs[] = {
                {"time",  Ns_ObjvTime, &tPtr,  NULL},
                {NULL, NULL, NULL, NULL}
            };

            if (Ns_ParseObjv(NULL, largs, interp, 2, objc, objv) != NS_OK) {
                rc = TCL_ERROR;
            } else {
                Tcl_DString ds, *dsPtr = &ds;

                Tcl_DStringInit(dsPtr);
                Ns_DStringAppendTime(dsPtr, tPtr);

                Tcl_DStringResult(interp, dsPtr);
            }
        }
        break;

    default:
        /* unexpected value */
        assert(opt && 0);
        rc = TCL_ERROR;
        break;
    }

    return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLocalTimeObjCmd, NsTclGmTimeObjCmd --
 *
 *      Implements "ns_gmtime" and "ns_localtime".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      ns_localtime depends on the timezone of the server process.
 *
 *----------------------------------------------------------------------
 */

static int
TmObjCmd(ClientData isGmt, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int              rc = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);

    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        rc = TCL_ERROR;
    } else {
        time_t           now;
        const struct tm *ptm;
        Tcl_Obj         *objPtr[9];

        now = time(NULL);
        if (PTR2INT(isGmt) != 0) {
            ptm = ns_gmtime(&now);
        } else {
            ptm = ns_localtime(&now);
        }
        objPtr[0] = Tcl_NewIntObj(ptm->tm_sec);
        objPtr[1] = Tcl_NewIntObj(ptm->tm_min);
        objPtr[2] = Tcl_NewIntObj(ptm->tm_hour);
        objPtr[3] = Tcl_NewIntObj(ptm->tm_mday);
        objPtr[4] = Tcl_NewIntObj(ptm->tm_mon);
        objPtr[5] = Tcl_NewIntObj(ptm->tm_year);
        objPtr[6] = Tcl_NewIntObj(ptm->tm_wday);
        objPtr[7] = Tcl_NewIntObj(ptm->tm_yday);
        objPtr[8] = Tcl_NewIntObj(ptm->tm_isdst);
        Tcl_SetListObj(Tcl_GetObjResult(interp), 9, objPtr);
    }

    return rc;
}

int
NsTclGmTimeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return TmObjCmd(INT2PTR(1), interp, objc, objv);
}

int
NsTclLocalTimeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    return TmObjCmd(NULL, interp, objc, objv);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclSleepObjCmd --
 *
 *      Sleep with millisecond resolution.
 *      Implements "ns_sleep".
 *
 * Results:
 *      Tcl Result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclSleepObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          rc = TCL_OK;
    Ns_Time     *tPtr = NULL;
    Ns_ObjvSpec  args[] = {
        {"duration", Ns_ObjvTime, &tPtr, &nonnegTimeRange},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        rc = TCL_ERROR;
    } else {
        time_t ms;

        assert(tPtr != NULL);
        ms = Ns_TimeToMilliseconds(tPtr);
        if (ms > 0) {
            Tcl_Sleep((int)ms);
        }
    }

    return rc;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclStrftimeObjCmd --
 *
 *      Implements "ns_fmttime".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Depends on the timezone of the server process.
 *
 *----------------------------------------------------------------------
 */

int
NsTclStrftimeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int               result = TCL_OK;
    long              sec = 0;
    char             *fmt = (char *)"%c";
    Ns_ObjvValueRange range = {0, LONG_MAX};
    Ns_ObjvSpec  args[] = {
        {"time", Ns_ObjvLong,   &sec, &range},
        {"?fmt", Ns_ObjvString, &fmt, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        char        buf[200];
        size_t      bufLength;
        time_t      t;

        t = sec;
        bufLength = strftime(buf, sizeof(buf), fmt, ns_localtime(&t));
        if (unlikely(bufLength == 0u)) {
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "invalid time: ",
                                   Tcl_GetString(objv[1]), (char *)0L);
            result = TCL_ERROR;
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(buf, (TCL_SIZE_T)bufLength));
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * UpdateStringOfTime --
 *
 *      Update the string representation for an Ns_Time object.
 *
 *      Note: This procedure does not free an existing old string rep
 *      so storage will be lost if this has not already been done.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      The object's string is set to a valid string that results from
 *      the Ns_Time-to-string conversion.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateStringOfTime(Tcl_Obj *objPtr)
{
    Ns_Time   *timePtr;
    TCL_SIZE_T len;
    char       buf[(TCL_INTEGER_SPACE * 2) + 1];

    NS_NONNULL_ASSERT(objPtr != NULL);

    timePtr = (Ns_Time *) (void *) &objPtr->internalRep;
    Ns_AdjTime(timePtr);

    if (timePtr->usec == 0 && timePtr->sec >= 0) {
        len = (TCL_SIZE_T)ns_uint64toa(buf, (uint64_t)timePtr->sec);
    } else {
        len = (TCL_SIZE_T)snprintf(buf, sizeof(buf), "%" PRId64 ":%ld",
                                   (int64_t)timePtr->sec, timePtr->usec);
    }
    Ns_TclSetStringRep(objPtr, buf, len);
}


/*
 *----------------------------------------------------------------------
 *
 * ParseTimeUnit --
 *
 *      Parse time units specified after an integer or a float.  Note
 *      that the smallest possible time value is 1 μs based on the
 *      internal representation.
 *
 *      Accepted time units are:
 *         μs, ms, s, m, h, d, w, y
 *
 * Results:
 *      Multiplier relative to seconds.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static double ParseTimeUnit(const char *str)
{
    double multiplier = 1.0;

    /*
     * Skip whitespace
     */
    while (CHARTYPE(space, *str) != 0) {
        str++;
    }

    if (*str == 's' && *(str+1) == '\0') {
        multiplier = 1.0;
    } else if (*str == 'm' && *(str+1) == '\0') {
        multiplier = 60.0;
    } else if (*str == 'h' && *(str+1) == '\0') {
        multiplier = 3600.0;
    } else if (*str == 'd' && *(str+1) == '\0') {
        multiplier = 86400.0;
    } else if (*str == 'w' && *(str+1) == '\0') {
        multiplier = 604800.0;
    } else if (*str == 'y' && *(str+1) == '\0') {
        multiplier = 31536000.0;
    } else if (*str == 'm' && *(str+1) == 's' && *(str+2) == '\0') {
        multiplier = 0.001;
    } else if (*str == '\xce' && *(str+1) == '\xbc' && *(str+2) == 's' && *(str+3) == '\0') {
        /* μ */
        multiplier = 0.000001;
    } else if (*str != '\0') {
        Ns_Log(Warning, "ignoring time unit '%s'", str);
    }

    return multiplier;
}

/*
 *----------------------------------------------------------------------
 *
 * DblValueToNstime --
 *
 *      Convert double value (in seconds) to a NaviServer time value.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updating the Ns_Time value in the first argument.
 *
 *----------------------------------------------------------------------
 */
static void
DblValueToNstime( Ns_Time *timePtr, double dblValue)
{
    NS_NONNULL_ASSERT(timePtr != NULL);

    if (dblValue < 0.0) {
        double posValue = -dblValue;

        /*
         * Calculate with the positive value.
         */
        timePtr->sec = (long)(posValue);
        timePtr->usec = (long)round((posValue - (double)timePtr->sec) * 1000000.0);

        /*
         * Fill in the minus sign at the right place.
         */
        if (timePtr->sec == 0) {
            timePtr->usec = -timePtr->usec;
        } else {
            timePtr->sec = -timePtr->sec;
        }

    } else {
        timePtr->sec = (long)(dblValue);
        timePtr->usec = (long)round((dblValue - (double)timePtr->sec) * 1000000.0);
    }
    /* fprintf(stderr, "gen dbltime %f final sec %ld usec %.06ld float %.10f %.10f long %ld\n",
       dblValue, timePtr->sec, timePtr->usec,
       (dblValue - (double)timePtr->sec),
       round((dblValue - (double)timePtr->sec) * 1000000.0),
       (long)((dblValue - (double)timePtr->sec)));
    */
}



/*
 *----------------------------------------------------------------------
 *
 * GetTimeFromString --
 *
 *      Try to fill ns_Time struct from a string based on a specified
 *      separator (':' or '.'). The colon separator is for the classical
 *      NaviServer time format "sec:usec", whereas the dot is used for the
 *      floating point format.
 *
 * Results:
 *      TCL_OK, TCL_ERROR or TCL_CONTINUE
 *
 * Side effects:
 *      Fill in sec und usec in the specified Ns_Time on success.
 *
 *----------------------------------------------------------------------
 */

static int
GetTimeFromString(Tcl_Interp *interp, const char *str, char separator, Ns_Time *tPtr)
{
    /*
     * Look for the separator
     */
    char *sep;
    int   result = TCL_CONTINUE;
    bool  isNegative;

    NS_NONNULL_ASSERT(str != NULL);
    NS_NONNULL_ASSERT(tPtr != NULL);

    isNegative = (*str == '-');
    sep = strchr(str, INTCHAR(separator));

    if (sep != NULL) {
        int intValue;

        /*
         * First get sec from the string.
         */
        if (unlikely(sep == str)) {
            /*
             * The first character was the separator, treat sec as 0.
             */
            tPtr->sec = 0L;

        } else {
            /*
             * Overwrite the separator with a null-byte to make the
             * first part null-terminated.
             */
            *sep = '\0';
            result = Tcl_GetInt(interp, str, &intValue);
            *sep = separator;
            if (result != TCL_OK) {
                result = TCL_ERROR;
            } else {
                tPtr->sec = (long)intValue;
                result = TCL_OK;
            }
        }

        /*
         * Get usec
         */
        if (result != TCL_ERROR) {
            /*
             * When the separator is a dot, try to get the value in floating
             * point format, which are fractions of a second.
             */
            if (separator == '.') {
                double dblValue;

                if (Tcl_GetDouble(NULL, sep, &dblValue) != TCL_OK) {
                    char *ptr = NULL, *p = sep;
                    long  fraction;

                    /*
                     * Skip separator
                     */
                    p ++;

                    fraction = strtol(p, &ptr, 10);
                    if (likely(p != ptr)) {
                        /*
                         * We could parse at least a part of the string as
                         * integer.
                         */
                        double multiplier = ParseTimeUnit(ptr);
                        double dblFraction = (double)fraction;

                        /*
                         * Shift fraction value to right of the fraction
                         * point.
                         */
                        while (p != ptr) {
                            dblFraction /= 10.0;
                            p ++;
                        }
                        if (isNegative) {
                            /* fprintf(stderr, "GetTimeFromString neg value\n"
                               "GetTimeFromString multiplier %.10f sec %ld dblFraction %.12f\n",
                               multiplier, tPtr->sec, dblFraction);*/
                            DblValueToNstime(tPtr, -1 *
                                             multiplier * ((double)llabs(tPtr->sec) + dblFraction));
                        } else {
                            DblValueToNstime(tPtr, multiplier * ((double)tPtr->sec + dblFraction));
                        }

                        result = TCL_OK;

                    } else {
                        result = TCL_ERROR;
                    }
                } else {
                    tPtr->usec = (long)(dblValue * 1000000.0);
                    result = TCL_OK;
                }

            } else {
                /*
                 * The separator must be the ":", the traditional
                 * separator for sec:usec.
                 */
                assert(separator == ':');

                if (Tcl_GetInt(interp, sep+1, &intValue) != TCL_OK) {
                    result = TCL_ERROR;
                } else {
                    tPtr->usec = (long)intValue;
                    result = TCL_OK;
                }
            }
        }
    } else if  (separator == '.') {
        char *ptr = NULL;
        long sec;

        /*
         * No separator found, so try to interpret the string as integer
         */
        sec = strtol(str, &ptr, 10);

        if (likely(str != ptr)) {
            /*
             * We could parse at least a part of the string as integer.
             */
            double multiplier = ParseTimeUnit(ptr);

            if (multiplier == 1.0) {
                tPtr->sec = sec;
                tPtr->usec = 0;
            } else {
                DblValueToNstime(tPtr, multiplier * (double)sec);
            }
            result = TCL_OK;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SetTimeFromAny --
 *
 *      Attempt to generate an Ns_Time internal representation for the Tcl
 *      object. It interprets integer as seconds, but allows as well the form
 *      sec:usec, sec.fraction, or number plus time unit.
 *
 * Results:
 *      The return value is a standard object Tcl result. If an error occurs
 *      during conversion, an error message is left in the interpreter's
 *      result unless "interp" is NULL.
 *
 * Side effects:
 *      If no error occurs, an int is stored as "objPtr"s internal
 *      representation.
 *
 *----------------------------------------------------------------------
 */
static int
SetTimeFromAny(Tcl_Interp *interp, Tcl_Obj *objPtr)
{
    Ns_Time  t;
    long     sec;
    int      result = TCL_OK;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(objPtr != NULL);

    if (objPtr->typePtr == NS_intTypePtr) {
        /*
         * When the type is "int", usec is 0.
         */
        if (Tcl_GetLongFromObj(interp, objPtr, &sec) != TCL_OK) {
            result = TCL_ERROR;
        } else {
            t.sec = sec;
            t.usec = 0;
        }
    } else {
        result = Ns_GetTimeFromString(interp, Tcl_GetString(objPtr), &t);
    }

    if (result == TCL_OK) {
        /* Ns_AdjTime(&t); */
        SetTimeInternalRep(objPtr, &t);
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_GetTimeFromString --
 *
 *      Convert string to time structure.  Check, if the string contains the
 *      classical NaviServer separator for sec:usec and interpret the string
 *      in this format.  If not, check if this has a "." as separator, and use
 *      a floating point notation. An optional time unit (ms, s, m, h, d) can
 *      be specified immediately after the numeric part.
 *
 * Results:
 *      Tcl result code.
 *
 * Side effects:
 *      If an error occurs and interp is given, leave error message in the
 *      interp.
 *
 *----------------------------------------------------------------------
 */
int
Ns_GetTimeFromString(Tcl_Interp *interp, const char *str, Ns_Time *tPtr)
{
    int result;

    NS_NONNULL_ASSERT(str != NULL);
    NS_NONNULL_ASSERT(tPtr != NULL);

    result = GetTimeFromString(interp, str, ':', tPtr);
    if (result == TCL_CONTINUE) {
        result = GetTimeFromString(interp, str, '.', tPtr);
    }
    if (result == TCL_CONTINUE) {
        /*
         * We should not come here, since the GetTimeFromString() with
         * '.' handles also integers and sets error messages.
         */
        Ns_TclPrintfResult(interp, "expected time value but got \"%s\"", str);
        result = TCL_ERROR;
    }
    /* fprintf(stderr, "GetTimeFromString final " NS_TIME_FMT " -- %d\n", tPtr->sec, tPtr->usec, result);*/

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * SetTimeInternalRep --
 *
 *      Set the internal Ns_Time, freeing a previous internal rep if
 *      necessary.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Object will be an Ns_Time type.
 *
 *----------------------------------------------------------------------
 */

static void
SetTimeInternalRep(Tcl_Obj *objPtr, const Ns_Time *timePtr)
{
    NS_NONNULL_ASSERT(objPtr != NULL);
    NS_NONNULL_ASSERT(timePtr != NULL);

    Ns_TclSetTwoPtrValue(objPtr, &timeType,
                         INT2PTR(timePtr->sec), INT2PTR(timePtr->usec));
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
