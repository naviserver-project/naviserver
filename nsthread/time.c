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
 *      Adjust an Ns_Time so the values are in range.
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
Ns_AdjTime(Ns_Time *timePtr)
{
    NS_NONNULL_ASSERT(timePtr != NULL);

    if (unlikely(timePtr->usec < 0) && unlikely(timePtr->sec > 0)) {
        timePtr->sec += (timePtr->usec / 1000000L) - 1;
        timePtr->usec = (timePtr->usec % 1000000L) + 1000000L;
    } else if (unlikely(timePtr->usec > 1000000L)) {
        timePtr->sec += timePtr->usec / 1000000L;
        timePtr->usec = timePtr->usec % 1000000L;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_DiffTime --
 *
 *      Determine the difference between values passed in t1 and t0
 *      Ns_Time structures.
 *
 * Results:
 *      -1, 0, or 1 if t1 is before, same, or after t0.
 *
 * Side effects:
 *      None.
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
    assert(sec >= 0);
    assert(usec >= 0);

    timePtr->sec += sec;
    timePtr->usec += usec;
    Ns_AdjTime(timePtr);
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
