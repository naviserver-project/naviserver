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
 * time.c --
 *
 *      Ns_Time support routines.
 */

#include "thread.h"

/*
 *----------------------------------------------------------------------
 *
 * Ns_GetTime --
 *
 *      Get the current time value.
 *
 * Results:
 *       Time fields in timePtr are updated.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
#if defined(HAVE_GETTIMEOFDAY)

void
Ns_GetTime(Ns_Time *timePtr)
{

    /*
     *  When gettimeofday() is available on the platform, use it.
     *  otherwise use other approaches below.
     */
    struct timeval tbuf;

    gettimeofday(&tbuf, NULL);
    timePtr->sec = tbuf.tv_sec;
    timePtr->usec = tbuf.tv_usec;
}

#elif defined(_MSC_VER)

void
Ns_GetTime(Ns_Time *timePtr)
{
    /*
     *  Platform-dependent approach to get the current time value from
     *  Windows via GetSystemTimeAsFileTime()
     *
     *  Note: This version can be used together with Mutex timing under
     *  windows.
     *
     */

    /*
     * Number of 100 nanosecond units from 1601-01-01 to 1970-01-01:
     */
    static const __int64 EPOCH_BIAS = 116444736000000000i64;

    union {
        unsigned __int64    i;
        FILETIME            s;
    } ft;

    GetSystemTimeAsFileTime(&ft.s);
    timePtr->sec  = (long)((ft.i - EPOCH_BIAS) / 10000000i64);
    timePtr->usec = (long)((ft.i / 10i64     ) %  1000000i64);
}

#else

void
Ns_GetTime(Ns_Time *timePtr)
{
    /*
     *  Platform-independent approach to get the current time value
     *  using Tcl_GetTime().
     *
     *  Be aware that calling this function requires Tcl to be
     *  initialized.  Therefore, one MUST NOT call this code from
     *  within DllMain() under windows, otherwise the call is blocked
     *  or the behavior is undefined.  Therefore, don't use this
     *  variant under Windows, when Mutex timing are activated.
     */

    Tcl_Time tbuf;
    Tcl_GetTime(&tbuf);

    timePtr->sec = tbuf.sec;
    timePtr->usec = tbuf.usec;
}
#endif

/*
 *----------------------------------------------------------------------
 *
 * Ns_AdjTime --
 *
 *      Normalizes an Ns_Time structure so that its microseconds field is in
 *      the range [0, 1000000). If the microseconds value (usec) is negative,
 *      the function subtracts the appropriate number of seconds from the
 *      seconds field (sec) and adjusts usec to a positive value. Conversely,
 *      if usec is greater than or equal to 1000000, the overflow is added
 *      to sec and usec is reduced modulo 1000000.  Note that "usec" is only
 *      allowed to be negative, when "sec" == 0 (to express e.g. -0.1sec).
 *      "usec" is kept in the range <1mio.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Modifies the Ns_Time structure pointed to by timePtr in place so that the usec
 *      field falls within the range [0, 1000000).
 *
 *----------------------------------------------------------------------
 */
