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
 * fieldvalue.c --
 *
 *      Routines to parse the content of request/replay header fields.
 *
 *      RFC7230: https://tools.ietf.org/html/rfc7230#section-3.2.6
 *
 *      Example:
 *      RFC7239: https://tools.ietf.org/html/rfc7239#section-4
 *
 */
#include "nsd.h"

/*
 * Local functions defined in this file.
 */

static const unsigned char *
GetToken(Tcl_DString *dsPtr, const unsigned char *source)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static const unsigned char *
GetQuotedString(Tcl_DString *dsPtr, const unsigned char *source)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static const unsigned char *
SkipWhitespace(const unsigned char *source)
    NS_GNUC_NONNULL(1);

/*
 *----------------------------------------------------------------------
 *
 * GetToken --
 *
 *      Read a token as defined in rfc7230#section-3.2.6 from source and add
 *      it to the provided Tcl_DString.
 *
 * Results:
 *      Pointer to the next character to parse.  In case, the source
 *      pointer is the same as the result, no token was parsed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const unsigned char *
GetToken(Tcl_DString *dsPtr, const unsigned char *source)
{
    register const unsigned char *p;
    /*
     * RFC 7230 https://tools.ietf.org/html/rfc7230#section-3.2.6
     * defines a token as
     *
     * token          = 1*tchar
     * tchar          = "!" / "#" / "$" / "%" / "&" / "'" / "*"
     *                  / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
     *                  / DIGIT / ALPHA
     *                  ; any VCHAR, except delimiters
     *
     *                 ; DIGIT %x30-39
     *                 ; ALPHA %x41-5A / %x61-7A
     */
    static const bool token_char[] = {
        /*          0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
        /* 0x00 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0x10 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0x20 */  0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0,
        /* 0x30 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
        /* 0x40 */  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        /* 0x50 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,
        /* 0x60 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        /* 0x70 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,
        /* 0x80 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0x90 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0xa0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0xb0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0xc0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0xd0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0xe0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        /* 0xf0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(source != NULL);

    for (p = source; token_char[*p]; p++) {
        /*
         * When the string is properly preallocated, the following
         * statement can be simplified
         */
        Tcl_DStringAppend(dsPtr, (const char *)p, 1);
    }

    return p;
}

/*
 *----------------------------------------------------------------------
 *
 * GetQuotedString --
 *
 *      Read a quoted string as defined in rfc7230#section-3.2.6 from source
 *      and add it to the provided Tcl_DString.
 *
 * Results:
 *      Pointer to the next character to parse.  In case, the source pointer
 *      is the same result, no quoted string could be parsed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
const unsigned char *
GetQuotedString(Tcl_DString *dsPtr, const unsigned char *source)
{
    register const unsigned char *p;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(source != NULL);

    /*
     * RFC 7230 https://tools.ietf.org/html/rfc7230#section-3.2.6
     * defines a quoted string as
     *
     *  quoted-string = DQUOTE *( qdtext / quoted-pair ) DQUOTE
     *  qdtext         = HTAB / SP / %x21 / %x23-5B / %x5D-7E / obs-text
     *  obs-text       = %x80-FF
     */

    p = source;
    if (*p == '"') {
        static const bool qdtext_char[] = {
            /*          0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
            /* 0x00 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
            /* 0x10 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            /* 0x20 */  1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0x30 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0x40 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0x50 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1,
            /* 0x60 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0x70 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
            /* 0x80 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0x90 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0xa0 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0xb0 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0xc0 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0xd0 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0xe0 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            /* 0xf0 */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
        };
        bool qpair = NS_FALSE;

        for (p++; ; p++) {
            if (qdtext_char[*p]) {
                Tcl_DStringAppend(dsPtr, (const char *)p, 1);
            } else if (!qpair && *p == '\\') {
                qpair = NS_TRUE;
            } else if (qpair && (*p == 0x09 || *p>10)) {
                qpair = NS_FALSE;
                Tcl_DStringAppend(dsPtr, (const char *)p, 1);
            } else if  (*p == '"') {
                p++;
                break;
            } else {
                Ns_Log(Warning, "Unexpected character %c in header field <%s>",
                       *p, (char *)source);
                break;
            }
        }
    }
    return p;
}

