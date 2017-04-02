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
 * httptime.c --
 *
 *      Manipulate times and dates; this is strongly influenced
 *      by HTSUtils.c from CERN. See also RFC 1123.
 */

#include "nsd.h"

/*
 * Local functions defined in this file
 */

static int MakeNum(const char *s)
    NS_GNUC_NONNULL(1);

static int MakeMonth(char *s)
    NS_GNUC_NONNULL(1);


/*
 * Static variables defined in this file
 */

static const char *const month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *const week_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

#ifdef HAVE_TIMEGM
static Ns_Mutex lock;
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_Httptime --
 *
 *      Convert a time_t into a time/date format used in HTTP
 *      (see RFC 1123). If passed-in time is null, then the
 *      current time will be used.
 *
 * Results:
 *      The string time, or NULL if error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_HttpTime(Ns_DString *dsPtr, const time_t *when)
{
    time_t           now;
    const struct tm *tmPtr;
    char            *result = NULL;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    if (when == NULL) {
        now = time(NULL);
        when = &now;
    }
    tmPtr = ns_gmtime(when);
    if (likely(tmPtr != NULL)) {

        /*
         * The format is RFC 1123 "Sun, 06 Nov 1997 09:12:45 GMT"
         * and is locale independant, so English week and month names
         * must always be used.
         */
        
        Ns_DStringPrintf(dsPtr, "%s, %02d %s %d %02d:%02d:%02d GMT",
                         week_names[tmPtr->tm_wday], tmPtr->tm_mday,
                         month_names[tmPtr->tm_mon], tmPtr->tm_year + 1900,
                         tmPtr->tm_hour, tmPtr->tm_min, tmPtr->tm_sec);
        result = dsPtr->string;
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseHttpTime --
 *
 *      Take a time in one of three formats and convert it to a time_t. 
 *      Formats are: "Thursday, 10-Jun-93 01:29:59 GMT", "Thu, 10 
 *      Jan 1993 01:29:59 GMT", or "Wed Jun  9 01:29:59 1993 GMT"
 *
 * Results:
 *      Standard time_t or 0 on error.
 *
 * Side effects:
 *      Unable to parse the Unix epoch because result is 0.
 *
 *----------------------------------------------------------------------
 */

time_t
Ns_ParseHttpTime(char *chars)
{
    char      *s;
    struct tm  timeInfo;
    time_t     t = 0;

    NS_NONNULL_ASSERT(chars != NULL);
    
    /*
     * Find the comma after day-of-week
     *
     * Thursday, 10-Jun-93 01:29:59 GMT
     *         ^
     *         +-- s
     *
     * Thu, 10 Jan 1993 01:29:59 GMT
     *    ^
     *    +-- s
     */

    s = strchr(chars, INTCHAR(','));
    if (s != NULL) {

        /*
         * Advance S to the first non-space after the comma
         * which should be the first digit of the day.
         */

        s++;
        while (*s == ' ') {
            s++;
        }

        /*
         * Figure out which format it is in. If there is a hyphen, then
         * it must be the first format.
         */

        if (strchr(s, INTCHAR('-')) != NULL) {
            if (strlen(s) < 18u) {
                return 0;
            }

            /*
             * The format is:
             *
             * Thursday, 10-Jun-93 01:29:59 GMT
             *           ^
             *           +--s
             */

            timeInfo.tm_mday = MakeNum(s);
            timeInfo.tm_mon = MakeMonth(s + 3);
            timeInfo.tm_year = MakeNum(s + 7);
            timeInfo.tm_hour = MakeNum(s + 10);
            timeInfo.tm_min = MakeNum(s + 13);
            timeInfo.tm_sec = MakeNum(s + 16);
        } else {
            int century;
            if ((int) strlen(s) < 20) {
                return 0;
            }

            /*
             * The format is:
             *
             * Thu, 10 Jan 1993 01:29:59 GMT
             *      ^
             *      +--s
             */

            timeInfo.tm_mday = MakeNum(s);
            timeInfo.tm_mon = MakeMonth(s + 3);
            century = ((100 * MakeNum(s + 7)) - 1900);
            timeInfo.tm_year = century + MakeNum(s + 9);
            timeInfo.tm_hour = MakeNum(s + 12);
            timeInfo.tm_min = MakeNum(s + 15);
            timeInfo.tm_sec = MakeNum(s + 18);
        }
    } else {

        /*
         * No commas, so it must be the third, fixed field, format:
         *
         * Wed Jun  9 01:29:59 1993 GMT
         *
         * Advance s to the first letter of the month.
         */

        s = chars;
        while (*s == ' ') {
            s++;
        }
        if ((int) strlen(s) < 24) {
            return 0;
        }
        timeInfo.tm_mday = MakeNum(s + 8);
        timeInfo.tm_mon  = MakeMonth(s + 4);
        timeInfo.tm_year = MakeNum(s + 22);
        timeInfo.tm_hour = MakeNum(s + 11);
        timeInfo.tm_min  = MakeNum(s + 14);
        timeInfo.tm_sec  = MakeNum(s + 17);
    }

    /*
     * If there are any impossible values, then return 0.
     */

    if (!(timeInfo.tm_sec < 0  || timeInfo.tm_sec  > 59 ||
         timeInfo.tm_min  < 0  || timeInfo.tm_min  > 59 ||
         timeInfo.tm_hour < 0  || timeInfo.tm_hour > 23 ||
         timeInfo.tm_mday < 1  || timeInfo.tm_mday > 31 ||
         timeInfo.tm_mon  < 0  || timeInfo.tm_mon  > 11 ||
         timeInfo.tm_year < 70 || timeInfo.tm_year > 120)) {

        timeInfo.tm_isdst = 0;
#ifdef HAVE_TIMEGM
        /*
         * Initialize the mutex (if this did not happen so far) and
         * provide a name for it.
         */
        if (lock == NULL) {
            Ns_MasterLock();
            if (lock == NULL) {
                Ns_MutexInit(&lock);
                Ns_MutexSetName2(&lock, "ns:httptime", NULL);
            }
            Ns_MasterUnlock();
        }
        Ns_MutexLock(&lock);
        t = timegm(&timeInfo);
        Ns_MutexUnlock(&lock);
#else
        t = mktime(&timeInfo) - timezone;
#endif
    }
    return t;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclParseHttpTimeObjCmd --
 *
 *      Implements ns_parsehttptime as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      Day and month names may have capitalisation bashed.
 *
 *----------------------------------------------------------------------
 */

int
NsTclParseHttpTimeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;
    char       *timeString;
    Ns_ObjvSpec args[] = {
        {"httptime", Ns_ObjvString,  &timeString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        time_t t = Ns_ParseHttpTime(timeString);
        
        if (likely(t != 0)) {
            Tcl_SetObjResult(interp, Tcl_NewWideIntObj(t));
        } else {
            Ns_TclPrintfResult(interp, "invalid time: %s", timeString);
            result = TCL_ERROR;
        }

    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclHttpTimeObjCmd --
 *
 *      Implements ns_httptime as obj command. 
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
NsTclHttpTimeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK, itime;
    Ns_ObjvSpec args[] = {
        {"time", Ns_ObjvInt,  &itime, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_DString ds;
        time_t     t = (time_t) itime;
        
        Ns_DStringInit(&ds);
        (void) Ns_HttpTime(&ds, &t);
        Tcl_DStringResult(interp, &ds);
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * MakeNum --
 *
 *      Convert a one or two-digit day into an integer, allowing a
 *      space in the first position.
 *
 * Results:
 *      An integer.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
MakeNum(const char *s)
{
    int result;
    
    NS_NONNULL_ASSERT(s != NULL);
    
    if (CHARTYPE(digit, *s) != 0) {
        result = (10 * ((int)UCHAR(*s) - (int)UCHAR('0'))) + ((int)UCHAR(*(s + 1)) - (int)UCHAR('0'));
    } else {
	result = (int)UCHAR(*(s + 1)) - (int)UCHAR('0');
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * MakeMonth --
 *
 *      Convert a three-digit abbreviated month name into a number;
 *      e.g., Jan=0, Feb=1, etc.
 *
 * Results:
 *      An integral month number.
 *
 * Side effects:
 *      Changes passed string to camel case.
 *
 *----------------------------------------------------------------------
 */

static int
MakeMonth(char *s)
{
    int i, result = 0;

    NS_NONNULL_ASSERT(s != NULL);

    /*
     * Make sure it's capitalized like this:
     * "Jan"
     */

    *s = CHARCONV(upper, *s);
    *(s + 1) = CHARCONV(lower, *(s + 1));
    *(s + 2) = CHARCONV(lower, *(s + 2));

    for (i = 0; i < 12; i++) {
        if (strncmp(month_names[i], s, 3u) == 0) {
            result = i;
            break;
        }
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
