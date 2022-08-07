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
 * uuencode.c --
 *
 *      Uuencoding and decoding routines which map 8-bit binary bytes
 *      into 6-bit ASCII characters.
 *
 */

#include "nsd.h"

/*
 * The following array specify the output ASCII character for each
 * of the 64 6-bit characters.
 */

static const char six2pr[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'
};

/*
 * The following array maps all 256 8-bit ASCII characters to
 * either the corresponding 6-bit value or -1 for invalid character.
 */

static const char pr2six[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

#define Encode(table, c) (UCHAR((table)[(c)]))
#define Decode(table, c) (UCHAR((table)[(int)(c)]))


/*
 * Tables for base64url encoding
 * return [string map {+ - / _ = {} \n {}} [ns_base64encode $string]]
 */
static const char six2pr_url[64] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_'
};

static const char pr2six_url[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};



/*
 *----------------------------------------------------------------------
 *
 * Ns_Base64Encode --
 *
 *      Encode a string with either base64 encoding or base64url encoding
 *      (when "encoding" is set to 1). When maxLineLenth is larger than 0,
 *      lines longer this value are wrapped by inserting a newline character.
 *
 * Results:
 *      Number of bytes placed in output buffer.
 *
 * Side effects:
 *      Encoded characters are placed into the output buffer which must be
 *      large enough for the result, i.e., (1 + (len * 4) / 3) bytes; the
 *      minimum output buffer size is 4 bytes.
 *
 *----------------------------------------------------------------------
 */
size_t
Ns_Base64Encode(const unsigned char *input, size_t inputSize, char *buf, size_t maxLineLength, int encoding)
{
    register const unsigned char *p;
    register unsigned char       *q;
    register size_t               lineLength = 0u, n;
    static const char            *encode_table;

    NS_NONNULL_ASSERT(input != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    if (encoding == 0) {
        encode_table = six2pr;
    } else {
        encode_table = six2pr_url;
    }

    /*
     * Convert every three input bytes into four output
     * characters.
     */

    p = input;
    q = (unsigned char *) buf;
    for (n = inputSize / 3u; n > 0u; --n) {
        /*
         * Add wrapping newline when line is longer than maxLineLength.
         */
        if (maxLineLength > 0 && lineLength >= maxLineLength) {
            *q++ = UCHAR('\n');
            lineLength = 0u;
        }
        *q++ = Encode(encode_table, p[0] >> 2);
        *q++ = Encode(encode_table, (UCHAR(p[0] << 4) & 0x30u) | ((p[1] >> 4) & 0x0Fu));
        *q++ = Encode(encode_table, (UCHAR(p[1] << 2) & 0x3CU) | ((p[2] >> 6) & 0x03u));
        *q++ = Encode(encode_table, p[2] & 0x3Fu);
        p += 3;
        lineLength += 4u;
    }

    /*
     * Convert and pad any remaining bytes.
     */

    n = inputSize % 3u;
    if (n > 0u) {
        *q++ = Encode(encode_table, p[0] >> 2);
        if (n == 1u) {
            *q++ = Encode(encode_table, UCHAR(p[0] << 4) & 0x30u);
            if (encoding == 0) {
                *q++ = UCHAR('=');
            }
        } else {
            *q++ = Encode(encode_table, (UCHAR(p[0] << 4) & 0x30u) | ((p[1] >> 4) & 0x0Fu));
            *q++ = Encode(encode_table, UCHAR( p[1] << 2) & 0x3CU);
        }
        if (encoding == 0) {
            *q++ = UCHAR('=');
        }
    }
    *q = UCHAR('\0');
    return (size_t)((char *)q - buf);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_HtuuEncode2, Ns_HtuuEncode --
 *
 *      Backward compatible functions for Ns_Base64Encode.
 *
 * Results:
 *      Number of bytes placed in output buffer.
 *
 * Side effects:
 *      Updated output buffer.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_HtuuEncode2(const unsigned char *input, size_t inputSize, char *buf, int encoding)
{
    size_t result;

    NS_NONNULL_ASSERT(input != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    if (encoding == 0) {
        /*
         * Add wrapping newline to be compatible with GNU uuencode
         * if line length exceeds max line length - without adding
         * extra newline character
         */
        result = Ns_Base64Encode(input, inputSize, buf, 60, encoding);
    } else {
        result = Ns_Base64Encode(input, inputSize, buf, 0u, encoding);
    }
    return result;
}


size_t
Ns_HtuuEncode(const unsigned char *input, size_t inputSize, char *buf)
{
    return Ns_Base64Encode(input, inputSize, buf, 60, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_HtuuDecode --
 *
 *      Decode a string.
 *
 * Results:
 *      Number of binary bytes decoded.
 *
 * Side effects:
 *      Decoded characters are placed in output which must be
 *    large enough for the result, i.e., (3 + (len * 3) / 4)
 *    bytes.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_HtuuDecode2(const char *input, unsigned char *buf, size_t bufSize, int encoding)
{
    register int                  n;
    unsigned char                 chars[4] = {0u, 0u, 0u, 0u};
    register const unsigned char *p;
    register unsigned char       *q;
    static const char            *decode_table;


    NS_NONNULL_ASSERT(input != NULL);
    NS_NONNULL_ASSERT(buf != NULL);

    if (encoding == 0) {
        decode_table = pr2six;
    } else {
        decode_table = pr2six_url;
    }


    /*
     * Skip leading space, if any.
     */

    while (*input == ' ' || *input == '\t') {
        ++input;
    }

    /*
     * Decode every four input bytes.
     */

    n = 0;
    p = (const unsigned char *) input;
    q = buf;
    while (*p != 0u) {
        if (decode_table[(int)(*p)] >= 0) {
            chars[n++] = *p;
            if (n == 4) {
                *q++ = UCHAR(Decode(decode_table, chars[0]) << 2) | Decode(decode_table, chars[1]) >> 4;
                *q++ = UCHAR(Decode(decode_table, chars[1]) << 4) | Decode(decode_table, chars[2]) >> 2;
                *q++ = UCHAR(Decode(decode_table, chars[2]) << 6) | Decode(decode_table, chars[3]);
                n = 0;
            }
        }
        p++;
    }

    /*
     * Decode remaining 2 or 3 bytes.
     */

    if (n > 1) {
        *q++ = UCHAR(Decode(decode_table, chars[0]) << 2) | Decode(decode_table, chars[1]) >> 4;
    }
    if (n > 2) {
        *q++ = UCHAR(Decode(decode_table, chars[1]) << 4) | Decode(decode_table, chars[2]) >> 2;
    }
    if ((size_t)(q - buf) < bufSize) {
        *q = UCHAR('\0');
    }
    return (size_t)(q - buf);
}

size_t
Ns_HtuuDecode(const char *input, unsigned char *buf, size_t bufSize)
{
    return Ns_HtuuDecode2(input, buf, bufSize, 0);
}
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
