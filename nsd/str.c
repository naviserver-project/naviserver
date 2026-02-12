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
 * str.c --
 *
 *      Functions that deal with strings.
 */

#include "nsd.h"

#ifdef HAVE_OPENSSL_EVP_H
# include "nsopenssl.h"
# include <openssl/ssl.h>
# include <openssl/err.h>
#endif

static void
InvalidUtf8ErrorMessage(Tcl_DString *dsPtr, const unsigned char *bytes, size_t nrBytes,
                 size_t index, TCL_SIZE_T nrMaxBytes, bool isTruncated);


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
 * Ns_StrToULongStrict --
 *
 *      Convert a NUL-terminated string to an unsigned long using strtoul()
 *      with strict validation. The conversion is performed with the specified
 *      base and requires that at least one digit is consumed and that the
 *      entire input string is consumed (i.e., no trailing characters).
 *
 *      This function uses errno to detect range errors. Since the C library
 *      does not guarantee that errno is cleared on successful conversion,
 *      errno is set to 0 prior to calling strtoul() and ERANGE is checked
 *      afterwards.
 *
 * Results:
 *      NS_TRUE on successful conversion with *valuePtr set.
 *      NS_FALSE if no digits were found, trailing characters were present,
 *      or the value overflowed/underflowed the representable range.
 *
 * Side Effects:
 *      May set errno (via strtoul()).
 *
 *----------------------------------------------------------------------
 */