void
Ns_AdjTime(Ns_Time *timePtr)
{
    NS_NONNULL_ASSERT(timePtr != NULL);

    if (unlikely(timePtr->usec < 0) && unlikely(timePtr->sec > 0)) {
        timePtr->sec += (timePtr->usec / 1000000L) - 1;
        timePtr->usec = (timePtr->usec % 1000000L) + 1000000L;
    } else if (unlikely(timePtr->usec >= 1000000L)) {
        timePtr->sec += timePtr->usec / 1000000L;
        timePtr->usec = timePtr->usec % 1000000L;
    }
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_DiffTime --
 *
 *      Compute the signed time difference between two timestamps (`t1 - t0`)
 *      expressed as `Ns_Time` structures. The result is written into
 *      `diffPtr` (if provided) and normalized to ensure `usec` is always in
 *      the range [0, 1,000,000). The function correctly handles negative
 *      timestamps and returns the relative ordering of the two
 *      times. `Ns_Time` may contain negative seconds and/or microseconds, so
 *      normalization and sign handling are required.
 *
 *      - If both `t1` and `t0` are positive: subtract normally.
 *      - If one is positive and the other negative: sum absolute values and
 *        set sign accordingly.
 *      - If both are negative: subtract magnitudes but carefully determine sign.
 *      - If `diffPtr` is NULL, the computed difference is stored in a local
 *        buffer and discarded.
 *
 *      Example:
 *
 *          Ns_Time t0 = { 10,  500000 };   // 10.5 seconds
 *          Ns_Time t1 = { 13,  200000 };   // 13.2 seconds
 *          Ns_Time diff;
 *
 *          int cmp = Ns_DiffTime(&t1, &t0, &diff);
 *          // diff = { 2, 700000 }, cmp = 1
 *
 *
 * Parameters:
 *      t1       - Pointer to the first timestamp (`Ns_Time`).
 *      t0       - Pointer to the second timestamp (`Ns_Time`).
 *      diffPtr  - Pointer to an `Ns_Time` struct where the result will be stored.
 *                 If NULL, the computed result is ignored.
 *
 * Returns:
 *      < 0    if `t1` < `t0`
 *        0    if `t1` == `t0`
 *      > 0    if `t1` > `t0`
 *
 *
 * Side Effects:
 *      - If `diffPtr` is non-NULL, it is filled with the normalized time delta:
 *          * `diffPtr->sec` = whole seconds difference
 *          * `diffPtr->usec` = remaining microseconds difference
 *      - Calls `Ns_AdjTime()` to normalize the result into canonical form.
 *
 *----------------------------------------------------------------------
 */

long
Ns_DiffTime(const Ns_Time *t1, const Ns_Time *t0, Ns_Time *diffPtr)
{
    Ns_Time diff, t0p, t1p, *t0Ptr, *t1Ptr;
    bool    t0pos, t1pos, subtract, isNegative;

    NS_NONNULL_ASSERT(t0 != NULL);
    NS_NONNULL_ASSERT(t1 != NULL);

    if (diffPtr == NULL) {
        diffPtr = &diff;
    }
    if (t0->sec < 0) {
        t0p.sec = -t0->sec;
        t0p.usec = t0->usec;
        t0pos = NS_FALSE;
    } else if (t0->sec == 0 && t0->usec < 0) {
        t0p.sec = -t0->sec;
        t0p.usec = -t0->usec;
        t0pos = NS_FALSE;
    } else {
        t0p.sec  = t0->sec;
        t0p.usec = t0->usec;
        t0pos = NS_TRUE;
    }

    if (t1->sec < 0) {
        t1p.sec = -t1->sec;
        t1p.usec = t1->usec;
        t1pos = NS_FALSE;
    } else if (t1->sec == 0 && t1->usec < 0) {
        t1p.sec = -t1->sec;
        t1p.usec = -t1->usec;
        t1pos = NS_FALSE;
    } else {
        t1p.sec  = t1->sec;
        t1p.usec = t1->usec;
        t1pos = NS_TRUE;
    }

    if (t1pos) {
        if (t0pos) {
            /*
             * Subtract POS - POS
             */
            subtract = NS_TRUE;
            isNegative = t1p.sec < t0p.sec
                || (t1p.sec == t0p.sec && (t1p.usec < t0p.usec));
            if (isNegative) {
                t0Ptr = &t1p;
                t1Ptr = &t0p;
            } else {
                t0Ptr = &t0p;
                t1Ptr = &t1p;
            }
        } else {
            /*
             * Add POS - NEG
             */
            subtract = NS_FALSE;
            isNegative = NS_FALSE;
            t0Ptr = &t1p;
            t1Ptr = &t0p;
        }
    } else {
        if (t0pos) {
            /*
             * ADD NEG - POS
             */
            subtract = NS_FALSE;
            isNegative = NS_TRUE;
        } else {
            /*
             * Subtract NEG - NEG
             */
            subtract = NS_TRUE;
            isNegative = t0p.sec < t1p.sec
                || (t1p.sec == t0p.sec && (t0p.usec < t1p.usec));
            if (isNegative) {
                t0Ptr = &t0p;
                t1Ptr = &t1p;
            } else {
                t0Ptr = &t1p;
                t1Ptr = &t0p;
            }
        }
    }

    if (subtract) {

        if (t1Ptr->usec >= t0Ptr->usec) {
            diffPtr->sec = t1Ptr->sec - t0Ptr->sec;
            diffPtr->usec = t1Ptr->usec - t0Ptr->usec;
        } else {
            diffPtr->sec = t1Ptr->sec - t0Ptr->sec - 1;
            if (diffPtr->sec < 0) {
                diffPtr->sec = t0Ptr->sec - t1Ptr->sec;
                diffPtr->usec = t0Ptr->usec - t1Ptr->usec;
            } else {
                diffPtr->usec = 1000000L + t1Ptr->usec - t0Ptr->usec;
            }
        }
    } else {

        diffPtr->sec = t0p.sec + t1p.sec;
        diffPtr->usec = t0p.usec + t1p.usec;
    }

    if (isNegative) {
        if (diffPtr->sec == 0) {
            diffPtr->usec = -diffPtr->usec;
        } else {
            diffPtr->sec = -diffPtr->sec;
        }
    }

    Ns_AdjTime(diffPtr);

    if (diffPtr->sec < 0) {
        return -1;
    }
    if (diffPtr->sec == 0) {
        if (diffPtr->usec == 0) {
            return 0;
        } else if (diffPtr->usec < 0) {
            return -1;
        }
    }

    return 1;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_IncrTime --
 *
 *      Increment the given Ns_Time structure with the given number
 *      of seconds and microseconds.
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
Ns_IncrTime(Ns_Time *timePtr, time_t sec, long usec)
{
    NS_NONNULL_ASSERT(timePtr != NULL);

    if (unlikely(usec < 0) || unlikely(sec < 0)) {
        fprintf(stderr, "Ns_IncrTime ignores negative increment sec %" PRId64
                " or usec %ld\n", (int64_t)sec, usec);
    } else {
        timePtr->sec += sec;
        timePtr->usec += usec;
        Ns_AdjTime(timePtr);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AbsoluteTime --
 *
 *      Return an absolute time in the future given adjPtr. Small
 *      values of adjPtr are added to the current time, large values
 *      are assumed to be absolute already. NULL is infinity.
 *
 * Results:
 *      Pointer to absPtr if adjusted, adjPtr otherwise.
 *
 * Side effects:
 *      Ns_Time structure pointed to by absPtr may be adjusted upwards.
 *
 *----------------------------------------------------------------------
 */

Ns_Time *
Ns_AbsoluteTime(Ns_Time *absPtr, Ns_Time *adjPtr)
{
    NS_NONNULL_ASSERT(absPtr != NULL);

    if (adjPtr != NULL) {
        if (adjPtr->sec < 1000000000) {
            Ns_GetTime(absPtr);
            Ns_IncrTime(absPtr, adjPtr->sec, adjPtr->usec);
            return absPtr;
        }
    }

    return adjPtr;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_RelativeTime --
 *
 *      Given an absolute time, return the relative time, relative to the time
 *      when this is called. When the input time is "now + 300s", the result
 *      will be "300s".  Small values are assumed to be relative already.
 *
 * Results:
 *      Pointer to relative time.
 *
 * Side effects:
 *      Ns_Time structure pointed to by relTimePtr may be adjusted upwards.
 *
 *----------------------------------------------------------------------
 */

const Ns_Time *
Ns_RelativeTime(Ns_Time *relTimePtr, const Ns_Time *absoluteTimePtr)
{
    const Ns_Time *resultTimePtr = absoluteTimePtr;

    NS_NONNULL_ASSERT(relTimePtr != NULL);

    if (absoluteTimePtr != NULL) {
        if (absoluteTimePtr->sec > 1000000000) {
            Ns_Time now;

            Ns_GetTime(&now);
            Ns_DiffTime(absoluteTimePtr, &now, relTimePtr);
            resultTimePtr = relTimePtr;
        }
    }
    return resultTimePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_TimeToMilliseconds --
 *
 *      Convert Ns_Time to milliseconds. Make sure that in case the Ns_Time
 *      value is not 0, the result is also not 0.
 *
 * Results:
 *      Time in milliseconds.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
time_t
Ns_TimeToMilliseconds(const Ns_Time *timePtr)
{
    time_t result;

    NS_NONNULL_ASSERT(timePtr != NULL);

    if (likely(timePtr->sec >= 0)) {
        result = timePtr->sec*1000 + timePtr->usec/1000;
    } else {
        result = timePtr->sec*1000 - timePtr->usec/1000;
    }
    if (result == 0 && timePtr->sec == 0 && timePtr->usec != 0) {
        result = 1;
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
