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
 * uuencode.c --
 *
 *      Uuencoding and decoding routines which map 8-bit binary bytes
 *      into 6-bit ascii characters.
 *
 */

#include "nsd.h"

/*
 * The following array specify the output ascii character for each
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
 * The following array maps all 256 8-bit ascii characters to
 * either the corresponding 6-bit value or -1 for invalid character.
 */

static const int pr2six[256] = {
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

#define Encode(c) (UCHAR(six2pr[(c)]))
#define Decode(c) (UCHAR(pr2six[(int)(c)]))


/*
 *----------------------------------------------------------------------
 *
 * Ns_HtuuEncode --
 *
 *      Encode a string.
 *
 * Results:
 *      Number of bytes placed in output.
 *
 * Side effects:
 *      Encoded characters are placed in output which must be
 *	large enough for the result, i.e., (1 + (len * 4) / 2)
 *	bytes, minimum outout buffer size is 4 bytes.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_HtuuEncode(const unsigned char *input, size_t bufSize, char *buf)
{
    register const unsigned char *p;
    register unsigned char *q;
    register int line = 0;
    register size_t n;

    assert(input != NULL);
    assert(buf != NULL);

    /*
     * Convert every three input bytes into four output
     * characters.
     */

    p = input;
    q = (unsigned char *) buf;
    for (n = bufSize / 3u; n > 0u; --n) {
        /*
         * Add wrapping newline to be compatible with GNU uuencode
         * if line length exceeds max line length - without adding
         * extra newline character
         */
        if (line >= 60) {
	    *q++ = UCHAR('\n'); 
	    line = 0;
        }       
	*q++ = Encode(p[0] >> 2);
	*q++ = Encode((UCHAR(p[0] << 4) & 0x30U) | ((p[1] >> 4) & 0x0FU));
        *q++ = Encode((UCHAR(p[1] << 2) & 0x3CU) | ((p[2] >> 6) & 0x03U));
	*q++ = Encode(p[2] & 0x3FU);
	p += 3;
        line += 4;
    }

    /*
     * Convert and pad any remaining bytes.
     */

    n = bufSize % 3u;
    if (n > 0u) {
	*q++ = Encode(p[0] >> 2);
	if (n == 1u) {
	    *q++ = Encode(UCHAR(p[0] << 4) & 0x30U);
	    *q++ = UCHAR('=');
	} else {
	    *q++ = Encode((UCHAR(p[0] << 4) & 0x30U) | ((p[1] >> 4) & 0x0FU));
	    *q++ = Encode(UCHAR( p[1] << 2) & 0x3CU);
	}
	*q++ = UCHAR('=');
    }
    *q = UCHAR('\0');
    return ((char *)q - buf);
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
 *	large enough for the result, i.e., (3 + (len * 3) / 4)
 *	bytes.
 *
 *----------------------------------------------------------------------
 */

size_t
Ns_HtuuDecode(const char *input, unsigned char *buf, size_t bufSize)
{
    register int n;
    unsigned char chars[4] = {0u, 0u, 0u, 0u};
    register const unsigned char *p;
    register unsigned char *q;

    assert(input != NULL);
    assert(buf != NULL);
    
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
    while (*p) {
        if (pr2six[(int)(*p)] >= 0) {
            chars[n++] = *p;
	    if (n == 4) {
		*q++ = UCHAR(Decode(chars[0]) << 2) | Decode(chars[1]) >> 4;
		*q++ = UCHAR(Decode(chars[1]) << 4) | Decode(chars[2]) >> 2;
		*q++ = UCHAR(Decode(chars[2]) << 6) | Decode(chars[3]);
		n = 0;
	    }
        }
	p++;
    }

    /*
     * Decode remaining 2 or 3 bytes.
     */

    if (n > 1) {
	*q++ = UCHAR(Decode(chars[0]) << 2) | Decode(chars[1]) >> 4;
    }
    if (n > 2) {
	*q++ = UCHAR(Decode(chars[1]) << 4) | Decode(chars[2]) >> 2;
    }
    if ((size_t)(q - buf) < bufSize) {
	*q = UCHAR('\0');
    }
    return (size_t)(q - buf);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
