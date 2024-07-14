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
 * url.c --
 *
 *      Parse URLs.
 */

#include "nsd.h"

/*
 * Local typedefs of functions
 */

/*
 * Local functions defined in this file
 */

static char* ParseUpTo(const char *chars, char ch)
    NS_GNUC_NONNULL(1);


/*
 *----------------------------------------------------------------------
 *
 * Ns_RelativeUrl --
 *
 *      If the url passed in is for this server, then the initial
 *      part of the URL is stripped off. e.g., on a server whose
 *      location is http://www.foo.com, Ns_RelativeUrl of
 *      "http://www.foo.com/hello" will return "/hello".
 *
 * Results:
 *      A pointer to the beginning of the relative url in the
 *      passed-in url, or NULL if error.
 *
 * Side effects:
 *      Will set errno on error.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_RelativeUrl(const char *url, const char *location)
{
    const char *v, *result;

    if (url == NULL || location == NULL) {
        result = NULL;
    } else {

        /*
         * Ns_Match will return the point in URL where location stops
         * being equal to it because location ends.
         *
         * e.g., if location = "http://www.foo.com" and
         * url="http://www.foo.com/a/b" then after the call,
         * v="/a/b", or NULL if there's a mismatch.
         */

        v = Ns_Match(location, url);
        if (v != NULL) {
            url = v;
        }
        while (url[0] == '/' && url[1] == '/') {
            ++url;
        }
        result = url;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseUserInfo --
 *
 *      Parse the user-info part from the "authority" part of a URL
 *
 *           authority   = [ userinfo "@" ] host [ ":" port ]
 *
 *      and return the reminded of the string.
 *
 * Results:
 *      String starting with the "host" part.
 *
 * Side effects:
 *
 *      In case the "authority" contains "userinfo", it is returned via the
 *      pointer in the second argument.
 *
 *----------------------------------------------------------------------
 */

