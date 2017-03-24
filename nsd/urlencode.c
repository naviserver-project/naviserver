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
 * urlencode.c --
 *
 *      Encode and decode URLs, as described in RFC 1738.
 */

#include "nsd.h"

/*
 * The following structure defines the encoding attributes
 * of a byte.
 */

typedef struct ByteKey {
    int   hex;	       /* Valid hex value or -1. */
    int   len;	       /* Length required to encode string. */
    const char *str;   /* String for multibyte encoded character. */
} ByteKey;

/*
 * Local functions defined in this file.
 */

static char *UrlEncode(Ns_DString *dsPtr, const char *urlSegment,
                       Tcl_Encoding encoding, char part, bool upperCase)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static char *UrlDecode(Ns_DString *dsPtr, const char *urlSegment,
                       Tcl_Encoding encoding, char part)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

/*
 * Local variables defined in this file.
 */

#ifdef RFC1738

/*
 * The following table is used for encoding and decoding the
 * segments of a URI query component according to RFC 1739.
 *
 * All ASCII control characters (00-1f and 7f) and the URI
 * 'delim' and 'unwise' characters are encoded.  In addition, the
 * following URI query component reserved characters are also
 * encoded:
 *
 *      $  &  +  ,  /  :  ;  =  ?  @
 *
 * The ASCII space character receives special treatment and is
 * encoded as +.  This is handled by the encoding/decoding
 * routines and is not represented in the table below.
 *
 */

static const ByteKey queryenc[] = {
    {-1, 3, "00"}, {-1, 3, "01"}, {-1, 3, "02"}, {-1, 3, "03"}, 
    {-1, 3, "04"}, {-1, 3, "05"}, {-1, 3, "06"}, {-1, 3, "07"}, 
    {-1, 3, "08"}, {-1, 3, "09"}, {-1, 3, "0a"}, {-1, 3, "0b"}, 
    {-1, 3, "0c"}, {-1, 3, "0d"}, {-1, 3, "0e"}, {-1, 3, "0f"}, 
    {-1, 3, "10"}, {-1, 3, "11"}, {-1, 3, "12"}, {-1, 3, "13"}, 
    {-1, 3, "14"}, {-1, 3, "15"}, {-1, 3, "16"}, {-1, 3, "17"}, 
    {-1, 3, "18"}, {-1, 3, "19"}, {-1, 3, "1a"}, {-1, 3, "1b"}, 
    {-1, 3, "1c"}, {-1, 3, "1d"}, {-1, 3, "1e"}, {-1, 3, "1f"}, 
    {-1, 1, "20"}, {-1, 1, NULL}, {-1, 3, "22"}, {-1, 3, "23"}, 
    {-1, 3, "24"}, {-1, 3, "25"}, {-1, 3, "26"}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "2b"}, 
    {-1, 3, "2c"}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "2f"}, 
    { 0, 1, NULL}, { 1, 1, NULL}, { 2, 1, NULL}, { 3, 1, NULL}, 
    { 4, 1, NULL}, { 5, 1, NULL}, { 6, 1, NULL}, { 7, 1, NULL}, 
    { 8, 1, NULL}, { 9, 1, NULL}, {-1, 3, "3a"}, {-1, 3, "3b"}, 
    {-1, 3, "3c"}, {-1, 3, "3d"}, {-1, 3, "3e"}, {-1, 3, "3f"}, 
    {-1, 3, "40"}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "5b"}, 
    {-1, 3, "5c"}, {-1, 3, "5d"}, {-1, 3, "5e"}, {-1, 1, NULL}, 
    {-1, 3, "60"}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "7b"}, 
    {-1, 3, "7c"}, {-1, 3, "7d"}, {-1, 1, NULL}, {-1, 3, "7f"}, 
    {-1, 3, "80"}, {-1, 3, "81"}, {-1, 3, "82"}, {-1, 3, "83"}, 
    {-1, 3, "84"}, {-1, 3, "85"}, {-1, 3, "86"}, {-1, 3, "87"}, 
    {-1, 3, "88"}, {-1, 3, "89"}, {-1, 3, "8a"}, {-1, 3, "8b"}, 
    {-1, 3, "8c"}, {-1, 3, "8d"}, {-1, 3, "8e"}, {-1, 3, "8f"}, 
    {-1, 3, "90"}, {-1, 3, "91"}, {-1, 3, "92"}, {-1, 3, "93"}, 
    {-1, 3, "94"}, {-1, 3, "95"}, {-1, 3, "96"}, {-1, 3, "97"}, 
    {-1, 3, "98"}, {-1, 3, "99"}, {-1, 3, "9a"}, {-1, 3, "9b"}, 
    {-1, 3, "9c"}, {-1, 3, "9d"}, {-1, 3, "9e"}, {-1, 3, "9f"}, 
    {-1, 3, "a0"}, {-1, 3, "a1"}, {-1, 3, "a2"}, {-1, 3, "a3"}, 
    {-1, 3, "a4"}, {-1, 3, "a5"}, {-1, 3, "a6"}, {-1, 3, "a7"}, 
    {-1, 3, "a8"}, {-1, 3, "a9"}, {-1, 3, "aa"}, {-1, 3, "ab"}, 
    {-1, 3, "ac"}, {-1, 3, "ad"}, {-1, 3, "ae"}, {-1, 3, "af"}, 
    {-1, 3, "b0"}, {-1, 3, "b1"}, {-1, 3, "b2"}, {-1, 3, "b3"}, 
    {-1, 3, "b4"}, {-1, 3, "b5"}, {-1, 3, "b6"}, {-1, 3, "b7"}, 
    {-1, 3, "b8"}, {-1, 3, "b9"}, {-1, 3, "ba"}, {-1, 3, "bb"}, 
    {-1, 3, "bc"}, {-1, 3, "bd"}, {-1, 3, "be"}, {-1, 3, "bf"}, 
    {-1, 3, "c0"}, {-1, 3, "c1"}, {-1, 3, "c2"}, {-1, 3, "c3"}, 
    {-1, 3, "c4"}, {-1, 3, "c5"}, {-1, 3, "c6"}, {-1, 3, "c7"}, 
    {-1, 3, "c8"}, {-1, 3, "c9"}, {-1, 3, "ca"}, {-1, 3, "cb"}, 
    {-1, 3, "cc"}, {-1, 3, "cd"}, {-1, 3, "ce"}, {-1, 3, "cf"}, 
    {-1, 3, "d0"}, {-1, 3, "d1"}, {-1, 3, "d2"}, {-1, 3, "d3"}, 
    {-1, 3, "d4"}, {-1, 3, "d5"}, {-1, 3, "d6"}, {-1, 3, "d7"}, 
    {-1, 3, "d8"}, {-1, 3, "d9"}, {-1, 3, "da"}, {-1, 3, "db"}, 
    {-1, 3, "dc"}, {-1, 3, "dd"}, {-1, 3, "de"}, {-1, 3, "df"}, 
    {-1, 3, "e0"}, {-1, 3, "e1"}, {-1, 3, "e2"}, {-1, 3, "e3"}, 
    {-1, 3, "e4"}, {-1, 3, "e5"}, {-1, 3, "e6"}, {-1, 3, "e7"}, 
    {-1, 3, "e8"}, {-1, 3, "e9"}, {-1, 3, "ea"}, {-1, 3, "eb"}, 
    {-1, 3, "ec"}, {-1, 3, "ed"}, {-1, 3, "ee"}, {-1, 3, "ef"}, 
    {-1, 3, "f0"}, {-1, 3, "f1"}, {-1, 3, "f2"}, {-1, 3, "f3"}, 
    {-1, 3, "f4"}, {-1, 3, "f5"}, {-1, 3, "f6"}, {-1, 3, "f7"}, 
    {-1, 3, "f8"}, {-1, 3, "f9"}, {-1, 3, "fa"}, {-1, 3, "fb"}, 
    {-1, 3, "fc"}, {-1, 3, "fd"}, {-1, 3, "fe"}, {-1, 3, "ff"}
};