/*
 *----------------------------------------------------------------------
 *
 * SkipWhitespace --
 *
 *      Skip pver whitespace in the provided input string.
 *
 * Results:
 *      Pointer to the character after the white space.  In case, the source
 *      pointer is the same as the result, no white space was skipped.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static const unsigned char *
SkipWhitespace(const unsigned char *source) {
    NS_NONNULL_ASSERT(source != NULL);

    for (; (CHARTYPE(space, *source) != 0); source++) {
    }
    return source;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclParseFieldvalue --
 *
 *      Parse one or multiple token value pairse from the content of the
 *      request header fields. These elements have typically the form:
 *
 *         elements = element *( OWS "," OWS element )
 *         element  = [ pair ] *( ";" [ pair ] )
 *         pair     = token "=" value
 *         value    = token / quoted-string
 *
 *      Per default this function returns a Tcl list of elements, where every
 *      element has the form of a Tcl dict, containing values from the "pair"
 *      definitions.  When the option "-single" is specified, the first
 *      "element" is parsed and returned as a single dict.
 *
 *      Implements "ns_parsefieldvalue".
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
NsTclParseFieldvalue(ClientData UNUSED(clientData), Tcl_Interp *interp,
                      TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK, singleInt = (int)NS_FALSE,
                 lowerInt = (int)NS_FALSE, strictInt = (int)NS_FALSE;
    char        *sourceString;
    Ns_ObjvSpec  opts[] = {
        {"-lower",  Ns_ObjvBool, &lowerInt, INT2PTR(NS_TRUE)},
        {"-single", Ns_ObjvBool, &singleInt, INT2PTR(NS_TRUE)},
        {"-strict", Ns_ObjvBool, &strictInt, INT2PTR(NS_TRUE)},
        {"--",      Ns_ObjvBreak,  NULL,     NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"fieldvalue", Ns_ObjvString, &sourceString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Tcl_DString          token, value;
        const unsigned char *p1;
        const char           listDelimiter = (singleInt == (int)NS_TRUE ? '\0' : ',');
        const char           sublistDelimiter = ';';
        Tcl_Obj             *listObj = NULL, *sublistObj = NULL;

        Tcl_DStringInit(&token);
        Tcl_DStringInit(&value);
        p1 = SkipWhitespace((unsigned char *)sourceString);

        for (;;) {
            const unsigned char *p2;

            p1 = SkipWhitespace(p1);
            p2 = GetToken(&token, p1);
            if (p1 == p2) {
                /*
                 * Silently skip tokens without names.
                 */
                p1 ++;
                continue;
            }

            p1 = SkipWhitespace(p2);
            if (*p1 == '=') {
                p1 ++;
                p1 = SkipWhitespace(p1);
                p2 = GetToken(&value, p1);
                if (p1 == p2) {
                    p2 = GetQuotedString(&value, p1);
                }
                p1 = SkipWhitespace(p2);
            }

            if (sublistObj == NULL) {
                sublistObj = Tcl_NewListObj(0, NULL);
            }
            if (lowerInt == (int)NS_TRUE) {
                Ns_StrToLower(token.string);
            }
            Tcl_ListObjAppendElement(interp, sublistObj, Tcl_NewStringObj(token.string, token.length));
            Tcl_ListObjAppendElement(interp, sublistObj, Tcl_NewStringObj(value.string, value.length));
            Tcl_DStringSetLength(&token, 0);
            Tcl_DStringSetLength(&value, 0);

            if (*p1 == sublistDelimiter) {
                p1 ++;
                /*
                 * Continue with sublist.
                 */
            } else if (*p1 != '\0' && *p1 == listDelimiter) {
                p1 ++;
                if (listObj == NULL) {
                    listObj = Tcl_NewListObj(0, NULL);
                }
                Tcl_ListObjAppendElement(interp, listObj, sublistObj);
                sublistObj = NULL;
                /*
                 * Continue with list.
                 */
            } else {
                break;
            }
        }
        if (listObj != NULL) {
            /*
             * We have a list; if there is an unhandled sublist, append it.
             */
            if (sublistObj != NULL) {
                Tcl_ListObjAppendElement(interp, listObj, sublistObj);
                sublistObj = NULL;
            }
        } else if (sublistObj != NULL) {
            /*
             * We have just a sublist, but no list. In "-single" mode, return
             * just the dict, otherwise return always a list of dicts.
             */
            if (singleInt == (int)NS_TRUE) {
                listObj = sublistObj;
            } else {
                listObj = Tcl_NewListObj(0, NULL);
                Tcl_ListObjAppendElement(interp, listObj, sublistObj);
            }
        }
        if (strictInt == (int)NS_TRUE && *p1 != '\0') {
            Ns_TclPrintfResult(interp, "unparsed content '%s'", p1);
            result = TCL_ERROR;
            if (listObj != NULL) {
                Tcl_DecrRefCount(listObj);
            }

        } else if (listObj != NULL) {
            Tcl_SetObjResult(interp, listObj);
        }
        Tcl_DStringFree(&token);
        Tcl_DStringFree(&value);
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
