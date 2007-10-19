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

NS_RCSID("@(#) $Header$");


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
Ns_StrTrim(char *string)
{
    return Ns_StrTrimLeft(Ns_StrTrimRight(string));
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
Ns_StrTrimLeft(char *string)
{
    if (string != NULL) {
        while (isspace(UCHAR(*string))) {
            ++string;
        }
    }
    return string;
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
Ns_StrTrimRight(char *string)
{
    int len;

    if (string != NULL) {
        len = strlen(string);
        while ((--len >= 0)
               && (isspace(UCHAR(string[len]))
                   || string[len] == '\n')) {
            string[len] = '\0';
        }
    }
    return string;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToLower --
 *
 *      All alph. chars in a string will be made to be lowercase.
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
Ns_StrToLower(char *string)
{
    char *s;

    s = string;
    while (*s != '\0') {
        if (isupper(UCHAR(*s))) {
            *s = tolower(UCHAR(*s));
        }
        ++s;
    }
    return string;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToUpper --
 *
 *      All alph. chars in a string will be made to be uppercase.
 *
 * Results:
 *      Same string as pssed in.
 *
 * Side effects:
 *      Will modify string.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_StrToUpper(char *string)
{
    char *s;

    s = string;
    while (*s != '\0') {
        if (islower(UCHAR(*s))) {
            *s = toupper(UCHAR(*s));
        }
        ++s;
    }
    return string;
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

int
Ns_StrToInt(CONST char *string, int *intPtr)
{
    long  lval;
    char *ep;

    errno = 0;
    lval = strtol(string, &ep, string[0] == '0' && string[1] == 'x' ? 16 : 10);
    if (string[0] == '\0' || *ep != '\0') {
        return NS_ERROR;
    }
    if ((errno == ERANGE && (lval == LONG_MAX || lval == LONG_MIN))
         || (lval > INT_MAX || lval < INT_MIN)) {
        return NS_ERROR;
    }
    *intPtr = (int) lval;

    return NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_StrToWideInt --
 *
 *      Attempt to convert the string value to an wide integer.
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

Tcl_WideInt
Ns_StrToWideInt(CONST char *string, Tcl_WideInt *intPtr)
{
    Tcl_WideInt  lval;
    char *ep;

    errno = 0;
    lval = strtoll(string, &ep, string[0] == '0' && string[1] == 'x' ? 16 : 10);
    if (string[0] == '\0' || *ep != '\0') {
        return NS_ERROR;
    }
    if ((errno == ERANGE && (lval == LLONG_MAX || lval == LLONG_MIN))) {
        return NS_ERROR;
    }
    *intPtr = (int) lval;

    return NS_OK;
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

CONST char *
Ns_Match(CONST char *a, CONST char *b)
{
    char c1, c2;

    if (a != NULL && b != NULL) {
        while (*a != '\0' && *b != '\0') {
            c1 = islower(UCHAR(*a)) ? *a : tolower(UCHAR(*a));
            c2 = islower(UCHAR(*b)) ? *b : tolower(UCHAR(*b));
            if (c1 != c2) {
                return NULL;
            }
            a++;
            b++;
        }
    }
    return (char *) b;
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

CONST char *
Ns_NextWord(CONST char *line)
{
    while (*line != '\0' && !isspace(UCHAR(*line))) {
        ++line;
    }
    while (*line != '\0' && isspace(UCHAR(*line))) {
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

CONST char *
Ns_StrNStr(CONST char *string, CONST char *substring)
{
    return Ns_StrCaseFind(string, substring);
}

CONST char *
Ns_StrCaseFind(CONST char *string, CONST char *substring)
{
    if (strlen(string) > strlen(substring)) {
        while (*string != '\0') {
            if (Ns_Match(string, substring)) {
                return string;
            }
            ++string;
        }
    }
    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StrIsHost --
 *
 *      Does the given string contain only characters permitted in a
 *      Host header? Letters, digits, single periods and the colon port
 *      seperator are valid.
 *
 * Results:
 *      NS_TRUE or NS_FALSE.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_StrIsHost(CONST char *string)
{
    register CONST char *p;

    for (p = string; *p != '\0'; p++) {
        if (!isalnum(UCHAR(*p)) && *p != ':'
            && (*p != '.' || (p[0] == '.' && p[1] == '.'))) {

            return NS_FALSE;
        }
    }

    return NS_TRUE;
}