/*
 * The following table is used for encoding and decoding the
 * segments of a URI path component.
 *
 * All ASCII control characters (00-1f and 7f) and the URI 'delim'
 * and 'unwise' characters are encoded.  In addition, the following
 * URI path component reserved characters are also encoded:
 *
 *      /  ;  =  ?
 *
 */

static const ByteKey pathenc[] = {
    {-1, 3, "00"}, {-1, 3, "01"}, {-1, 3, "02"}, {-1, 3, "03"}, 
    {-1, 3, "04"}, {-1, 3, "05"}, {-1, 3, "06"}, {-1, 3, "07"}, 
    {-1, 3, "08"}, {-1, 3, "09"}, {-1, 3, "0a"}, {-1, 3, "0b"}, 
    {-1, 3, "0c"}, {-1, 3, "0d"}, {-1, 3, "0e"}, {-1, 3, "0f"}, 
    {-1, 3, "10"}, {-1, 3, "11"}, {-1, 3, "12"}, {-1, 3, "13"}, 
    {-1, 3, "14"}, {-1, 3, "15"}, {-1, 3, "16"}, {-1, 3, "17"}, 
    {-1, 3, "18"}, {-1, 3, "19"}, {-1, 3, "1a"}, {-1, 3, "1b"}, 
    {-1, 3, "1c"}, {-1, 3, "1d"}, {-1, 3, "1e"}, {-1, 3, "1f"}, 
    {-1, 3, "20"}, {-1, 1, NULL}, {-1, 3, "22"}, {-1, 3, "23"}, 
    {-1, 1, NULL}, {-1, 3, "25"}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "2f"}, 
    { 0, 1, NULL}, { 1, 1, NULL}, { 2, 1, NULL}, { 3, 1, NULL}, 
    { 4, 1, NULL}, { 5, 1, NULL}, { 6, 1, NULL}, { 7, 1, NULL}, 
    { 8, 1, NULL}, { 9, 1, NULL}, {-1, 1, NULL}, {-1, 3, "3b"}, 
    {-1, 3, "3c"}, {-1, 3, "3d"}, {-1, 3, "3e"}, {-1, 3, "3f"}, 
    {-1, 1, NULL}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "5b"}, 
    {-1, 3, "5c"}, {-1, 3, "5d"}, {-1, 3, "5e"}, {-1, 1, NULL}, 
    {-1, 3, "60"}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "7b"}, 
    {-1, 3, "7c"}, {-1, 3, "7d"}, {-1, 1, NULL}, {-1, 3, "7f"}, 
    {-1, 3, "80"}, {-1, 3, "81"}, {-1, 3, "82"}, {-1, 3, "83"}, 
    {-1, 3, "84"}, {-1, 3, "85"}, {-1, 3, "86"}, {-1, 3, "87"}, 
    {-1, 3, "88"}, {-1, 3, "89"}, {-1, 3, "8a"}, {-1, 3, "8b"}, 
    {-1, 3, "8c"}, {-1, 3, "8d"}, {-1, 3, "8e"}, {-1, 3, "8f"}, 
    {-1, 3, "90"}, {-1, 3, "91"}, {-1, 3, "92"}, {-1, 3, "93"}, 
    {-1, 3, "94"}, {-1, 3, "95"}, {-1, 3, "96"}, {-1, 3, "97"}, 
    {-1, 3, "98"}, {-1, 3, "99"}, {-1, 3, "9a"}, {-1, 3, "9b"}, 
    {-1, 3, "9c"}, {-1, 3, "9d"}, {-1, 3, "9e"}, {-1, 3, "9f"}, 
    {-1, 3, "a0"}, {-1, 3, "a1"}, {-1, 3, "a2"}, {-1, 3, "a3"}, 
    {-1, 3, "a4"}, {-1, 3, "a5"}, {-1, 3, "a6"}, {-1, 3, "a7"}, 
    {-1, 3, "a8"}, {-1, 3, "a9"}, {-1, 3, "aa"}, {-1, 3, "ab"}, 
    {-1, 3, "ac"}, {-1, 3, "ad"}, {-1, 3, "ae"}, {-1, 3, "af"}, 
    {-1, 3, "b0"}, {-1, 3, "b1"}, {-1, 3, "b2"}, {-1, 3, "b3"}, 
    {-1, 3, "b4"}, {-1, 3, "b5"}, {-1, 3, "b6"}, {-1, 3, "b7"}, 
    {-1, 3, "b8"}, {-1, 3, "b9"}, {-1, 3, "ba"}, {-1, 3, "bb"}, 
    {-1, 3, "bc"}, {-1, 3, "bd"}, {-1, 3, "be"}, {-1, 3, "bf"}, 
    {-1, 3, "c0"}, {-1, 3, "c1"}, {-1, 3, "c2"}, {-1, 3, "c3"}, 
    {-1, 3, "c4"}, {-1, 3, "c5"}, {-1, 3, "c6"}, {-1, 3, "c7"}, 
    {-1, 3, "c8"}, {-1, 3, "c9"}, {-1, 3, "ca"}, {-1, 3, "cb"}, 
    {-1, 3, "cc"}, {-1, 3, "cd"}, {-1, 3, "ce"}, {-1, 3, "cf"}, 
    {-1, 3, "d0"}, {-1, 3, "d1"}, {-1, 3, "d2"}, {-1, 3, "d3"}, 
    {-1, 3, "d4"}, {-1, 3, "d5"}, {-1, 3, "d6"}, {-1, 3, "d7"}, 
    {-1, 3, "d8"}, {-1, 3, "d9"}, {-1, 3, "da"}, {-1, 3, "db"}, 
    {-1, 3, "dc"}, {-1, 3, "dd"}, {-1, 3, "de"}, {-1, 3, "df"}, 
    {-1, 3, "e0"}, {-1, 3, "e1"}, {-1, 3, "e2"}, {-1, 3, "e3"}, 
    {-1, 3, "e4"}, {-1, 3, "e5"}, {-1, 3, "e6"}, {-1, 3, "e7"}, 
    {-1, 3, "e8"}, {-1, 3, "e9"}, {-1, 3, "ea"}, {-1, 3, "eb"}, 
    {-1, 3, "ec"}, {-1, 3, "ed"}, {-1, 3, "ee"}, {-1, 3, "ef"}, 
    {-1, 3, "f0"}, {-1, 3, "f1"}, {-1, 3, "f2"}, {-1, 3, "f3"}, 
    {-1, 3, "f4"}, {-1, 3, "f5"}, {-1, 3, "f6"}, {-1, 3, "f7"}, 
    {-1, 3, "f8"}, {-1, 3, "f9"}, {-1, 3, "fa"}, {-1, 3, "fb"}, 
    {-1, 3, "fc"}, {-1, 3, "fd"}, {-1, 3, "fe"}, {-1, 3, "ff"}
};
#else
/*
 * The following table is used for encoding and decoding the segments of
 * a URI query component based on RFC 3986 (2005)
 *
 * A percent-encoding is used to represent a data octet in a component
 * when that octet's corresponding character is outside the allowed set
 * or is being used as a delimiter of, or within, the component.
 *
 * For all components all ASCII control characters (00-1f and 7f) and
 * characters above 7f are encoded, 'unreserved' characters are never
 * encoded.
 *
 * The query part of a URL is defined as: 
 *
 *    query       = *( pchar / "/" / "?" )
 *    pchar       = unreserved / pct-encoded / sub-delims / ":" / "@"
 *    unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
 *    sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
 *                   / "*" / "+" / "," / ";" / "="
 *
 * The rfc just defines the "outer" syntax of the query, the content is
 * usually form-urlencoded, where "&", "=" and "+" have special
 * meanings (https://www.w3.org/TR/html401/interact/forms.html#h-17.13.4.1)
 * so only the following sub-delims are allowed literally.
 *
 *   query-sub-delims = "!" / "$" / "'" / "(" / ")" / "*" / "," / ";" 
 *
 * This means a total of 78 characters are allowed unencoded in query
 * parts:
 *    unreserved:       26 + 26 + 10 + 4 = 66
 *    query-sub-delims: 8
 *    pchar:            66 + 8 + 2 = 76
 *    query:            76 + 2 = 78
 */

