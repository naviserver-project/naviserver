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
 *      Trim trailing white space from a string. Do NOT trim potential
 *      parts of UTF-8 characters such we do not damage a string like
 *      "test\xc3\x85", where \x85 is a UTF-8 whitespace; fortunately,
 *      byte 2-4 in UTF-8 follows the pattern 10xxxxxx.
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
           && (chars[len] > 0)
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
 *      The string may begin with an arbitrary amount of white space (as
 *      determined by isspace(3)) followed by a single optional `+' or `-'
 *      sign.  If string starts with `0x' prefix, the number will be read in
 *      base 16, otherwise the number will be treated as decimal
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
 *      Attempt to convert the string value to a wide integer.
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
                    long long decimal;
                    long      i;
                    ptrdiff_t digits;
                    int       divisor = 1;
                    char     *ep;

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
                 * Parse memory units.
                 *
                 * The International System of Units (SI) defines
                 *    kB, MB, GB as 1000, 1000^2, 1000^3 bytes,
                 * and IEC defines
                 *    KiB, MiB and GiB as 1024, 1024^2, 1024^3 bytes.
                 *
                 * For effective memory usage, multiple of 1024 are
                 * better. Therefore, we follow the PostgreSQL conventions and
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
 * Ns_StrIsValidHostHeaderContent --
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
Ns_StrIsValidHostHeaderContent(const char *chars)
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
const unsigned char *
Ns_GetBinaryString(Tcl_Obj *obj, bool forceBinary, int *lengthPtr, Tcl_DString *dsPtr)
{
    const unsigned char *result;

    NS_NONNULL_ASSERT(obj != NULL);
    NS_NONNULL_ASSERT(lengthPtr != NULL);

    /*
     * Obtain the binary data from an obj. If the Tcl_Obj is not a bytecode
     * obj, produce it on the fly to avoid putting the burden to the user.
     *
     * Tcl requires the user to convert UTF-8 characters into bytearrays on
     * the scripting level, as the following example illustrates. The UTF-8
     * character "ü" requires 2 bytes:
     *
     * % binary encode hex ü
     * fc
     * % binary encode hex [encoding convertto utf-8 ü]
     * c3bc
     *
     * When doing a base64 encoding, the casual user expect the same behavior
     * as in a shell, e.g. for transforming "ü" into a based64 encoded string:
     *
     * $ echo -n "ü" |base64
     * w7w=
     *
     * % ns_base64encode ü
     * w7w=
     *
     * But Tcl requires this interaction "manually":
     *
     * % binary encode base64 ü
     * /A==
     *
     * % binary encode base64 [encoding convertto utf-8 ü]
     * w7w=
     *
     * The same principle should as well apply for the crypto commands, where
     * the NaviServer user should not have to care for converting chars to
     * bytestrings manually.
     *
     * $ echo -n "ü" |openssl sha1
     * (stdin)= 94a759fd37735430753c7b6b80684306d80ea16e
     *
     * % ns_sha1 "ü"
     * 94A759FD37735430753C7B6B80684306D80EA16E
     *
     * % ns_md string -digest sha1 ü
     * 94a759fd37735430753c7b6b80684306d80ea16e
     *
     * The same should hold as well for 3-byte UTF-8 characters
     *
     * $ echo -n "☀" |openssl sha1
     * (stdin)= d5b6c20ee0b3f6dafa632a63eafe3fd0db26752d
     *
     * % ns_sha1 ☀
     * D5B6C20EE0B3F6DAFA632A63EAFE3FD0DB26752D
     *
     * % ns_md string -digest sha1 ☀
     * d5b6c20ee0b3f6dafa632a63eafe3fd0db26752d
     *
     */

    if (forceBinary || NsTclObjIsByteArray(obj)) {
        //fprintf(stderr, "NsTclObjIsByteArray\n");
        result = (unsigned char *)Tcl_GetByteArrayFromObj(obj, lengthPtr);
    } else {
        int         stringLength;
        const char *charInput;

        charInput = Tcl_GetStringFromObj(obj, &stringLength);

        //if (NsTclObjIsEncodedByteArray(obj)) {
        //    fprintf(stderr, "NsTclObjIsEncodedByteArray\n");
        //} else {
        //    //fprintf(stderr, "some other obj\n");
        //}

        Tcl_UtfToExternalDString(NS_utf8Encoding, charInput, stringLength, dsPtr);
        result = (unsigned char *)dsPtr->string;
        *lengthPtr = dsPtr->length;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_Valid_UTF8 --
 *
 *      Check the validity of the UTF-8 input string.
 *
 *      This implementation is fully platform independent and
 *      based on the code from Daniel Lemire, but not using
 *      the SIMD operations from
 *
 *         John Keiser, Daniel Lemire, Validating UTF-8 In Less Than
 *         One Instruction Per Byte, Software: Practice &
 *         Experience 51 (5), 2021
 *         https://github.com/lemire/fastvalidate-utf-8
 *
 *      If necessary, more performance can be squeezed
 *      out. This might be the case, when this function would
 *      be used internally for validating.
 *
 * Results:
 *      Boolean value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool Ns_Valid_UTF8(const unsigned char *bytes, size_t nrBytes)
{
    size_t index = 0;

    for (;;) {
        unsigned char byte1, byte2;

        /*
         * First a loop over 7-bit ASCII characters.
         *
         * In most cases, the strings are longer. Reduce the number of
         * loops by processing eight characters at a time.
         */
        if (likely(index + 8 < nrBytes)) {
            const uint64_t *p = (const uint64_t*)&bytes[index];

            if ((*p & 0x8080808080808080u) == 0u) {
                index += 8;
                continue;
            }
        } else if (unlikely(index >= nrBytes)) {
            /*
             * Successful end of string.
             */
            return NS_TRUE;
        }

        /*Ns_Log(Notice, "[%ld] work on %.2x %c", index, bytes[index], bytes[index]);*/
        byte1 = bytes[index++];
        if (byte1 < 0x80) {
            continue;

        } else if (byte1 < 0xE0) {
            /*
             * Two-byte UTF-8.
             */
            if (index == nrBytes) {
                /*
                 * Premature end of string.
                 */
                Ns_Log(Debug, "UTF8 decode '%s': 2byte premature", bytes);
                return NS_FALSE;
            }
            byte2 = bytes[index++];
            if (byte1 < 0xC2 || ((/*bytes[index++]*/ byte2 & 0xC0u) != 0x80u)) {
                Ns_Log(Debug, "UTF8 decode '%s': 2-byte invalid 2nd byte %.2x", bytes, byte2);
                return NS_FALSE;
            }
        } else if (byte1 < 0xF0) {
            /*
             * Three-byte UTF-8.
             */
            if (index + 1 >= nrBytes) {
                /*
                 * Premature end of string.
                 */
                Ns_Log(Debug, "UTF8 decode '%s': 3-byte premature", bytes);
                return NS_FALSE;
            }
            byte2 = bytes[index++];
            if (byte2 > 0xBF
                /* Overlong? 5 most significant bits must not all be zero. */
                || (byte1 == 0xE0 && byte2 < 0xA0)
                /* Check for illegal surrogate codepoints. */
                || (byte1 == 0xED && 0xA0 <= byte2)
                /* Third byte trailing-byte test. */
                || bytes[index++] > 0xBF) {
                return NS_FALSE;
            }
        } else {
            /*
             * Four-byte UTF-8.
             */
            if (index + 2 >= nrBytes) {
                /*
                 * Premature end of string.
                 */
                Ns_Log(Debug, "UTF8 decode '%s': 3-byte premature", bytes);
                return NS_FALSE;
            }
            byte2 = bytes[index++];
            if (byte2 > 0xBF
                /* Check that 1 <= plane <= 16. Tricky optimized form of:
                 * if (byte1 > (byte) 0xF4
                 *     || byte1 == (unsigned char) 0xF0 && byte2 < (unsigned char) 0x90
                 *     || byte1 == (unsigned char) 0xF4 && byte2 > (unsigned char) 0x8F)
                 */
                || (((unsigned)(byte1 << 28) + (byte2 - 0x90u)) >> 30) != 0
                /* Third byte trailing byte test */
                || bytes[index++] > 0xBF
                /*  Fourth byte trailing byte test */
                || bytes[index++] > 0xBF) {
                return NS_FALSE;
            }
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_Is7-bit --
 *
 *      Checks whether the input string is 7-bit. This functions tries to
 *      perform this test with a low number of iterations for
 *      performance reasons.
 *
 * Results:
 *      Boolean value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool Ns_Is7bit(const char *bytes, size_t nrBytes)
{
    const char *current = bytes, *end = bytes + nrBytes;
    uint64_t mask1 = 0u, mask2 = 0u, mask3 = 0u, mask4 = 0u, last_mask = 0u;

    /*
     * An unsigned 64-bit integral type is not guaranteed by the C
     * standard but is typically available on 32-bit machines, and on
     * virtually all machines running Linux. ... and since we use this
     * as well on other places, this should be ok.
     */
    for (; current < end - 32; current += 32) {
        const uint64_t* p = (const uint64_t*)current;
        mask1 |= p[0];
        mask2 |= p[1];
        mask3 |= p[2];
        mask4 |= p[3];
    }

    for (; current < end - 8; current += 8) {
        const uint64_t* p = (const uint64_t*)current;
        mask1 |= p[0];
    }

    for (; current < end; current++) {
        last_mask |= *(const uint8_t*)current;
    }
    return ((mask1 | mask2 | mask3 | mask4 | last_mask) & 0x8080808080808080u) == 0u;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