bool
Ns_StrToULongNStrict(const char *s, size_t len, int base, unsigned long *valuePtr)
{
    char buf[32]; /* enough for entity numbers; adjust as needed */
    char *end;
    unsigned long v;

    NS_NONNULL_ASSERT(s != NULL);
    NS_NONNULL_ASSERT(valuePtr != NULL);

    if (len == 0 || len >= sizeof(buf)) {
        return NS_FALSE;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';

    errno = 0;
    v = strtoul(buf, &end, base);

    if (end == buf
        || *end != '\0'
        || errno == ERANGE) {
        return NS_FALSE;
    } else {
        *valuePtr = v;
    }
    return NS_TRUE;
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
                     * floating point value and convert the result to integer.
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

#ifdef NS_WITH_DEPRECATED
const char *
Ns_StrNStr(const char *chars, const char *subString)
{
    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(subString != NULL);

    return Ns_StrCaseFind(chars, subString);
}
#endif


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
Ns_GetBinaryString(Tcl_Obj *obj, bool forceBinary, TCL_SIZE_T *lengthPtr, Tcl_DString *dsPtr)
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
    Ns_Log(Debug, "Ns_GetBinaryString is byte-array: %d", NsTclObjIsByteArray(obj));
    if (forceBinary || NsTclObjIsByteArray(obj)) {
        result = (unsigned char *)Tcl_GetByteArrayFromObj(obj, lengthPtr);
    } else {
        TCL_SIZE_T  stringLength;
        const char *charInput;

        charInput = Tcl_GetStringFromObj(obj, &stringLength);

        //if (NsTclObjIsEncodedByteArray(obj)) {
        //    fprintf(stderr, "NsTclObjIsEncodedByteArray\n");
        //} else {
        //    //fprintf(stderr, "some other obj\n");
        //}

        (void)Tcl_UtfToExternalDString(NS_utf8Encoding, charInput, stringLength, dsPtr);
        result = (unsigned char *)dsPtr->string;
        *lengthPtr = dsPtr->length;
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * InvalidUtf8ErrorMessage --
 *
 *      Helper function for Ns_Valid_UTF8 to return a shoreted error string,
 *      when dsPtr is not NULL. The function initializes the string and
 *      expects the caller to free it.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
`*/
static void
InvalidUtf8ErrorMessage(Tcl_DString *dsPtr, const unsigned char *bytes, size_t nrBytes,
                 size_t index, TCL_SIZE_T nrMaxBytes, bool isTruncated)
{
    if (dsPtr != NULL) {
        long prefixLen = MIN(10, (long)index);

        Tcl_DStringInit(dsPtr);
        if ((long)index > prefixLen) {
            Tcl_DStringAppend(dsPtr, (const char *)bytes, (TCL_SIZE_T)prefixLen);
            Tcl_DStringAppend(dsPtr, "...", 3);
        } else {
            Tcl_DStringAppend(dsPtr, (const char *)bytes, (TCL_SIZE_T)index-1);
        }
        Tcl_DStringAppend(dsPtr, "|", 1);
        Tcl_DStringAppend(dsPtr, (const char *)(bytes+index-1), MIN(nrMaxBytes, (TCL_SIZE_T)(nrBytes-(index-1))));
        Tcl_DStringAppend(dsPtr, "|", 1);
        if (!isTruncated) {
            Tcl_DStringAppend(dsPtr, "...", 3);
        }
        /*Ns_Log(Notice, ".... '%s'", dsPtr->string);*/
    }
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
 *      When a dsPtr is provided, and a validation error occurs, it will ne
 *      initialized and filled with a truncated error string.
 *
 * Results:
 *      Boolean value.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool Ns_Valid_UTF8(const unsigned char *bytes, size_t nrBytes, Tcl_DString *dsPtr)
{
    size_t idx = 0;

    /*NsHexPrint("Valid UTF8?", bytes, (size_t)nrBytes, 32, NS_FALSE);*/

    for (;;) {
        unsigned char byte1, byte2, byte3;

        /*
         * First a loop over 7-bit ASCII characters.
         *
         * In most cases, the strings are longer. Reduce the number of
         * loops by processing eight characters at a time.
         */
        if (likely(nrBytes - idx >= 8)) {
            uint64_t w;
            memcpy(&w, bytes + idx, sizeof w);

            if ((w & 0x8080808080808080u) == 0u) {
                idx += 8;
                continue;
            }
        } else if (unlikely(idx >= nrBytes)) {
            /*
             * Successful end of string.
             */
            return NS_TRUE;
        }

        /*Ns_Log(Notice, "[%ld] work on %.2x %c", idx, bytes[idx], bytes[idx]);*/
        byte1 = bytes[idx++];
        if (byte1 < 0x80) {
            continue;

        } else if (byte1 < 0xE0) {
            /*
             * Two-byte UTF-8.
             */
            if (idx == nrBytes) {
                /*
                 * Premature end of string.
                 */
                Ns_Log(Debug, "UTF8 decode '%s': 2byte premature", bytes);
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx, 2, NS_TRUE);
                return NS_FALSE;
            }
            byte2 = bytes[idx++];
            if (byte1 < 0xC2 || ((/*bytes[idx++]*/ byte2 & 0xC0u) != 0x80u)) {
                Ns_Log(Debug, "UTF8 decode '%s': 2-byte invalid 2nd byte %.2x", bytes, byte2);
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx-1, 2, NS_FALSE);
                return NS_FALSE;
            }
        } else if (byte1 < 0xF0) {
            /*
             * Three-byte UTF-8.
             */
            if (idx + 1 >= nrBytes) {
                /*
                 * Premature end of string.
                 */
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx, 3, NS_TRUE);
                Ns_Log(Debug, "UTF8 decode '%s': 3-byte premature", bytes);
                return NS_FALSE;
            }
            byte2 = bytes[idx++];
            /* second byte must be a continuation byte */
            if ((byte2 & 0xC0u) != 0x80u
                || (byte1 == 0xE0 && byte2 < 0xA0)   /* overlong */
                || (byte1 == 0xED && byte2 >= 0xA0)) /* surrogate */
                {
                    InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx - 1, 3, NS_FALSE);
                    Ns_Log(Debug, "UTF8 decode '%s': 3-byte 2nd byte must be continuation byte", bytes);
                    return NS_FALSE;
                }

            /* third byte must be a continuation byte */
            byte3 = bytes[idx++];
            if ((byte3 & 0xC0u) != 0x80u) {
                Ns_Log(Debug, "UTF8 decode '%s': 3-byte invalid 3rd byte %.2x %.2x %.2x",
                       bytes, byte1, byte2, byte3);
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx - 2, 3, NS_FALSE);
                return NS_FALSE;
            }

        } else {
            /*
             * Four-byte UTF-8.
             */
            unsigned char byte4;
            size_t        startIndex;

            if (idx + 2 >= nrBytes) {
                /*
                 * Premature end of string.
                 */
                Ns_Log(Debug, "UTF8 decode '%s': 4-byte premature", bytes);
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx, 4, NS_TRUE);
                return NS_FALSE;
            }
            startIndex = idx;
            byte2 = bytes[idx++];
            /* byte2 must be continuation, plus range constraints for planes 1..16 */
            if ((byte2 & 0xC0u) != 0x80u
                || (((unsigned)(byte1 << 28) + (byte2 - 0x90u)) >> 30) != 0) {
                Ns_Log(Debug, "UTF8 decode '%s': 4-byte 2nd byte must be continuation byte + range", bytes);
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, startIndex, 4, NS_FALSE);
                return NS_FALSE;
            }

            byte3 = bytes[idx++];
            if ((byte3 & 0xC0u) != 0x80u) {
                Ns_Log(Debug, "UTF8 decode '%s': 4-byte 3rd byte must be continuation byte", bytes);
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx - 3, 4, NS_FALSE);
                return NS_FALSE;
            }

            byte4 = bytes[idx++];
            if ((byte4 & 0xC0u) != 0x80u) {
                Ns_Log(Debug, "UTF8 decode '%s': 4-byte 4th byte must be continuation byte", bytes);
                InvalidUtf8ErrorMessage(dsPtr, bytes, nrBytes, idx - 4, 4, NS_FALSE);
                return NS_FALSE;
            }
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_Utf8FromCodePoint --
 *
 *      Encode the provided Unicode code point as UTF-8 and store the resulting
 *      byte sequence in the buffer pointed to by dst.  The function encodes
 *      Unicode scalar values in the range U+0000..U+10FFFF and rejects values
 *      in the surrogate range U+D800..U+DFFF.
 *
 * Results:
 *      Number of UTF-8 bytes written to dst (1 to 4) on success.
 *      0 when the code point is not a valid Unicode scalar value.
 *
 * Side Effects:
 *      Writes up to 4 bytes to dst on success.
 *
 *----------------------------------------------------------------------
 */
size_t
Ns_Utf8FromCodePoint(uint32_t cp, char *dst)
{
    size_t length = 0u;

    NS_NONNULL_ASSERT(dst != NULL);

    if(cp <= 0x7F) {
        *dst = (char)cp;
        length = 1;

    } else if (cp <= 0x7FF) {
        *dst++ = (char)(((cp >> 6) & 0x1F) | 0xC0);
        *dst++ = (char)(((cp >> 0) & 0x3F) | 0x80);
        length = 2;

    } else if (cp <= 0xFFFF) {
        *dst++ = (char) (((cp >> 12) & 0x0F) | 0xE0);
        *dst++ = (char) (((cp >>  6) & 0x3F) | 0x80);
        *dst++ = (char) (((cp >>  0) & 0x3F) | 0x80);
        length = 3;

    } else if (cp <= 0x10FFFF) {
        *dst++ = (char) (((cp >> 18) & 0x07) | 0xF0);
        *dst++ = (char) (((cp >> 12) & 0x3F) | 0x80);
        *dst++ = (char) (((cp >>  6) & 0x3F) | 0x80);
        *dst++ = (char) (((cp >>  0) & 0x3F) | 0x80);
        length = 4;
    } else {
        length = 0;
    }
    return length;
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
    for (; current + 32 <= end; current += 32) {
        uint64_t p[4];

        memcpy(p, current, sizeof(p));
        mask1 |= p[0];
        mask2 |= p[1];
        mask3 |= p[2];
        mask4 |= p[3];
    }

    for (; current + 8 <= end; current += 8) {
        uint64_t p;
        memcpy(&p, current, sizeof(p));
        mask1 |= p;
    }

    for (; current < end; current++) {
        last_mask |= *(const uint8_t*)current;
    }
    return ((mask1 | mask2 | mask3 | mask4 | last_mask) & 0x8080808080808080u) == 0u;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_UpperCharPos --
 *
 *      This function searches the given byte array for the first uppercase
 *      character. It iterates over the first 'nrBytes' characters of the array,
 *      and returns the index of the first character that is recognized as uppercase
 *      by the CHARTYPE macro.
 *
 * Results:
 *      Returns the zero-based index of the first uppercase character found in the
 *      array. If no uppercase character is encountered within the specified range,
 *      the function returns -1.
 *
 * Side Effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

ssize_t Ns_UpperCharPos(const char *bytes, size_t nrBytes)
{
    size_t  i;
    ssize_t result = -1;

    NS_NONNULL_ASSERT(bytes != NULL);

    for (i = 0; i < nrBytes; i++) {
        if (CHARTYPE(upper, bytes[i]) != 0) {
            result = (ssize_t)i;
            break;
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsHexPrint --
 *
 *      Debugging function for internal use. Print the potentially binary
 *      content of a buffer in human-readable form.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Output to stderr.
 *
 *----------------------------------------------------------------------
 */
void NsHexPrint(const char *msg, const unsigned char *octets, size_t octetLength,
                unsigned int perLine, bool withChar)
{
    size_t i;

    fprintf(stderr, "%s octetLength %" PRIuz ":\n", msg, octetLength);
    for (i = 0; i < octetLength; i++) {
        if (withChar) {
            fprintf(stderr, "%c %.2x ",
                    iscntrl(octets[i] & 0xff) ? 46 : octets[i] & 0xff,
                    octets[i] & 0xff);
        } else {
            fprintf(stderr, "%.2x ", octets[i] & 0xff);
        }
        if (((i + 1) % perLine) == 0) {
            fprintf(stderr, "\n");
        }
    }
    if (octetLength % perLine != 0) {
        fprintf(stderr, "\n");
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_ReturnCodeString, Ns_TclReturnCodeString, Ns_FilterTypeString --
 *
 *      Debugging functions, map internal codes to human readable strings.
 *
 * Results:
 *      String.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *Ns_ReturnCodeString(Ns_ReturnCode code)
{
    const char  *result;

    switch (code) {
    case NS_OK:            result = "NS_OK"; break;
    case NS_ERROR:         result = "NS_ERROR"; break;
    case NS_TIMEOUT:       result = "NS_TIMEOUT"; break;
    case NS_UNAUTHORIZED:  result = "NS_UNAUTHORIZED"; break;
    case NS_FORBIDDEN:     result = "NS_FORBIDDEN"; break;
    case NS_FILTER_BREAK:  result = "NS_FILTER_BREAK"; break;
    case NS_FILTER_RETURN: result = "NS_FILTER_RETURN"; break;
    default: result = "Unknown NaviServer Result Code";
    }

    return result;
}

const char *Ns_TclReturnCodeString(int code)
{
    const char *result;

    switch (code) {
    case TCL_OK:       result = "TCL_OK"; break;
    case TCL_ERROR:    result = "TCL_ERROR"; break;
    case TCL_RETURN:   result = "TCL_RETURN"; break;
    case TCL_BREAK:    result = "TCL_BREAK"; break;
    case TCL_CONTINUE: result = "TCL_CONTINUE"; break;
    default: result = "Unknown Tcl Result Code";
    }
    return result;
}

const char *Ns_FilterTypeString(Ns_FilterType when)
{
    const char *result;

    switch (when) {
    case NS_FILTER_PRE_AUTH:   result = "preauth"; break;
    case NS_FILTER_POST_AUTH:  result = "postauth"; break;
    case NS_FILTER_TRACE:      result = "trace"; break;
    case NS_FILTER_VOID_TRACE: result = "void"; break;
    default: result = "Unknown Filter Type";
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsSockErrorCodeString --
 *
 *      Return a human readable string from the generalized
 *      "unsigned long" error code. This code is capable to
 *      carry the OpenSSL error codes as well as the classical
 *      POSIX codes. The OpenSSL function ERR_GET_LIB() decides
 *      how to get the "reason" code.
 *
 * Results:
 *      String.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const char *
NsSockErrorCodeString(unsigned long errorCode, char *buffer, size_t bufferSize)
{
    const char *result = NULL;

#ifdef HAVE_OPENSSL_EVP_H
    if (ERR_GET_LIB(errorCode) == ERR_LIB_SYS) {
        result = ns_sockstrerror(ERR_GET_REASON(errorCode));
    } else if (ERR_GET_LIB(errorCode) != 0) {
        ERR_error_string_n(errorCode, buffer, bufferSize);
        result = buffer;
    }
#endif
    if (result == NULL) {
        result = ns_sockstrerror((int)errorCode);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsStringObj --
 *
 *      Create a Tcl string object from a NUL-terminated C string. When
 *      the provided pointer is NULL, this function returns the shared
 *      empty-string atom object instead of allocating a new Tcl_Obj.
 *
 *      Callers must treat the returned object as a normal Tcl object:
 *      if the object is to be retained, the caller has to increment its
 *      reference count. Note that the shared empty-string atom object is
 *      owned by the global atom registry.
 *
 * Results:
 *      A Tcl_Obj pointer containing the specified string, or the shared
 *      empty-string atom object when chars is NULL.
 *
 * Side effects:
 *      May allocate a new Tcl_Obj when chars is non-NULL.
 *
 *----------------------------------------------------------------------
 */
Tcl_Obj *
NsStringObj(const char* chars) {
    Tcl_Obj *resultObj;

    if (chars != NULL) {
        resultObj = Tcl_NewStringObj(chars, TCL_INDEX_NONE);
    } else {
        resultObj = NsAtomObj(NS_ATOM_EMPTY);
    }
    return resultObj;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