static const ByteKey queryenc[] = {
    /* 0x00 */  {-1, 3, "00"}, {-1, 3, "01"}, {-1, 3, "02"}, {-1, 3, "03"}, 
    /* 0x04 */  {-1, 3, "04"}, {-1, 3, "05"}, {-1, 3, "06"}, {-1, 3, "07"}, 
    /* 0x08 */  {-1, 3, "08"}, {-1, 3, "09"}, {-1, 3, "0a"}, {-1, 3, "0b"}, 
    /* 0x0c */  {-1, 3, "0c"}, {-1, 3, "0d"}, {-1, 3, "0e"}, {-1, 3, "0f"}, 
    /* 0x10 */  {-1, 3, "10"}, {-1, 3, "11"}, {-1, 3, "12"}, {-1, 3, "13"}, 
    /* 0x14 */  {-1, 3, "14"}, {-1, 3, "15"}, {-1, 3, "16"}, {-1, 3, "17"}, 
    /* 0x18 */  {-1, 3, "18"}, {-1, 3, "19"}, {-1, 3, "1a"}, {-1, 3, "1b"}, 
    /* 0x1c */  {-1, 3, "1c"}, {-1, 3, "1d"}, {-1, 3, "1e"}, {-1, 3, "1f"}, 
    /* 0x20 */  {-1, 3, "20"}, {-1, 1, NULL}, {-1, 3, "22"}, {-1, 3, "23"}, 
    /* 0x24 */  {-1, 1, NULL}, {-1, 3, "25"}, {-1, 3, "26"}, {-1, 1, NULL}, 
    /* 0x28 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "2b"}, 
    /* 0x2c */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x30 */  { 0, 1, NULL}, { 1, 1, NULL}, { 2, 1, NULL}, { 3, 1, NULL}, 
    /* 0x34 */  { 4, 1, NULL}, { 5, 1, NULL}, { 6, 1, NULL}, { 7, 1, NULL}, 
    /* 0x38 */  { 8, 1, NULL}, { 9, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x3c */  {-1, 3, "3c"}, {-1, 3, "3d"}, {-1, 3, "3e"}, {-1, 1, NULL}, 
    /* 0x40 */  {-1, 1, NULL}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    /* 0x44 */  {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    /* 0x48 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x4c */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x50 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x54 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x58 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "5b"}, 
    /* 0x5c */  {-1, 3, "5c"}, {-1, 3, "5d"}, {-1, 3, "5e"}, {-1, 1, NULL}, 
    /* 0x60 */  {-1, 3, "60"}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    /* 0x64 */  {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    /* 0x68 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x6c */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x70 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x74 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x78 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "7b"}, 
    /* 0x7c */  {-1, 3, "7c"}, {-1, 3, "7d"}, {-1, 1, NULL}, {-1, 3, "7f"}, 
    /* 0x80 */  {-1, 3, "80"}, {-1, 3, "81"}, {-1, 3, "82"}, {-1, 3, "83"}, 
    /* 0x84 */  {-1, 3, "84"}, {-1, 3, "85"}, {-1, 3, "86"}, {-1, 3, "87"}, 
    /* 0x88 */  {-1, 3, "88"}, {-1, 3, "89"}, {-1, 3, "8a"}, {-1, 3, "8b"}, 
    /* 0x8c */  {-1, 3, "8c"}, {-1, 3, "8d"}, {-1, 3, "8e"}, {-1, 3, "8f"}, 
    /* 0x90 */  {-1, 3, "90"}, {-1, 3, "91"}, {-1, 3, "92"}, {-1, 3, "93"}, 
    /* 0x94 */  {-1, 3, "94"}, {-1, 3, "95"}, {-1, 3, "96"}, {-1, 3, "97"}, 
    /* 0x98 */  {-1, 3, "98"}, {-1, 3, "99"}, {-1, 3, "9a"}, {-1, 3, "9b"}, 
    /* 0x9c */  {-1, 3, "9c"}, {-1, 3, "9d"}, {-1, 3, "9e"}, {-1, 3, "9f"}, 
    /* 0xa0 */  {-1, 3, "a0"}, {-1, 3, "a1"}, {-1, 3, "a2"}, {-1, 3, "a3"}, 
    /* 0xa4 */  {-1, 3, "a4"}, {-1, 3, "a5"}, {-1, 3, "a6"}, {-1, 3, "a7"}, 
    /* 0xa8 */  {-1, 3, "a8"}, {-1, 3, "a9"}, {-1, 3, "aa"}, {-1, 3, "ab"}, 
    /* 0xac */  {-1, 3, "ac"}, {-1, 3, "ad"}, {-1, 3, "ae"}, {-1, 3, "af"}, 
    /* 0xb0 */  {-1, 3, "b0"}, {-1, 3, "b1"}, {-1, 3, "b2"}, {-1, 3, "b3"}, 
    /* 0xb4 */  {-1, 3, "b4"}, {-1, 3, "b5"}, {-1, 3, "b6"}, {-1, 3, "b7"}, 
    /* 0xb8 */  {-1, 3, "b8"}, {-1, 3, "b9"}, {-1, 3, "ba"}, {-1, 3, "bb"}, 
    /* 0xbc */  {-1, 3, "bc"}, {-1, 3, "bd"}, {-1, 3, "be"}, {-1, 3, "bf"}, 
    /* 0xc0 */  {-1, 3, "c0"}, {-1, 3, "c1"}, {-1, 3, "c2"}, {-1, 3, "c3"}, 
    /* 0xc4 */  {-1, 3, "c4"}, {-1, 3, "c5"}, {-1, 3, "c6"}, {-1, 3, "c7"}, 
    /* 0xc8 */  {-1, 3, "c8"}, {-1, 3, "c9"}, {-1, 3, "ca"}, {-1, 3, "cb"}, 
    /* 0xcc */  {-1, 3, "cc"}, {-1, 3, "cd"}, {-1, 3, "ce"}, {-1, 3, "cf"}, 
    /* 0xd0 */  {-1, 3, "d0"}, {-1, 3, "d1"}, {-1, 3, "d2"}, {-1, 3, "d3"}, 
    /* 0xd4 */  {-1, 3, "d4"}, {-1, 3, "d5"}, {-1, 3, "d6"}, {-1, 3, "d7"}, 
    /* 0xd8 */  {-1, 3, "d8"}, {-1, 3, "d9"}, {-1, 3, "da"}, {-1, 3, "db"}, 
    /* 0xdc */  {-1, 3, "dc"}, {-1, 3, "dd"}, {-1, 3, "de"}, {-1, 3, "df"}, 
    /* 0xe0 */  {-1, 3, "e0"}, {-1, 3, "e1"}, {-1, 3, "e2"}, {-1, 3, "e3"}, 
    /* 0xe4 */  {-1, 3, "e4"}, {-1, 3, "e5"}, {-1, 3, "e6"}, {-1, 3, "e7"}, 
    /* 0xe8 */  {-1, 3, "e8"}, {-1, 3, "e9"}, {-1, 3, "ea"}, {-1, 3, "eb"}, 
    /* 0xec */  {-1, 3, "ec"}, {-1, 3, "ed"}, {-1, 3, "ee"}, {-1, 3, "ef"}, 
    /* 0xf0 */  {-1, 3, "f0"}, {-1, 3, "f1"}, {-1, 3, "f2"}, {-1, 3, "f3"}, 
    /* 0xf4 */  {-1, 3, "f4"}, {-1, 3, "f5"}, {-1, 3, "f6"}, {-1, 3, "f7"}, 
    /* 0xf8 */  {-1, 3, "f8"}, {-1, 3, "f9"}, {-1, 3, "fa"}, {-1, 3, "fb"}, 
    /* 0xfc */  {-1, 3, "fc"}, {-1, 3, "fd"}, {-1, 3, "fe"}, {-1, 3, "ff"}
};


/*
 * The following table is used for encoding and decoding the segments of
 * a URI path component based on RFC 3986 (2005)
 *
 * The query part of a URL is defined as: 
 *
 *    segment     = *pchar
 *    pchar       = unreserved / pct-encoded / sub-delims / ":" / "@"
 *    unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
 *    sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
 *                   / "*" / "+" / "," / ";" / "="
 * 
 * The RFC states that semicolon (";") and equals ("=") reserved
 * characters are often used to delimit parameters and parameter values
 * applicable to that segment. Whatever "often" means! to be on the safe
 * side, and to support that charaters as part of the segment, these are
 * encoded.
 * 
 *    segment-sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
 *                         / "*" / "+" / "," 

 * This means a total of 7cters are allowed unencoded in query
 * parts:
 *    unreserved:         26 + 26 + 10 + 4 = 66
 *    segment-sub-delims: 9
 *    segment-chars:      66 + 2 + 9 = 77
 */


static const ByteKey pathenc[] = {
    /* 0x00 */  {-1, 3, "00"}, {-1, 3, "01"}, {-1, 3, "02"}, {-1, 3, "03"}, 
    /* 0x04 */  {-1, 3, "04"}, {-1, 3, "05"}, {-1, 3, "06"}, {-1, 3, "07"}, 
    /* 0x08 */  {-1, 3, "08"}, {-1, 3, "09"}, {-1, 3, "0a"}, {-1, 3, "0b"}, 
    /* 0x0c */  {-1, 3, "0c"}, {-1, 3, "0d"}, {-1, 3, "0e"}, {-1, 3, "0f"}, 
    /* 0x10 */  {-1, 3, "10"}, {-1, 3, "11"}, {-1, 3, "12"}, {-1, 3, "13"}, 
    /* 0x14 */  {-1, 3, "14"}, {-1, 3, "15"}, {-1, 3, "16"}, {-1, 3, "17"}, 
    /* 0x18 */  {-1, 3, "18"}, {-1, 3, "19"}, {-1, 3, "1a"}, {-1, 3, "1b"}, 
    /* 0x1c */  {-1, 3, "1c"}, {-1, 3, "1d"}, {-1, 3, "1e"}, {-1, 3, "1f"}, 
    /* 0x20 */  {-1, 3, "20"}, {-1, 1, NULL}, {-1, 3, "22"}, {-1, 3, "23"}, 
    /* 0x24 */  {-1, 1, NULL}, {-1, 3, "25"}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x28 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x2c */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "2f"}, 
    /* 0x30 */  { 0, 1, NULL}, { 1, 1, NULL}, { 2, 1, NULL}, { 3, 1, NULL}, 
    /* 0x34 */  { 4, 1, NULL}, { 5, 1, NULL}, { 6, 1, NULL}, { 7, 1, NULL}, 
    /* 0x38 */  { 8, 1, NULL}, { 9, 1, NULL}, {-1, 1, NULL}, {-1, 3, "3b"}, 
    /* 0x3c */  {-1, 3, "3c"}, {-1, 3, "3d"}, {-1, 3, "3e"}, {-1, 3, "3f"}, 
    /* 0x40 */  {-1, 1, NULL}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    /* 0x44 */  {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    /* 0x48 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x4c */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x50 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x54 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x58 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "5b"}, 
    /* 0x5c */  {-1, 3, "5c"}, {-1, 3, "5d"}, {-1, 3, "5e"}, {-1, 1, NULL}, 
    /* 0x60 */  {-1, 3, "60"}, {10, 1, NULL}, {11, 1, NULL}, {12, 1, NULL}, 
    /* 0x64 */  {13, 1, NULL}, {14, 1, NULL}, {15, 1, NULL}, {-1, 1, NULL}, 
    /* 0x68 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x6c */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x70 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x74 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, 
    /* 0x78 */  {-1, 1, NULL}, {-1, 1, NULL}, {-1, 1, NULL}, {-1, 3, "7b"}, 
    /* 0x7c */  {-1, 3, "7c"}, {-1, 3, "7d"}, {-1, 1, NULL}, {-1, 3, "7f"}, 
    /* 0x80 */  {-1, 3, "80"}, {-1, 3, "81"}, {-1, 3, "82"}, {-1, 3, "83"}, 
    /* 0x84 */  {-1, 3, "84"}, {-1, 3, "85"}, {-1, 3, "86"}, {-1, 3, "87"}, 
    /* 0x88 */  {-1, 3, "88"}, {-1, 3, "89"}, {-1, 3, "8a"}, {-1, 3, "8b"}, 
    /* 0x8c */  {-1, 3, "8c"}, {-1, 3, "8d"}, {-1, 3, "8e"}, {-1, 3, "8f"}, 
    /* 0x90 */  {-1, 3, "90"}, {-1, 3, "91"}, {-1, 3, "92"}, {-1, 3, "93"}, 
    /* 0x94 */  {-1, 3, "94"}, {-1, 3, "95"}, {-1, 3, "96"}, {-1, 3, "97"}, 
    /* 0x98 */  {-1, 3, "98"}, {-1, 3, "99"}, {-1, 3, "9a"}, {-1, 3, "9b"}, 
    /* 0x9c */  {-1, 3, "9c"}, {-1, 3, "9d"}, {-1, 3, "9e"}, {-1, 3, "9f"}, 
    /* 0xa0 */  {-1, 3, "a0"}, {-1, 3, "a1"}, {-1, 3, "a2"}, {-1, 3, "a3"}, 
    /* 0xa4 */  {-1, 3, "a4"}, {-1, 3, "a5"}, {-1, 3, "a6"}, {-1, 3, "a7"}, 
    /* 0xa8 */  {-1, 3, "a8"}, {-1, 3, "a9"}, {-1, 3, "aa"}, {-1, 3, "ab"}, 
    /* 0xac */  {-1, 3, "ac"}, {-1, 3, "ad"}, {-1, 3, "ae"}, {-1, 3, "af"}, 
    /* 0xb0 */  {-1, 3, "b0"}, {-1, 3, "b1"}, {-1, 3, "b2"}, {-1, 3, "b3"}, 
    /* 0xb4 */  {-1, 3, "b4"}, {-1, 3, "b5"}, {-1, 3, "b6"}, {-1, 3, "b7"}, 
    /* 0xb8 */  {-1, 3, "b8"}, {-1, 3, "b9"}, {-1, 3, "ba"}, {-1, 3, "bb"}, 
    /* 0xbc */  {-1, 3, "bc"}, {-1, 3, "bd"}, {-1, 3, "be"}, {-1, 3, "bf"}, 
    /* 0xc0 */  {-1, 3, "c0"}, {-1, 3, "c1"}, {-1, 3, "c2"}, {-1, 3, "c3"}, 
    /* 0xc4 */  {-1, 3, "c4"}, {-1, 3, "c5"}, {-1, 3, "c6"}, {-1, 3, "c7"}, 
    /* 0xc8 */  {-1, 3, "c8"}, {-1, 3, "c9"}, {-1, 3, "ca"}, {-1, 3, "cb"}, 
    /* 0xcc */  {-1, 3, "cc"}, {-1, 3, "cd"}, {-1, 3, "ce"}, {-1, 3, "cf"}, 
    /* 0xd0 */  {-1, 3, "d0"}, {-1, 3, "d1"}, {-1, 3, "d2"}, {-1, 3, "d3"}, 
    /* 0xd4 */  {-1, 3, "d4"}, {-1, 3, "d5"}, {-1, 3, "d6"}, {-1, 3, "d7"}, 
    /* 0xd8 */  {-1, 3, "d8"}, {-1, 3, "d9"}, {-1, 3, "da"}, {-1, 3, "db"}, 
    /* 0xdc */  {-1, 3, "dc"}, {-1, 3, "dd"}, {-1, 3, "de"}, {-1, 3, "df"}, 
    /* 0xe0 */  {-1, 3, "e0"}, {-1, 3, "e1"}, {-1, 3, "e2"}, {-1, 3, "e3"}, 
    /* 0xe4 */  {-1, 3, "e4"}, {-1, 3, "e5"}, {-1, 3, "e6"}, {-1, 3, "e7"}, 
    /* 0xe8 */  {-1, 3, "e8"}, {-1, 3, "e9"}, {-1, 3, "ea"}, {-1, 3, "eb"}, 
    /* 0xec */  {-1, 3, "ec"}, {-1, 3, "ed"}, {-1, 3, "ee"}, {-1, 3, "ef"}, 
    /* 0xf0 */  {-1, 3, "f0"}, {-1, 3, "f1"}, {-1, 3, "f2"}, {-1, 3, "f3"}, 
    /* 0xf4 */  {-1, 3, "f4"}, {-1, 3, "f5"}, {-1, 3, "f6"}, {-1, 3, "f7"}, 
    /* 0xf8 */  {-1, 3, "f8"}, {-1, 3, "f9"}, {-1, 3, "fa"}, {-1, 3, "fb"}, 
    /* 0xfc */  {-1, 3, "fc"}, {-1, 3, "fd"}, {-1, 3, "fe"}, {-1, 3, "ff"}
};
#endif


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlEncodingWarnUnencoded --
 *
 *      Heuristic to warn about unencoded characters in a URL string.
 *      This function warns only about characters that have to be
 *      encoded always in the path and query component.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Produces potentially warnings in the error.log
 *
 *----------------------------------------------------------------------
 */

