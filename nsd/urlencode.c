/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
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

NS_RCSID("@(#) $Header$");


/*
 * The following structure defines the encoding attributes
 * of a byte.
 */

typedef struct ByteKey {
    int   hex;	    /* Valid hex value or -1. */
    int   len;	    /* Length required to encode string. */
    char *str;	    /* String for multibyte encoded character. */
} ByteKey;

/*
 * Local functions defined in this file.
 */

static char *UrlEncode(Ns_DString *dsPtr, char *string,
                       Tcl_Encoding encoding, int part);
static char *UrlDecode(Ns_DString *dsPtr, char *string,
                       Tcl_Encoding encoding, int part);

/*
 * Local variables defined in this file.
 */

/*
 * The following table is used for encoding and decoding the
 * segments of a URI query component.
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

static ByteKey queryenc[] = {
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

static ByteKey pathenc[] = {
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
Ns_GetUrlEncoding(char *charset)
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
        Conn *connPtr = (Conn *) Ns_GetConn();
        if (connPtr != NULL) {
            encoding = connPtr->urlEncoding;
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
Ns_UrlPathEncode(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding)
{
    return UrlEncode(dsPtr, string, encoding, 'p');
}

char *
Ns_UrlPathDecode(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding)
{
    return UrlDecode(dsPtr, string, encoding, 'p');
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
Ns_UrlQueryEncode(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding)
{
    return UrlEncode(dsPtr, string, encoding, 'q');
}

char *
Ns_UrlQueryDecode(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding)
{
    return UrlDecode(dsPtr, string, encoding, 'q');
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
 *      A pointer to the transformed string (which is part of the 
 *      passed-in DString's memory) 
 *
 * Side effects:
 *      transformed input will be copied to given dstring.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_EncodeUrlWithEncoding(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding)
{
    return Ns_UrlQueryEncode(dsPtr, string, encoding);
}

char *
Ns_EncodeUrlCharset(Ns_DString *dsPtr, char *string, char *charset)
{
    Tcl_Encoding encoding = Ns_GetUrlEncoding(charset);

    return Ns_UrlQueryEncode(dsPtr, string, encoding);

}

char *
Ns_DecodeUrlWithEncoding(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding)
{
    return Ns_UrlQueryDecode(dsPtr, string, encoding);
}

char *
Ns_DecodeUrlCharset(Ns_DString *dsPtr, char *string, char *charset)
{
    Tcl_Encoding encoding = Ns_GetUrlEncoding(charset);

    return Ns_UrlQueryDecode(dsPtr, string, encoding);
}


/*
 *----------------------------------------------------------------------
 *
 * NsUpdateUrlEncode
 *
 *      Initialize UrlEncode structures from config.
 *
 * Results:
 *      Tcl result. 
 *
 * Side effects:
 *      See docs. 
 *
 *----------------------------------------------------------------------
 */

