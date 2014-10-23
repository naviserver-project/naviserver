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
	FILETIME	    s;
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
    assert(timePtr != NULL);

    if (unlikely(timePtr->usec < 0)) {
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

int
Ns_DiffTime(const Ns_Time *t1, Ns_Time *t0, Ns_Time *diffPtr)
{
    Ns_Time diff;

    assert(t0 != NULL);
    assert(t1 != NULL);
    
    if (diffPtr == NULL) {
        diffPtr = &diff;
    }
    if (t1->usec >= t0->usec) {
        diffPtr->sec = t1->sec - t0->sec;
        diffPtr->usec = t1->usec - t0->usec;
    } else {
        diffPtr->sec = t1->sec - t0->sec - 1;
        diffPtr->usec = 1000000L + t1->usec - t0->usec;
    }
    Ns_AdjTime(diffPtr);
    if (diffPtr->sec < 0) {
        return -1;
    }
    if (diffPtr->sec == 0 && diffPtr->usec == 0) {
        return 0;
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
Ns_IncrTime(Ns_Time *timePtr, long sec, long usec)
{
    assert(timePtr != NULL);

    timePtr->usec += usec;
    timePtr->sec += sec;
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
    assert(absPtr != NULL);

    if (adjPtr != NULL) {
        if (adjPtr->sec < 1000000000) {
            Ns_GetTime(absPtr);
            Ns_IncrTime(absPtr, adjPtr->sec, adjPtr->usec);
            return absPtr;
        }
    }

    return adjPtr;
}