void
Ns_UrlEncodingWarnUnencoded(const char *msg, const char *chars) 
{
    static bool initialized = NS_FALSE;
    static bool mustBeEncoded[256];
    size_t i;

    NS_NONNULL_ASSERT(msg != NULL);
    NS_NONNULL_ASSERT(chars != NULL);

    if (!initialized) {
        /*
         * no need for a fine-grained lock.
         */
        Ns_MasterLock();
        for (i = 0u; i < 256; i++) {
            mustBeEncoded[i] = NS_TRUE;
        }

        /*
         * Don't try to distinguish for now between percents in
         * pct-encoded chars and literal percents (same with '=').
         */
        mustBeEncoded[UCHAR('%')] = NS_FALSE;
        mustBeEncoded[UCHAR('=')] = NS_FALSE;
        
        for (i = 0u; i < 256; i++) {
            if (pathenc[i].str == NULL) {
                mustBeEncoded[i] = NS_FALSE;
            }
            if (queryenc[i].str == NULL) {
                mustBeEncoded[i] = NS_FALSE;
            }
            /* fprintf(stderr, "%lu (%c): %d\n", i, (char)i, mustBeEncoded[i]);*/
        }
        initialized = NS_TRUE;
        Ns_MasterUnlock();
    }

    for (i = 0u; i < strlen(chars); i++) {
        if (mustBeEncoded[UCHAR(chars[i])]) {
            Ns_Log(Warning, "%s value '%s': byte with binary value %u must be url encoded",
                   msg, chars, UCHAR(chars[i]));
            /* 
             * Just warn about the first invalid character 
             */
            break;
        }
    }
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_GetUrlEncoding --
 *
 *      Get the encoding to use for Ns_UrlQueryDecode and related
 *      routines.  The encoding is determined by the following sequence:
 *
 *      charset parameter
 *      connection->urlEncoding
 *      config parameter urlEncoding
 *      static default
 *
 * Results:
 *      A Tcl_Encoding.
 *
 * Side effects:
 *      None. 
 *
 *----------------------------------------------------------------------
 */

Tcl_Encoding
Ns_GetUrlEncoding(const char *charset)
{
    Tcl_Encoding  encoding = NULL;

    if (charset != NULL) {
        encoding = Ns_GetCharsetEncoding(charset);
        if (encoding == NULL) {
            Ns_Log(Warning, "no encoding found for charset \"%s\"", charset);
        }
    }

    /*
     * The conn urlEncoding field is initialized from the config default
     * url encoding.  This implements the fallback described above in
     * a single step.
     */

    if (encoding == NULL) {
        const Conn *connPtr = (const Conn *) Ns_GetConn();

        if (connPtr != NULL) {
            encoding = connPtr->urlEncoding;
        } else {
            /* In the current code, the URL is decoded via
               UrlPathDecode() *before* the NsConnThread() is started, so
               connPtr will be normally NULL. 
               
               We need here an appropriate encoding to decode the
               URL. It would be nice to do here:
           
                 NsServer   *servPtr = NsGetServer(server);
                 return servPtr->encoding.urlEncoding;
           
               However, "server" is not available here.  Reading
               values from the config file would require "server" as
               well.
           
               Unfortunately, the general default for encoding opens a
               door for a path traversal attack with (invalid)
               UTF-8 characters.  For example, ".." can be encoded via
               UTF-8 in an URL as "%c0%ae%c0%ae" or
               "%e0%80%ae%e0%80%ae" and many more forms, so the
               literal checks against path traversal based on
               character data (here in Ns_NormalizePath()) fail. As a
               consequence, it would be possible to retrieve
               e.g. /etc/passwd from naviserver. For more details,
               see Section "Canonicalization" in

               http://www.cgisecurity.com/owasp/html/ch11s03.html 

               A simple approach to handle this attack is to fall back
               to utf-8 encodings and let Tcl do the UTF-8
               canonicalization.

               -gustaf neumann
            */
            encoding = Ns_GetCharsetEncoding("utf-8");
	}
    }

    return encoding;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlPathEncode, Ns_UrlPathDecode --
 *
 *      Encode/decode the given segment of URI path component.
 *      If encoding is NULL, UTF8 is assumed.
 *
 * Results:
 *      A pointer to the dstring's value, containing the transformed
 *      path component.
 *
 * Side effects:
 *      Transformed path component will be copied to given dstring.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_UrlPathEncode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return UrlEncode(dsPtr, urlSegment, encoding, 'p', NS_FALSE);
}

char *
Ns_UrlPathDecode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return UrlDecode(dsPtr, urlSegment, encoding, 'p');
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_UrlQueryEncode, Ns_UrlQueryDecode --
 *
 *      Encode/decode the given segment of URI query component.
 *      If encoding is NULL, UTF8 is assumed.
 *
 * Results:
 *      A pointer to the dstring's value, containing the transformed
 *      query string component.
 *
 * Side effects:
 *      Transformed query string component will be copied to given
 *      dstring.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_UrlQueryEncode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return UrlEncode(dsPtr, urlSegment, encoding, 'q', NS_FALSE);
}

