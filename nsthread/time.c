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
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_GetTime(Ns_Time *timePtr)
{
    Tcl_Time tbuf;

    Tcl_GetTime(&tbuf);

    timePtr->sec = tbuf.sec;
    timePtr->usec = tbuf.usec;
}


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
    if (timePtr->usec < 0) {
        timePtr->sec += (timePtr->usec / 1000000L) - 1;
        timePtr->usec = (timePtr->usec % 1000000L) + 1000000L;
    } else if (timePtr->usec > 1000000L) {
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
Ns_DiffTime(Ns_Time *t1, Ns_Time *t0, Ns_Time *diffPtr)
{
    Ns_Time diff;
    
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
Ns_IncrTime(Ns_Time *timePtr, time_t sec, long usec)
{
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
 *      Pointer to absPtr if adusted, adjPtr otherwise.
 *
 * Side effects:
 *      Ns_Time structure pointed to by absPtr may be adjusted upwards.
 *
 *----------------------------------------------------------------------
 */

Ns_Time *
Ns_AbsoluteTime(Ns_Time *absPtr, Ns_Time *adjPtr)
{
    if (adjPtr != NULL) {
        if (adjPtr->sec < 1000000000) {
            Ns_GetTime(absPtr);
            Ns_IncrTime(absPtr, adjPtr->sec, adjPtr->usec);
            return absPtr;
        }
    }

    return adjPtr;
}