void
NsUpdateUrlEncode(void)
{

    nsconf.encoding.urlCharset = Ns_ConfigGetValue(NS_CONFIG_PARAMETERS,
                                                   "URLCharset");
    if (nsconf.encoding.urlCharset != NULL) {
        nsconf.encoding.urlEncoding =
            Ns_GetCharsetEncoding(nsconf.encoding.urlCharset);
        if (nsconf.encoding.urlEncoding == NULL ) {
            Ns_Log(Warning,
                   "no encoding found for charset \"%s\" from config",
                   nsconf.encoding.urlCharset);
        }
    } else {
        nsconf.encoding.urlEncoding = NULL;
    }
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
NsTclUrlEncodeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_DString  ds;
    int         i, nargs, part = 'q';

    Ns_ObjvTable parts[] = {
        {"query",    'q'},
        {"path",     'p'},
        {NULL,       0}
    };
    Ns_ObjvSpec opts[] = {
        {"-part",    Ns_ObjvIndex,  &part,   &parts},
        {"--",       Ns_ObjvBreak,  NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"component", Ns_ObjvArgs,  &nargs, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_DStringInit(&ds);
    for (i = objc - nargs; i < objc; ++i) {
        UrlEncode(&ds, Tcl_GetString(objv[i]), NULL, part);
        if (i + 1 < objc) {
            if (part == 'q') {
                Ns_DStringNAppend(&ds, "&", 1);
            } else {
                Ns_DStringNAppend(&ds, "/", 1);
            }
        }
    }
    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
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
NsTclUrlDecodeObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    Ns_DString   ds;
    char        *string;
    int          part = 'q';

    Ns_ObjvTable parts[] = {
        {"query",    'q'},
        {"path",     'p'},
        {NULL,       0}
    };
    Ns_ObjvSpec opts[] = {
        {"-part",    Ns_ObjvIndex,   &part,   &parts},
        {"--",       Ns_ObjvBreak,    NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"component", Ns_ObjvString, &string, NULL},
        {NULL, NULL, NULL, NULL}
    };
    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_DStringInit(&ds);
    UrlDecode(&ds, string, NULL, part);
    Tcl_DStringResult(interp, &ds);

    return TCL_OK;
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
UrlEncode(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding, int part)
{
    register int   i, n;
    register char *p, *q;
    Tcl_DString    ds;
    ByteKey       *enc;

    if (encoding != NULL) {
        string = Tcl_UtfToExternalDString(encoding, string, -1, &ds);
    }

    /*
     * Determine and set the requried dstring length.
     */

    enc = (part == 'q') ? queryenc : pathenc;
    p = string;
    n = 0;
    while ((i = UCHAR(*p)) != 0) {
        n += enc[i].len;
        ++p;
    }
    i = dsPtr->length;
    Ns_DStringSetLength(dsPtr, dsPtr->length + n);

    /*
     * Copy the result directly to the pre-sized dstring.
     */

    q = dsPtr->string + i;
    p = string;
    while ((i = UCHAR(*p)) != 0) {
        if (enc[i].str == NULL) {
            *q++ = *p;
        } else if (*p == ' ' && part == 'q') {
            *q++ = '+';
        } else {
            *q++ = '%';
            *q++ = enc[i].str[0];
            *q++ = enc[i].str[1];
        }
        ++p;
    }

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

char *
UrlDecode(Ns_DString *dsPtr, char *string, Tcl_Encoding encoding, int part)
{
    register int   i, j, n;
    register char *p, *q;
    char          *copy = NULL;
    int            length;
    Tcl_DString    ds;
    ByteKey       *enc;

    /*
     * Copy the decoded characters directly to the dstring,
     * unless we need to do encoding.
     */
    length = strlen(string);
    if (encoding != NULL) {
        copy = ns_malloc((size_t)(length+1));
        q = copy;
    } else {
        /*
         * Expand the dstring to the length of the input
         * string which will be the largest size required.
         */
        i = dsPtr->length;
        Ns_DStringSetLength(dsPtr, i + length);
        q = dsPtr->string + i;
    }

    enc = (part == 'q') ? queryenc : pathenc;
    p = string;
    n = 0;
    while (UCHAR(*p) != '\0') {
        if (UCHAR(p[0]) == '%' &&
            (i = enc[UCHAR(p[1])].hex) >= 0 &&
            (j = enc[UCHAR(p[2])].hex) >= 0) {
            *q++ = (unsigned char) ((i << 4) + j);
            p += 3;
#ifndef URLDECODE_RELAXED 
        } else if (UCHAR(p[0]) == '+' && part == 'q') {
#else
        } else if (UCHAR(p[0]) == '+') {
#endif
            *q++ = ' ';
            ++p;
        } else {
            *q++ = *p++;
        }
        ++n;
    }
    /* Ensure our new string is terminated */
    *q = '\0';

    if (encoding != NULL) {
        Tcl_ExternalToUtfDString(encoding, copy, n, &ds);
        Ns_DStringAppend(dsPtr, Tcl_DStringValue(&ds));
        Tcl_DStringFree(&ds);
        if (copy) {
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