char *
Ns_UrlQueryDecode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return UrlDecode(dsPtr, urlSegment, encoding, 'q');
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_EncodeUrlWithEncoding, Ns_EncodeUrlCharset,
 * Ns_DecodeUrlWithEncoding, Ns_DecodeUrlCharset --
 *
 *      Deprecated.
 *
 * Results:
 *      A pointer to the transformed urlSegment (which is part of the 
 *      passed-in DString's memory) 
 *
 * Side effects:
 *      transformed input will be copied to given dstring.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_EncodeUrlWithEncoding(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return Ns_UrlQueryEncode(dsPtr, urlSegment, encoding);
}

char *
Ns_EncodeUrlCharset(Ns_DString *dsPtr, const char *urlSegment, const char *charset)
{
    Tcl_Encoding encoding = Ns_GetUrlEncoding(charset);

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return Ns_UrlQueryEncode(dsPtr, urlSegment, encoding);

}

char *
Ns_DecodeUrlWithEncoding(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return Ns_UrlQueryDecode(dsPtr, urlSegment, encoding);
}

char *
Ns_DecodeUrlCharset(Ns_DString *dsPtr, const char *urlSegment, const char *charset)
{
    Tcl_Encoding encoding = Ns_GetUrlEncoding(charset);

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    return Ns_UrlQueryDecode(dsPtr, urlSegment, encoding);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUrlEncodeObjCmd --
 *
 *      Encode 1 or more segments of a either a URI path or query
 *      component part.  If the part is not specified, query is assumed.
 *      Segments are joined with a separator according to part.
 *
 *      NB: Path component param sections are not supported -- the ';'
 *      and '=' characters are encoded.  This is a relatively little used
 *      feature.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclUrlEncodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          nargs, upperCase = 0, result = TCL_OK;
    char        *charset = NULL;
    char         part = 'q';
    Ns_ObjvTable parts[] = {
        {"query", UCHAR('q')},
        {"path",  UCHAR('p')},
        {NULL,    0u}
    };
    Ns_ObjvSpec lopts[] = {
        {"-charset",   Ns_ObjvString, &charset,   NULL},
        {"-part",      Ns_ObjvIndex,  &part,      parts},
        {"-uppercase", Ns_ObjvBool,   &upperCase, INT2PTR(NS_TRUE)},
        {"--",         Ns_ObjvBreak,  NULL,       NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"args", Ns_ObjvArgs, &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };    

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_DString   ds;
        Tcl_Encoding encoding = NULL;
        int          i;

        if (charset != NULL) {
            encoding = Ns_GetCharsetEncoding(charset);
        }
        Ns_DStringInit(&ds);
        for (i = objc - nargs; i < objc; ++i) {
            (void)UrlEncode(&ds, Tcl_GetString(objv[i]), encoding, part, upperCase == 1);
            if (i + 1 < objc) {
                if (part == 'q') {
                    Ns_DStringNAppend(&ds, "&", 1);
                } else {
                    Ns_DStringNAppend(&ds, "/", 1);
                }
            }
        }
        Tcl_DStringResult(interp, &ds);
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclUrlDecodeObjCmd --
 *
 *      Decode a component of either a URL path or query.  If the part
 *      is not specified, query is assumed.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclUrlDecodeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int          result = TCL_OK;
    char        *charset = NULL, *chars = NULL;
    char         part = 'q';
    Ns_ObjvTable parts[] = {
        {"query",    UCHAR('q')},
        {"path",     UCHAR('p')},
        {NULL,       0u}
    };
    Ns_ObjvSpec  lopts[] = {
        {"-charset", Ns_ObjvString, &charset, NULL},
        {"-part",    Ns_ObjvIndex,  &part,    parts},
        {"--",       Ns_ObjvBreak,  NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"string", Ns_ObjvString, &chars, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(lopts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Ns_DString    ds;
        Tcl_Encoding  encoding = NULL;

        Ns_DStringInit(&ds);
        if (charset != NULL) {
            encoding = Ns_GetCharsetEncoding(charset);
        }
        
        (void)UrlDecode(&ds, chars, encoding, part);
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * UrlEncode --
 *
 *      Encode the given URL component according to part.
 *
 * Results:
 *      A pointer to the encoded string (which is part of the 
 *      passed-in DString's memory) 
 *
 * Side effects:
 *      Encoded URL component will be copied to given dstring.
 *
 *----------------------------------------------------------------------
 */

static char *
UrlEncode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding, char part, bool upperCase)
{
    register int   i, n;
    register char *q;
    const char    *p;
    Tcl_DString    ds;
    const ByteKey *enc;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    if (encoding != NULL) {
        urlSegment = Tcl_UtfToExternalDString(encoding, urlSegment, -1, &ds);
    }

    /*
     * Determine and set the requried dstring length.
     */

    enc = (part == 'q') ? queryenc : pathenc;
    n = 0;
    for (p = urlSegment; *p != '\0'; p++) {
	n += enc[UCHAR(*p)].len;
    }
    i = dsPtr->length;
    Ns_DStringSetLength(dsPtr, dsPtr->length + n + 1);

    /*
     * Copy the result directly to the pre-sized dstring.
     */

    q = dsPtr->string + i;
    for (p = urlSegment; *p != '\0'; p++) {
        if (enc[UCHAR(*p)].str == NULL) {
            *q++ = *p;
        } else if (*p == ' ' && part == 'q') {
            *q++ = '+';
        } else {
            char c1 = enc[UCHAR(*p)].str[0];
            char c2 = enc[UCHAR(*p)].str[1];
            
            if (upperCase) {
                if (c1 >= 'a' && c1 <= 'f') {
                    c1 = CHARCONV(upper, c1);
                }
                if (c2 >= 'a' && c2 <= 'f') {
                    c2 = CHARCONV(upper, c2);
                }
            }
            *q++ = '%';
            *q++ = c1;
            *q++ = c2;
        }
    }
    *q = '\0';

    if (encoding != NULL) {
        Tcl_DStringFree(&ds);
    }
    
    return dsPtr->string;
}


/*
 *----------------------------------------------------------------------
 *
 * UrlDecode --
 *
 *      Decode the given URL component according to part.
 *
 * Results:
 *      A pointer to the dstring's value, containing the decoded 
 *	    URL.
 *
 * Side effects:
 *	    Decoded URL will be copied to given dstring.
 *
 *----------------------------------------------------------------------
 */

static char *
UrlDecode(Ns_DString *dsPtr, const char *urlSegment, Tcl_Encoding encoding, char part)
{
    register int   i, n;
    register char *q;
    register const char *p;
    char          *copy = NULL;
    size_t         length;
    Tcl_DString    ds;
    const ByteKey *enc;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(urlSegment != NULL);

    /*
     * Copy the decoded characters directly to the dstring,
     * unless we need to do encoding.
     */
    length = strlen(urlSegment);
    if (encoding != NULL) {
        copy = ns_malloc(length + 1u);
        q = copy;
    } else {
        /*
         * Expand the dstring to the length of the input
         * string which will be the largest size required.
         */
        i = dsPtr->length;
        Ns_DStringSetLength(dsPtr, i + (int)length);
        q = dsPtr->string + i;
    }

    enc = (part == 'q') ? queryenc : pathenc;
    p = urlSegment;
    n = 0;
    while (likely(*p != '\0')) {
	int j;
        char c1, c2 = '\0';

        if (unlikely(p[0] == '%')) {
            /*
             * Decode percent code and make sure not to read date after
             * the NULL character. Convert character to lower if
             * necessary.
             */
            c1 = p[1];
            if (c1 != '\0') {
                if (c1 >= 'A' && c1 <= 'F') {
                    c1 = CHARCONV(lower, c1);
                }
                c2 = p[2];
                if (c2 >= 'A' && c2 <= 'F') {
                    c2 = CHARCONV(lower, c2);
                }
            }
        }

	if (c2 != '\0' 
            && (i = enc[UCHAR(c1)].hex) >= 0
            && (j = enc[UCHAR(c2)].hex) >= 0) {
            *q++ = (char)(UCHAR(UCHAR(i) << 4u) + UCHAR(j));
            p += 3;
        } else if (unlikely(p[0] == '+') && part == 'q') {
            *q++ = ' ';
            p++;
        } else {
            *q++ = *p++;
        }
        n++;
    }
    /* Ensure our new string is terminated */
    *q = '\0';

    if (encoding != NULL) {
        (void)Tcl_ExternalToUtfDString(encoding, copy, n, &ds);
        Ns_DStringAppend(dsPtr, Tcl_DStringValue(&ds));
        Tcl_DStringFree(&ds);
        if (copy != 0) {
            ns_free(copy);
        }
    } else {
        /*
         * Set the dstring length to the actual size required.
         */

        Ns_DStringSetLength(dsPtr, n);
    }

    return dsPtr->string;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 72
 * indent-tabs-mode: nil
 * End:
 */
