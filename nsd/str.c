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
 * str.c --
 *
 *      Functions that deal with strings.
 */

#include "nsd.h"


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrTrim --
 *
 *      Trim leading and trailing white space from a string.
 *
 * Results:
 *      A pointer to the trimmed string, which will be in the original
 *      string.
 *
 * Side effects:
 *      May modify passed-in string.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_StrTrim(char *chars)
{
    NS_NONNULL_ASSERT(chars != NULL);

    return Ns_StrTrimLeft(Ns_StrTrimRight(chars));
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_StrTrimLeft --
 *
 *      Trim leading white space from a string.
 *
 * Results:
 *      A pointer to the trimmed string, which will be in the
 *      original string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_StrTrimLeft(char *chars)
{
    NS_NONNULL_ASSERT(chars != NULL);

    while (CHARTYPE(space, *chars) != 0) {
        ++chars;
    }

    return chars;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_StrTrimRight --
 *
 *      Trim trailing white space from a string.
 *
 * Results:
 *      A pointer to the trimmed string, which will be in the
 *      original string.
 *
 * Side effects:
 *      The string will be modified.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_StrTrimRight(char *chars)
{
    int len;

    NS_NONNULL_ASSERT(chars != NULL);

    len = (int)strlen(chars);

    while ((--len >= 0)
           && (CHARTYPE(space, chars[len]) != 0
               || chars[len] == '\n')) {
        chars[len] = '\0';
    }
    return chars;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToLower --
 *
 *      All alphabetic characters in "chars" are changed to lowercase.
 *
 * Results:
 *      Same string as passed in.
 *
 * Side effects:
 *      Will modify string.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_StrToLower(char *chars)
{
    char *p;

    NS_NONNULL_ASSERT(chars != NULL);

    p = chars;
    while (*p != '\0') {
        if (CHARTYPE(upper, *p) != 0) {
            *p = CHARCONV(lower, *p);
        }
        ++p;
    }
    return chars;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToUpper --
 *
 *      All alphabetic Ccars in "chars" are changed to uppercase.
 *
 * Results:
 *      Same string as passed in.
 *
 * Side effects:
 *      Will modify string.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_StrToUpper(char *chars)
{
    char *s;

    NS_NONNULL_ASSERT(chars != NULL);

    s = chars;
    while (*s != '\0') {
        if (CHARTYPE(lower, *s) != 0) {
            *s = CHARCONV(upper, *s);
        }
        ++s;
    }
    return chars;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToInt --
 *
 *      Attempt to convert the string value to an integer.
 *
 * Results:
 *      NS_OK and *intPtr updated, NS_ERROR if the number cannot be
 *      parsed or overflows.
 *
 * Side effects:
 *      The string may begin with an arbitrary amount of white space (as determined by
 *      isspace(3)) followed by a  single  optional `+' or `-' sign.  If string starts with `0x' prefix,
 *      the number will be read in base 16, otherwise the number will be treated as decimal
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_StrToInt(const char *chars, int *intPtr)
{
    long          lval;
    char         *ep;
    Ns_ReturnCode status = NS_OK;

    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(intPtr != NULL);

    errno = 0;
    lval = strtol(chars, &ep, chars[0] == '0' && chars[1] == 'x' ? 16 : 10);
    if (unlikely(chars[0] == '\0' || *ep != '\0')) {
        status = NS_ERROR;
    } else {
        if (unlikely((errno == ERANGE && (lval == LONG_MAX || lval == LONG_MIN))
                     || (lval > INT_MAX || lval < INT_MIN))) {
            status = NS_ERROR;
        } else {
            *intPtr = (int) lval;
        }
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToWideInt --
 *
 *      Attempt to convert the string value to an wide integer.
 *
 *      The string may begin with an arbitrary amount of white space (as
 *      determined by isspace(3)) followed by a single optional `+' or `-'
 *      sign.  If string starts with `0x' prefix, the number will be read in
 *      base 16, otherwise the number will be treated as decimal.
 *
 * Results:
 *      NS_OK and *intPtr updated, NS_ERROR if the number cannot be
 *      parsed or overflows.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_StrToWideInt(const char *chars, Tcl_WideInt *intPtr)
{
    Tcl_WideInt   lval;
    char         *ep;
    Ns_ReturnCode status = NS_OK;

    errno = 0;
    lval = strtoll(chars, &ep, chars[0] == '0' && chars[1] == 'x' ? 16 : 10);
    if (unlikely(chars[0] == '\0' || *ep != '\0')) {
        status = NS_ERROR;
    } else {
        if (unlikely(errno == ERANGE && (lval == LLONG_MAX || lval == LLONG_MIN))) {
            status = NS_ERROR;
        } else {
            *intPtr = (Tcl_WideInt) lval;
        }
    }

    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToMemUnit --
 *
 *      Attempt to convert the string value to a memory unit value
 *      (an integer followed by kB, MB, GB, KiB, MiB, GiB).
 *
 *      The string may begin an integer followed by an arbitrary amount of
 *      white space (as determined by isspace(3)) followed by a single
 *      optional `.' and integer fraction part.  If the reminder is one of the
 *      accepted mem unit strings above, multiply the value with the
 *      corresponding multiplier.
 *
 * Results:
 *      NS_OK and *intPtr updated, NS_ERROR if the number cannot be
 *      parsed or overflows.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_StrToMemUnit(const char *chars, Tcl_WideInt *intPtr)
{
    Ns_ReturnCode status = NS_OK;

    if (chars[0] == '\0') {
        *intPtr = (Tcl_WideInt) 0;

    } else {
        Tcl_WideInt lval;
        char       *endPtr;

        /*
         * Parse the first part of the number.
         */
        errno = 0;
        lval = strtoll(chars, &endPtr, 10);
        if (unlikely(errno == ERANGE && (lval == LLONG_MAX || lval == LLONG_MIN))) {
            /*
             * strtoll() parsing failed.
             */
            status = NS_ERROR;
        } else {
            int    multiplier = 1;
            double fraction = 0.0;

            if (*endPtr != '\0') {
                /*
                 * We are not at the end of the string, check for decimal
                 * digits.
                 */
                if (*endPtr == '.') {
                    long   decimal, i, digits, divisor = 1;
                    char  *ep;

                    endPtr++;
                    decimal = strtoll(endPtr, &ep, 10);
                    digits = ep-endPtr;
                    for (i = 0; i < digits; i++) {
                        divisor *= 10;
                    }
                    fraction = (double)decimal / (double)divisor;
                    endPtr = ep;
                }
                /*
                 * Skip whitespace
                 */
                while (CHARTYPE(space, *endPtr) != 0) {
                    endPtr++;
                }
                /*
                 * Parse units.
                 *
                 * The International System of Units (SI) defines
                 *    kB, MB, GB as 1000, 1000^2, 1000^3 bytes,
                 * and IEC defines
                 *    KiB, MiB and GiB as 1024, 1024^2, 1024^3 bytes.
                 *
                 * For effective memory usage, multiple of 1024 are
                 * better. Therefore we follow the PostgreSQL conventions and
                 * use 1024 as multiplier, but we allow as well the IEC
                 * abbreviations.
                 */
                if (*endPtr == 'M' && *(endPtr+1) == 'B') {
                    multiplier = 1024 * 1024;
                } else if ((*endPtr == 'K' || *endPtr == 'k') && *(endPtr+1) == 'B') {
                    multiplier = 1024;
                } else if (*endPtr == 'G' && *(endPtr+1) == 'B') {
                    multiplier = 1024 * 1024 * 1024;

                } else if (*endPtr == 'M' && *(endPtr+1) == 'i' && *(endPtr+2) == 'B') {
                    multiplier = 1024 * 1024;
                } else if ((*endPtr == 'K') && *(endPtr+1) == 'i' && *(endPtr+2) == 'B') {
                    multiplier = 1024;
                } else if (*endPtr == 'G' && *(endPtr+1) == 'i' && *(endPtr+2) == 'B') {
                    multiplier = 1024 * 1024 * 1024;
                } else {
                    status = NS_ERROR;
                }
            }
            if (status == NS_OK) {
                /*
                 * The mem unit value was parsed correctly.
                 */
                if (fraction > 0.0) {
                    /*
                     * We have a fraction (e.g. 1.5MB). Compute the value as
                     * floating point value and covert the result to integer.
                     */
                    double r = (double)(lval * multiplier) + fraction * multiplier;

                    *intPtr = (Tcl_WideInt)r;
                } else {
                    /*
                     * No need to compute with floating point values.
                     */
                    *intPtr = (Tcl_WideInt) lval * multiplier;
                }
            }
        }
    }

    return status;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_Match --
 *
 *      Compare the beginnings of two strings, case insensitively.
 *      The comparison stops when the end of the shorter string is
 *      reached.
 *
 * Results:
 *      NULL if no match, b if match.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_Match(const char *a, const char *b)
{
    if (a != NULL && b != NULL) {
        while (*a != '\0' && *b != '\0') {
            char c1 = (CHARTYPE(lower, *a) != 0) ? *a : CHARCONV(lower, *a);
            char c2 = (CHARTYPE(lower, *b) != 0) ? *b : CHARCONV(lower, *b);
            if (c1 != c2) {
                b = NULL;
                break;
            }
            a++;
            b++;
        }
    }
    return b;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_NextWord --
 *
 *        Return a pointer to first character of the next word in a
 *        string; words are separated by white space.
 *
 * Results:
 *        A string pointer in the original string.
 *
 * Side effects:
 *        None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_NextWord(const char *line)
{
    while (*line != '\0' && CHARTYPE(space, *line) == 0) {
        ++line;
    }
    while (*line != '\0' && CHARTYPE(space, *line) != 0) {
        ++line;
    }
    return line;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrCaseFind --
 *
 *      Search for first substring within string, case insensitive.
 *
 * Results:
 *      A pointer to where substring starts or NULL.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_StrNStr(const char *chars, const char *subString)
{
    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(subString != NULL);

    return Ns_StrCaseFind(chars, subString);
}

const char *
Ns_StrCaseFind(const char *chars, const char *subString)
{
    const char *result = NULL;

    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(subString != NULL);

    if (strlen(chars) > strlen(subString)) {
        while (*chars != '\0') {
            if (Ns_Match(chars, subString) != NULL) {
                result = chars;
                break;
            }
            ++chars;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrIsHost --
 *
 *      Does the given string contain only characters permitted in a
 *      Host header? Letters, digits, single periods and the colon port
 *      separator are valid.
 *
 * Results:
 *      NS_TRUE or NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_StrIsHost(const char *chars)
{
    register const char *p;
    bool result = NS_TRUE;

    NS_NONNULL_ASSERT(chars != NULL);

    for (p = chars; *p != '\0'; p++) {
        if ((CHARTYPE(alnum, *p) == 0) && (*p != ':')
            && (*p != '[') && (*p != ']')                           /* IP-literal notation */
            && ((*p != '.') || (p[0] == '.' && p[1] == '.'))) {

            result = NS_FALSE;
            break;
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetBinaryString --
 *
 *      Helper function to return the content of a Tcl_Obj either binary (if
 *      available) or from the string representation.
 *
 * Results:
 *      Content of the Tcl_Obj.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
Ns_GetBinaryString(Tcl_Obj *obj, int *lengthPtr, Tcl_DString *dsPtr)
{
    const char *result;

    NS_NONNULL_ASSERT(obj != NULL);
    NS_NONNULL_ASSERT(lengthPtr != NULL);

    /*
     * Just reference dsPtr for the time being, we should wait, until Tcl 8.7
     * is released an then maybe get tid of dsPtr.
     */
    (void)dsPtr;

    result = (char *)Tcl_GetByteArrayFromObj(obj, lengthPtr);

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