static char *
ParseUserInfo(char *chars, char **userinfo)
{
    char *p;

    /*
     * RFC 3986 defines
     *
     *   userinfo      = *( unreserved / pct-encoded / sub-delims / ":" )
     *   unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
     *   sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
     *               / "*" / "+" / "," / ";" / "="
     *
     *   ALPHA   = (%41-%5A and %61-%7A)
     *   DIGIT   = (%30-%39),
     *   hyphen (%2D), period (%2E), underscore (%5F), tilde (%7E)
     *   exclam (%21) dollar (%24) amp (%26) singlequote (%27)
     *   lparen (%28) lparen (%29) asterisk (%2A) plus (%2B)
     *   comma (%2C) semicolon (%3B) equals (%3D)
     *
     *   colon (%3a)
     *
     * Percent-encoded is just checked by the character range, but does not
     * check the two following (number) chars.
     *
     *   percent (%25) ... for percent-encoded
     */
    static const bool userinfo_table[256] = {
        /*          0  1  2  3   4  5  6  7   8  9  a  b   c  d  e  f */
        /* 0x00 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x10 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x20 */  0, 1, 0, 0,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,
        /* 0x30 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  0, 1, 0, 0,
        /* 0x40 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 1,
        /* 0x60 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 1, 0,
        /* 0x80 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x90 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xa0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xb0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xc0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xd0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xe0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xf0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
    };

    NS_NONNULL_ASSERT(chars != NULL);
    NS_NONNULL_ASSERT(userinfo != NULL);

    for (p = chars; userinfo_table[UCHAR(*p)] != 0; p++) {
        ;
    }

    if (*p == '\x40') {
        *userinfo = chars;
        *p = '\0';
        chars = p+1;
    } else {
        *userinfo = NULL;
    }
    /*fprintf(stderr, "==== userinfo p %.2x, '%s'\n", *p, chars);*/

    return chars;
}


/*
 *----------------------------------------------------------------------
 *
 * ParseUpTo --
 *
 *    Helper function of Ns_ParseUrl(). Return the characters up to a
 *    specified character and terminate the parsed string by a NUL
 *    character.  The string is searched from left to right.  If the
 *    character does not exist in the string, return NULL.
 *
 * Results:
 *    Parsed string or NULL.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

static char *
ParseUpTo(const char *chars, char ch)
{
    char *p = strchr(chars, INTCHAR(ch));

    if (p != NULL) {
        *p++ = '\0';
    }
    return p;
}

/*
 *----------------------------------------------------------------------
 *
 * ValidateChars --
 *
 *    Helper function of Ns_ParseUrl(). Scan a string up to the end based on
 *    the provided table of valid characters.
 *
 * Results:
 *
 *    When the string is valid, it is returned unmodified. in case it contains
 *    errors, NULL is returned and the error message is set.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

static char *
ValidateChars(char *chars, const bool *table, const char *msg, const char** errorMsg)
{
    char *p, *result;

    for (p = chars; table[UCHAR(*p)] != 0; p++) {
        ;
    }
    if (*p == '\0') {
        result = chars;
    } else {
        *errorMsg = msg;
        result = NULL;
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_ParseUrl --
 *
 *      Parse a URL into its component parts
 *
 * Results:
 *      NS_OK or NS_ERROR
 *
 * Side effects:
 *      Pointers to the protocol, host, port, path, and "tail" (last
 *      path element) will be set by reference in the passed-in pointers.
 *      The passed-in url will be modified.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_ParseUrl(char *url, bool strict, Ns_URL *urlPtr, const char **errorMsg)
{
    char *end;

    /*
     * RFC 3986 defines
     *
     *    foo://example.com:8042/over/there?name=ferret#nose
     *    \_/   \______________/\_________/ \_________/ \__/
     *     |           |            |            |        |
     *   scheme     authority       path        query   fragment
     *
     *      scheme  = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
     *      ALPHA   = (%41-%5A and %61-%7A)
     *      DIGIT   = (%30-%39),
     *      plus (%2B) hyphen (%2D), period (%2E),
     *
     *      underscore (%5F), tilde (%7E)
     */

    static const bool scheme_table[256] = {
        /*          0  1  2  3   4  5  6  7   8  9  a  b   c  d  e  f */
        /* 0x00 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x10 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x20 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 1,  0, 1, 1, 0,
        /* 0x30 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 0, 0,  0, 0, 0, 0,
        /* 0x40 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 0,
        /* 0x60 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 0,
        /* 0x80 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x90 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xa0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xb0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xc0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xd0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xe0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xf0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
    };

    /*
     * RFC 3986 defines (simplified)
     *
     *   path          = path-abempty    ; begins with "/" or is empty
     *                   / path-absolute   ; begins with "/" but not "//"
     *   path-absolute = "/" [ segment-nz *( "/" segment ) ]
     *   segment       = *pchar
     *   segment-nz    = 1*pchar
     *   pchar         = unreserved / pct-encoded / sub-delims / ":" / "@"
     *
     *   unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
     *   sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
     *               / "*" / "+" / "," / ";" / "="
     *
     *   ALPHA   = (%41-%5A and %61-%7A)
     *   DIGIT   = (%30-%39),
     *   hyphen (%2D), period (%2E), underscore (%5F), tilde (%7E)
     *   exclam (%21) dollar (%24) amp (%26) singlequote (%27)
     *   lparen (%28) lparen (%29) asterisk (%2A) plus (%2B)
     *   comma (%2C) semicolon (%3B) equals (%3D)
     *
     *   slash (%2F) colon (%3A) at (%40)
     *
     * Percent-encoded is just checked by the character range, but does not
     * check the two following (number) chars.
     *
     *   percent (%25) ... for percent-encoded
     */

    static const bool path_table[256] = {
        /*          0  1  2  3   4  5  6  7   8  9  a  b   c  d  e  f */
        /* 0x00 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x10 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x20 */  0, 1, 0, 0,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x30 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  0, 1, 0, 0,
        /* 0x40 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 1,
        /* 0x60 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 1, 0,
        /* 0x80 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x90 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xa0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xb0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xc0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xd0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xe0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xf0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
    };

    /*
     * RFC 3986 defines
     *
     *   query       = *( pchar / "/" / "?" )
     *   fragment    = *( pchar / "/" / "?" )
     *
     *   pchar       = unreserved / pct-encoded / sub-delims / ":" / "@"
     *
     *   unreserved  = ALPHA / DIGIT / "-" / "." / "_" / "~"
     *   sub-delims  = "!" / "$" / "&" / "'" / "(" / ")"
     *               / "*" / "+" / "," / ";" / "="
     *
     *   ALPHA   = (%41-%5A and %61-%7A)
     *   DIGIT   = (%30-%39),
     *   hyphen (%2D), period (%2E), underscore (%5F), tilde (%7E)
     *   exclam (%21) dollar (%24) amp (%26) singlequote (%27)
     *   lparen (%28) lparen (%29) asterisk (%2A) plus (%2B)
     *   comma (%2C) semicolon (%3B) equals (%3D)
     *
     *   slash (%2F) colon (%3A) question mark (%3F) at (%40)
     *
     * Percent-encoded is just checked by the character range, but does not
     * check the two following (number) chars.
     *
     *   percent (%25) ... for percent-encoded
     */

    static const bool fragment_table[256] = {
        /*          0  1  2  3   4  5  6  7   8  9  a  b   c  d  e  f */
        /* 0x00 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x10 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x20 */  0, 1, 0, 0,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x30 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  0, 1, 0, 1,
        /* 0x40 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 1,
        /* 0x60 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 1, 0,
        /* 0x80 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x90 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xa0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xb0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xc0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xd0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xe0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xf0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
    };
    static const bool alpha_table[256] = {
        /*          0  1  2  3   4  5  6  7   8  9  a  b   c  d  e  f */
        /* 0x00 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x10 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x20 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x30 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x40 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 0,
        /* 0x60 */  0, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 0,  0, 0, 0, 0,
        /* 0x80 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0x90 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xa0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xb0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xc0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xd0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xe0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
        /* 0xf0 */  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
    };

    NS_NONNULL_ASSERT(urlPtr);

    memset(urlPtr, 0, sizeof(Ns_URL));

    /*
     * Set variable "end" to the end of the protocol
     * http://www.foo.com:8000/baz/blah/spoo.html
     *     ^
     *     +--end
     */

    if (alpha_table[UCHAR(*url)]) {
        for (end = url+1; scheme_table[UCHAR(*end)] != 0; end++) {
            ;
        }
    } else {
        end = url;
    }
    if (end != url && *end == ':') {
        /*
         * There is a protocol specified. Clear out the colon.
         * Set pprotocol to the start of the protocol, and url to
         * the first character after the colon.
         *
         * http\0//www.foo.com:8000/baz/blah/spoo.html
         * ^   ^ ^
         * |   | +-- url
         * |   +-- end
         * +-------- protocol
         */

        *end = '\0';
        urlPtr->protocol = url;
        url = end + 1;
        /*fprintf(stderr, "SCHEME looks ok: %s\n", *pprotocol);*/

    } else if (*end != '/' && *end != '?' && *end != '#' && *end != '\0' ) {
        /*
         * We do not have an explicit relative URL starting with a
         * slash. Accept relative URL based on the heuristic to avoid getting
         * every non-accepted scheme here (the remainding URL must not have a
         * colon before a slash.
         */
        char *p;

        for (p = end; *p != '\0' && *p != '/'; p++) {
            if (*p == ':') {
                /*
                 * We have a colon before the slash or end, do not accept
                 * this.
                 */
                Ns_Log(Debug, "URI scheme does not look ok: last char 0x%.2x '%s'",
                       *end, url);
                *errorMsg = "invalid scheme";
                return NS_ERROR;
            }
        }
    }


    if (url[0] == '/' && url[1] == '/') {
        bool  hostParsedOk;

        urlPtr->path = (char *)"";
        urlPtr->tail = (char *)"";

        /*
         * The URL starts with two slashes, which means an authority part
         * (host) is specified.  Advance url past that and set *phost.
         *
         * http\0//www.foo.com:8000/baz/blah/spoo.html
         * ^   ^   ^
         * |   |   +-- url, *host
         * |   +-- end
         * +-------- protocol
         */
        url = url + 2;

        /*
         * RFC 3986 defines
         *
         *     authority   = [ userinfo "@" ] host [ ":" port ]
         *
         */
        url = ParseUserInfo(url, &urlPtr->userinfo);
        urlPtr->host = url;

        /*
         * Parse authority part and return the optional string pointing to the
         * port.
         */
        hostParsedOk = Ns_HttpParseHost2(url, strict, &urlPtr->host, &urlPtr->port, &end);
        if (!hostParsedOk) {
            *errorMsg = "invalid authority";
            return NS_ERROR;
        }

        if (urlPtr->port != NULL) {

            /*
             * A port was specified. Set urlPtr->port to the first
             * digit.
             *
             * http\0//www.foo.com\08000/baz/blah/spoo.html
             * ^       ^          ^ ^
             * |       +-- host   | +------ url, port
             * +----- protocol    +--- end
             */

            url = urlPtr->port;
            urlPtr->port = url;
        }
    } else {
        end = url;
    }
    /*
     * "end" points now either to
     * - the string terminator (NUL)
     * - the slash which starts the path/tail, or to
     * - one of the remaining components (query, or fragment)
     *
     * http\0//www.foo.com\08000\0baz/blah/spoo.html
     * ^       ^            ^   ^ ^
     * |       |            |   | +-- url
     * |       +-- host     |   +-- end
     * +----- protocol      +-- port
     */
    /*fprintf(stderr, "CHECK FOR PATH <%s>\n", end);*/


    if (*end == '\0') {
        /*
         * No path, tail, query, fragment specified: we are done.
         */

    } else if (*end == '#') {
        /*
         * No path, tail, query, just a fragment specified.
         * We could validate.
         */
        *end = '\0';
        urlPtr->fragment = end + 1;

    } else if (*end == '?') {
        /*
         * No path, tail, just a query and maybe a fragment specified.
         */
        *end = '\0';
        urlPtr->query = end + 1;
        urlPtr->fragment = ParseUpTo(urlPtr->query, '#');

    } else {
        if (*end == '/') {
            urlPtr->path = (char *)"";
            urlPtr->tail = (char *)"";

            /*
             * We have a path, tail, and maybe a query or fragment specified.
             */
            *end = '\0';
            url = end + 1;
            /*
             * Set the path to URL and advance to the last slash.
             * Set ptail to the character after that, or if there is none,
             * it becomes path and path becomes an empty string.
             *
             * http\0//www.foo.com\08000\0baz/blah/spoo.html
             * ^       ^            ^   ^ ^       ^^
             * |       |            |   | |       |+-- tail
             * |       |            |   | |       +-- end
             * |       |            |   | +-- path
             * |       +-- host     |   +-- end
             * +----- protocol      +-- port
             */


            /*
             * Separate the "tail" from the "path", otherwise the string is
             * just "tail".
             */
            urlPtr->query = ParseUpTo(url, '?');
            if (urlPtr->query == NULL) {
                urlPtr->fragment = ParseUpTo(url, '#');
            }

            end = strrchr(url, INTCHAR('/'));
            if (end == NULL) {
                urlPtr->tail = url;
            } else {
                *end = '\0';
                urlPtr->path = url;
                urlPtr->tail = end + 1;
            }

        } else {
            /*
             * The URL starts with no slash, just set the "tail" and let
             * "path" undefined (legacy NaviServer).
             */
            urlPtr->tail = end;
        }

        if (urlPtr->tail != NULL) {
            if (urlPtr->query == NULL) {
                urlPtr->query = ParseUpTo(urlPtr->tail, '?');
            }
            if (urlPtr->query != NULL) {
                urlPtr->fragment = ParseUpTo(urlPtr->query, '#');
            } else if (urlPtr->fragment == NULL) {
                urlPtr->fragment = ParseUpTo(urlPtr->tail, '#');
            }
        }
        if (strict) {
            /*
             * Validate content.
             */
            if (urlPtr->query != NULL) {
                urlPtr->query = ValidateChars(urlPtr->query, fragment_table,
                                              "query contains invalid character", errorMsg);
            }
            if (urlPtr->fragment != NULL) {
                urlPtr->fragment = ValidateChars(urlPtr->fragment, fragment_table,
                                                 "fragment contains invalid character", errorMsg);
            }
            if (urlPtr->tail != NULL) {
                urlPtr->tail = ValidateChars(urlPtr->tail, path_table,
                                             "query contains invalid character", errorMsg);
            }
            if (urlPtr->path != NULL) {
                urlPtr->path = ValidateChars(urlPtr->path, path_table,
                                             "path contains invalid character", errorMsg);
            }
        }
    }

    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_AbsoluteUrl --
 *
 *      Construct a URL based on baseurl but with as many parts of
 *      the incomplete url as possible.
 *
 * Results:
 *      NS_OK or NS_ERROR.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_AbsoluteUrl(Ns_DString *dsPtr, const char *urlString, const char *baseString)
{
    Ns_DString    urlDs, baseDs;
    Ns_URL        url, base;
    const char   *errorMsg = NULL;
    Ns_ReturnCode status;

    /*
     * Copy the URL's to allow Ns_ParseUrl to destroy them.
     */

    Ns_DStringInit(&urlDs);
    Ns_DStringInit(&baseDs);

    /*
     * The first part does not have to be a valid URL. If it is just empty,
     * interpret it as "/".
     */
    Ns_DStringAppend(&urlDs, urlString);
    if (unlikely(urlDs.length == 0)) {
        Tcl_DStringAppend(&urlDs, "/", 1);
    }
    (void) Ns_ParseUrl(urlDs.string, NS_FALSE, &url, &errorMsg);

    Ns_DStringAppend(&baseDs, baseString);
    status = Ns_ParseUrl(baseDs.string, NS_FALSE, &base, &errorMsg);

    if (base.protocol == NULL || base.host == NULL || base.path == NULL) {
        status = NS_ERROR;
        goto done;
    }
    if (url.protocol == NULL) {
        url.protocol = base.protocol;
    }
    assert(url.protocol != NULL);

    if (url.host == NULL) {
        url.host = base.host;
        url.port = base.port;
    }
    assert(url.host != NULL);

    if (url.path == NULL) {
        url.path = base.path;
    }
    assert(url.path != NULL);

    if (strchr(url.host, INTCHAR(':')) == NULL) {
        /*
         * We have to use IP literal notation to avoid ambiguity of colon
         * (part of address or separator for port).
         */
        Ns_DStringVarAppend(dsPtr, url.protocol, "://", url.host, (char *)0L);
    } else {
        Ns_DStringVarAppend(dsPtr, url.protocol, "://[", url.host, "]", (char *)0L);
    }
    if (url.port != NULL) {
        Ns_DStringVarAppend(dsPtr, ":", url.port, (char *)0L);
    }
    if (*url.path == '\0') {
        Ns_DStringVarAppend(dsPtr, "/", url.tail, (char *)0L);
    } else {
        Ns_DStringVarAppend(dsPtr, "/", url.path, "/", url.tail, (char *)0L);
    }
done:
    Ns_DStringFree(&urlDs);
    Ns_DStringFree(&baseDs);

    return status;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclParseUrlObjCmd --
 *
 *    Implements "ns_parseurl". Offers the functionality of
 *    Ns_ParseUrl on the Tcl layer.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */

int
NsTclParseUrlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, strict = 0;
    char       *urlString;
    Ns_ObjvSpec opts[] = {
        {"-strict",     Ns_ObjvBool,    &strict,          INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"url",  Ns_ObjvString, &urlString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        char       *url;
        Ns_URL      u;
        const char *errorMsg = NULL;

        url = ns_strdup(urlString);

        if (Ns_ParseUrl(url, (bool)strict, &u, &errorMsg) == NS_OK) {
            Tcl_Obj *resultObj = Tcl_NewListObj(0, NULL);

            if (u.protocol != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("proto", 5));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.protocol, TCL_INDEX_NONE));
            }
            if (u.userinfo != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("userinfo", 8));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.userinfo, TCL_INDEX_NONE));
            }
            if (u.host != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("host", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.host, TCL_INDEX_NONE));
            }
            if (u.port != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("port", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.port, TCL_INDEX_NONE));
            }
            if (u.path != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("path", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.path, TCL_INDEX_NONE));
            }
            if (u.tail != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("tail", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.tail, TCL_INDEX_NONE));
            }
            if (u.query != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("query", 5));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.query, TCL_INDEX_NONE));
            }
            if (u.fragment != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("fragment", 8));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(u.fragment, TCL_INDEX_NONE));
            }
            if (errorMsg != NULL) {
                Ns_TclPrintfResult(interp, "Could not parse URL \"%s\": %s", urlString, errorMsg);
                result = TCL_ERROR;
            } else {
                Tcl_SetObjResult(interp, resultObj);
            }

        } else {
            Ns_TclPrintfResult(interp, "Could not parse URL \"%s\": %s", urlString, errorMsg);
            result = TCL_ERROR;
        }
        ns_free(url);
    }
    /*Ns_Log(Notice, "===== ns_parseurl '%s' returns result %d", urlString, result);*/
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclParseHostportObjCmd --
 *
 *    Implements "ns_parsehostport". Offers the functionality of
 *    Ns_HttpParseHost2 on the Tcl layer.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */

int
NsTclParseHostportObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK, strict = 0;
    char       *hostportString;
    Ns_ObjvSpec opts[] = {
        {"-strict",     Ns_ObjvBool,    &strict,          INT2PTR(NS_TRUE)},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"hostport",  Ns_ObjvString, &hostportString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        char *hostport, *hostStart, *portStart, *end;
        bool  success;

        hostport = ns_strdup(hostportString);
        success = Ns_HttpParseHost2(hostport, strict, &hostStart, &portStart, &end);
        if (success && *hostStart != '\0' && portStart != hostport) {
            Tcl_Obj *resultObj = Tcl_NewListObj(0, NULL);

            if (hostStart != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("host", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(hostStart, TCL_INDEX_NONE));
            }
            if (portStart != NULL) {
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj("port", 4));
                Tcl_ListObjAppendElement(interp, resultObj, Tcl_NewStringObj(portStart, TCL_INDEX_NONE));
            }

            Tcl_SetObjResult(interp, resultObj);

        } else {
            Ns_TclPrintfResult(interp, "Could not parse host and port \"%s\"", hostportString);
            result = TCL_ERROR;
        }
        ns_free(hostport);
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclAbsoluteUrlObjCmd --
 *
 *    Implements "ns_absoluteurl". Offers the functionality of
 *    Ns_AbsoluteUrl on the Tcl layer.
 *
 * Results:
 *    Tcl result.
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */
int
NsTclAbsoluteUrlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int         result = TCL_OK;
    char       *urlString, *baseString;
    Ns_ObjvSpec args[] = {
        {"partialurl", Ns_ObjvString, &urlString, NULL},
        {"baseurl",    Ns_ObjvString, &baseString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
    } else {
        Tcl_DString ds;

        Tcl_DStringInit(&ds);
        if (Ns_AbsoluteUrl(&ds, urlString, baseString) == NS_OK) {
            Tcl_DStringResult(interp, &ds);
        } else {
            Ns_TclPrintfResult(interp, "Could not parse base URL into protocol, host and path");
            Tcl_DStringFree(&ds);
            result = TCL_ERROR;
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_PlainUrlPath --
 *
 *    Checks, it the provides URL path is valid and does not contain
 *    query variables or fragments.
 *
 * Results:
 *    Boolean.
 *
 * Side effects:
 *    none
 *
 *----------------------------------------------------------------------
 */
bool Ns_PlainUrlPath(const char *url, const char **errorMsgPtr)
{
    Ns_URL       parsedUrl;
    bool         result = NS_TRUE;
    Tcl_DString  ds;

    NS_NONNULL_ASSERT(url != NULL);
    NS_NONNULL_ASSERT(errorMsgPtr != NULL);

    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, url, TCL_INDEX_NONE);

    if (Ns_ParseUrl(ds.string, NS_FALSE, &parsedUrl, errorMsgPtr) != NS_OK) {
        result = NS_FALSE;
    } else if (parsedUrl.query != NULL || parsedUrl.fragment != NULL) {
        *errorMsgPtr = "request path contains query and/or fragment, which is not allowed";
        result = NS_FALSE;
    }
    Tcl_DStringFree(&ds);

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
