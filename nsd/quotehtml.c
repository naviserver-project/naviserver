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
 * quotehtml.c --
 *
 *      Take text and make it safe for HTML.
 */

#include "nsd.h"

/*
 * Static functions defined in this file.
 */
static void QuoteHtml(Ns_DString *dsPtr, const char *breakChar, const char *htmlString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static bool WordEndsInSemi(const char *word, size_t *lengthPtr)
    NS_GNUC_NONNULL(1);

static int ToUTF8(long value, char *outPtr)
    NS_GNUC_NONNULL(2);

static size_t EntityDecode(const char *entity, size_t length, bool *needEncodePtr, char *outPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);



/*
 *----------------------------------------------------------------------
 *
 * Ns_QuoteHtml --
 *
 *      Quote an HTML string.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Copies quoted HTML to given dstring.
 *
 *----------------------------------------------------------------------
 */
static void
QuoteHtml(Ns_DString *dsPtr, const char *breakChar, const char *htmlString)
{
    const char *toProcess = htmlString;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(breakChar != NULL);
    NS_NONNULL_ASSERT(htmlString != NULL);

    do {
        /*
         * Append the first part, escape the protected char, and
         * continue.
         */
        Ns_DStringNAppend(dsPtr, toProcess, (int)(breakChar - toProcess));
        switch (*breakChar) {
        case '<':
            Ns_DStringNAppend(dsPtr, "&lt;", 4);
            break;

        case '>':
            Ns_DStringNAppend(dsPtr, "&gt;", 4);
            break;

        case '&':
            Ns_DStringNAppend(dsPtr, "&amp;", 5);
            break;

        case '\'':
            Ns_DStringNAppend(dsPtr, "&#39;", 5);
            break;

        case '"':
            Ns_DStringNAppend(dsPtr, "&#34;", 5);
            break;

        default:
            /*should not happen */ assert(0);
            break;
        }
        /*
         * Check for further protected characters.
         */
        toProcess = breakChar + 1;
        breakChar = strpbrk(toProcess, "<>&'\"");

    } while (breakChar != NULL);

    /*
     * Append the last part if nonempty.
     */
    if (toProcess != NULL) {
        Ns_DStringAppend(dsPtr, toProcess);
    }
}


void
Ns_QuoteHtml(Ns_DString *dsPtr, const char *htmlString)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(htmlString != NULL);

    /*
     * If the first character is a null character, there is nothing to do.
     */
    if (*htmlString != '\0') {
        const char *breakChar = strpbrk(htmlString, "<>&'\"");

        if (breakChar != NULL) {
            QuoteHtml(dsPtr, strpbrk(htmlString, "<>&'\""), htmlString);
        } else {
            Ns_DStringAppend(dsPtr, htmlString);
        }
    }
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclQuoteHtmlObjCmd --
 *
 *      Implements "ns_quotehtml".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclQuoteHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    Tcl_Obj     *htmlObj;
    Ns_ObjvSpec  args[] = {
        {"html", Ns_ObjvObj,  &htmlObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        const char *htmlString = Tcl_GetString(htmlObj);

        if (*htmlString != '\0') {
            const char *breakChar = strpbrk(htmlString, "<>&'\"");

            if (breakChar == NULL) {
                /*
                 * No need to copy anything.
                 */
                Tcl_SetObjResult(interp, htmlObj);
            } else {
                Ns_DString ds;

                Ns_DStringInit(&ds);
                QuoteHtml(&ds, breakChar, htmlString);
                Tcl_DStringResult(interp, &ds);

            }
        }
    }

    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclUnquoteHtmlObjCmd --
 *
 *      This is essentially the opposite operation of NsTclQuoteHtmlObjCmd.
 *
 *      Implements "ns_unquotehtml".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclUnquoteHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    Tcl_Obj     *htmlObj;
    Ns_ObjvSpec  args[] = {
        {"html", Ns_ObjvObj,  &htmlObj, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_DString  ds, *dsPtr = &ds;
        const char *htmlString = Tcl_GetString(htmlObj);
        bool        needEncode = NS_FALSE;

        Ns_DStringInit(&ds);

        if (*htmlString != '\0') {

            for (;;) {
                const char *possibleEntity = strchr(htmlString, '&');

                if (possibleEntity == NULL) {
                    /*
                     * We are done.
                     */
                    break;

                } else {
                    size_t length = 0u;
                    int prefixLength = (int)(possibleEntity - htmlString);

                    /*
                     * Add the string leading to the ampersand to the output
                     * and proceed in the string by this amount of bytes.
                     */
                    if (possibleEntity != htmlString) {
                        Ns_DStringNAppend(dsPtr, htmlString, prefixLength);
                        htmlString += prefixLength;
                    }

                    if (WordEndsInSemi(possibleEntity, &length)) {
                        size_t decoded;
                        int    oldLength = dsPtr->length;

                        /*
                         * The appended characters are max 4 bytes; make sure, we
                         * have this space in the Tcl_DString.
                         */
                        Tcl_DStringSetLength(dsPtr, oldLength + 4);
                        decoded = EntityDecode(possibleEntity + 1u, length, &needEncode,
                                               dsPtr->string + oldLength);
                        Tcl_DStringSetLength(dsPtr, oldLength + (int)decoded);

                        /*
                         * Include the boundary characters "&" and ";" in the
                         * length calculation.
                         */
                        htmlString += (length + 2);
                    } else {
                        Ns_DStringNAppend(dsPtr, "&", 1);
                        htmlString ++;
                    }
                }
            }

            /*
             * Append the last chunk
             */
            Ns_DStringNAppend(dsPtr, htmlString, -1);

        }

        if (needEncode) {
            Tcl_DString ds2;

            (void)Tcl_ExternalToUtfDString(Ns_GetCharsetEncoding("utf-8"),
                                           dsPtr->string, dsPtr->length, &ds2);
            Tcl_DStringResult(interp, &ds2);
            Tcl_DStringFree(dsPtr);

        } else {
            Tcl_DStringResult(interp, dsPtr);
        }
    }

    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ToUTF8 --
 *
 *      Convert a unicode code point to UTF8. The function writes from 0 up to
 *      4 bytes to the output.
 *
 * Results:
 *      Returns number of bytes written to the output. The value of 0 means
 *      invalid input.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
ToUTF8(long value, char *outPtr)
{
    int length = 0;

    NS_NONNULL_ASSERT(outPtr != NULL);

    if(value <= 0x7F) {
        *outPtr = (char)value;
        length = 1;

    } else if (value <= 0x7FF) {
        *outPtr++ = (char)(((value >> 6) & 0x1F) | 0xC0);
        *outPtr++ = (char)(((value >> 0) & 0x3F) | 0x80);
        length = 2;

    } else if (value <= 0xFFFF) {
        *outPtr++ = (char) (((value >> 12) & 0x0F) | 0xE0);
        *outPtr++ = (char) (((value >>  6) & 0x3F) | 0x80);
        *outPtr++ = (char) (((value >>  0) & 0x3F) | 0x80);
        length = 3;

    } else if (value <= 0x10FFFF) {
        *outPtr++ = (char) (((value >> 18) & 0x07) | 0xF0);
        *outPtr++ = (char) (((value >> 12) & 0x3F) | 0x80);
        *outPtr++ = (char) (((value >>  6) & 0x3F) | 0x80);
        *outPtr++ = (char) (((value >>  0) & 0x3F) | 0x80);
        length = 4;
    } else {
        length = 0;
    }
    return length;
}


/*
 *----------------------------------------------------------------------
 *
 * EntityDecode --
 *
 *      Decode an HTML/XML entity, which might be numeric (starting with a '#'
 *      sign) or non-numeric.
 *
 * Results:
 *      Number of decoded characters.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

typedef struct namedEntity_t {
    const char *name;
    size_t length;
    const char *value;
    size_t outputLength;
} namedEntity_t;

static const namedEntity_t namedEntities[] = {
    {"AElig",    5, "\xc3\x86",     2},    /* "Æ" */
    {"Aacute",   6, "\xc3\x81",     2},    /* "Á" */
    {"Acirc",    5, "\xc3\x82",     2},    /* "Â" */
    {"Agrave",   6, "\xc3\x80",     2},    /* "À" */
    {"Alpha",    5, "\xce\x91",     2},    /* "Α" */
    {"Aring",    5, "\xc3\x85",     2},    /* "Å" */
    {"Atilde",   6, "\xc3\x83",     2},    /* "Ã" */
    {"Auml",     4, "\xc3\x84",     2},    /* "Ä" */
    {"Beta",     4, "\xce\x92",     2},    /* "Β" */
    {"Ccedil",   6, "\xc3\x87",     2},    /* "Ç" */
    {"Chi",      3, "\xce\xa7",     2},    /* "Χ" */
    {"Delta",    5, "\xce\x94",     2},    /* "Δ" */
    {"ETH",      3, "\xc3\x90",     2},    /* "Ð" */
    {"Eacute",   6, "\xc3\x89",     2},    /* "É" */
    {"Ecirc",    5, "\xc3\x8a",     2},    /* "Ê" */
    {"Egrave",   6, "\xc3\x88",     2},    /* "È" */
    {"Epsilon",  7, "\xce\x95",     2},    /* "Ε" */
    {"Eta",      3, "\xce\x97",     2},    /* "Η" */
    {"Euml",     4, "\xc3\x8b",     2},    /* "Ë" */
    {"Gamma",    5, "\xce\x93",     2},    /* "Γ" */
    {"Iacute",   6, "\xc3\x8d",     2},    /* "Í" */
    {"Icirc",    5, "\xc3\x8e",     2},    /* "Î" */
    {"Igrave",   6, "\xc3\x8c",     2},    /* "Ì" */
    {"Iota",     4, "\xce\x99",     2},    /* "Ι" */
    {"Iuml",     4, "\xc3\x8f",     2},    /* "Ï" */
    {"Kappa",    5, "\xce\x9a",     2},    /* "Κ" */
    {"Lambda",   6, "\xce\x9b",     2},    /* "Λ" */
    {"Mu",       2, "\xce\x9c",     2},    /* "Μ" */
    {"Ntilde",   6, "\xc3\x91",     2},    /* "Ñ" */
    {"Nu",       2, "\xce\x9d",     2},    /* "Ν" */
    {"Oacute",   6, "\xc3\x93",     2},    /* "Ó" */
    {"Ocirc",    5, "\xc3\x94",     2},    /* "Ô" */
    {"Ograve",   6, "\xc3\x92",     2},    /* "Ò" */
    {"Omega",    5, "\xce\xa9",     2},    /* "Ω" */
    {"Omicron",  7, "\xce\x9f",     2},    /* "Ο" */
    {"Oslash",   6, "\xc3\x98",     2},    /* "Ø" */
    {"Otilde",   6, "\xc3\x95",     2},    /* "Õ" */
    {"Ouml",     4, "\xc3\x96",     2},    /* "Ö" */
    {"Phi",      3, "\xce\xa6",     2},    /* "Φ" */
    {"Pi",       2, "\xce\xa0",     2},    /* "Π" */
    {"Prime",    5, "\xe2\x80\xb3", 3},    /* "″" */
    {"Psi",      3, "\xce\xa8",     2},    /* "Ψ" */
    {"Rho",      3, "\xce\xa1",     2},    /* "Ρ" */
    {"Sigma",    5, "\xce\xa3",     2},    /* "Σ" */
    {"THORN",    5, "\xc3\x9e",     2},    /* "Þ" */
    {"Tau",      3, "\xce\xa4",     2},    /* "Τ" */
    {"Theta",    5, "\xce\x98",     2},    /* "Θ" */
    {"Uacute",   6, "\xc3\x9a",     2},    /* "Ú" */
    {"Ucirc",    5, "\xc3\x9b",     2},    /* "Û" */
    {"Ugrave",   6, "\xc3\x99",     2},    /* "Ù" */
    {"Upsilon",  7, "\xce\xa5",     2},    /* "Υ" */
    {"Uuml",     4, "\xc3\x9c",     2},    /* "Ü" */
    {"Xi",       2, "\xce\x9e",     2},    /* "Ξ" */
    {"Yacute",   6, "\xc3\x9d",     2},    /* "Ý" */
    {"Zeta",     4, "\xce\x96",     2},    /* "Ζ" */
    {"aacute",   6, "\xc3\xa1",     2},    /* "á" */
    {"acirc",    5, "\xc3\xa2",     2},    /* "â" */
    {"acute",    5, "\xc2\xb4",     2},    /* "´" */
    {"aelig",    5, "\xc3\xa6",     2},    /* "æ" */
    {"agrave",   6, "\xc3\xa0",     2},    /* "à" */
    {"alefsym",  7, "\xe2\x84\xb5", 3},    /* "ℵ" */
    {"alpha",    5, "\xce\xb1",     2},    /* "α" */
    {"amp",      3, "\x26",         1},    /* "&" */
    {"and",      3, "\xe2\x88\xa7", 3},    /* "∧" */
    {"ang",      3, "\xe2\x88\xa0", 3},    /* "∠" */
    {"aring",    5, "\xc3\xa5",     2},    /* "å" */
    {"asymp",    5, "\xe2\x89\x88", 3},    /* "≈" */
    {"atilde",   6, "\xc3\xa3",     2},    /* "ã" */
    {"auml",     4, "\xc3\xa4",     2},    /* "ä" */
    {"beta",     4, "\xce\xb2",     2},    /* "β" */
    {"brvbar",   6, "\xc2\xa6",     2},    /* "¦" */
    {"bull",     4, "\xe2\x80\xa2", 3},    /* "•" */
    {"cap",      3, "\xe2\x88\xa9", 3},    /* "∩" */
    {"ccedil",   6, "\xc3\xa7",     2},    /* "ç" */
    {"cedil",    5, "\xc2\xb8",     2},    /* "¸" */
    {"cent",     4, "\xc2\xa2",     2},    /* "¢" */
    {"chi",      3, "\xcf\x87",     2},    /* "χ" */
    {"clubs",    5, "\xe2\x99\xa3", 3},    /* "♣" */
    {"cong",     4, "\xe2\x89\x85", 3},    /* "≅" */
    {"copy",     4, "\xc2\xa9",     2},    /* "©" */
    {"crarr",    5, "\xe2\x86\xb5", 3},    /* "↵" */
    {"cup",      3, "\xe2\x88\xaa", 3},    /* "∪" */
    {"curren",   6, "\xc2\xa4",     2},    /* "¤" */
    {"dArr",     4, "\xe2\x87\x93", 3},    /* "⇓" */
    {"darr",     4, "\xe2\x86\x93", 3},    /* "↓" */
    {"deg",      3, "\xc2\xb0",     2},    /* "°" */
    {"delta",    5, "\xce\xb4",     2},    /* "δ" */
    {"diams",    5, "\xe2\x99\xa6", 3},    /* "♦" */
    {"divide",   6, "\xc3\xb7",     2},    /* "÷" */
    {"eacute",   6, "\xc3\xa9",     2},    /* "é" */
    {"ecirc",    5, "\xc3\xaa",     2},    /* "ê" */
    {"egrave",   6, "\xc3\xa8",     2},    /* "è" */
    {"empty",    5, "\xe2\x88\x85", 3},    /* "∅" */
    {"epsilon",  7, "\xce\xb5",     2},    /* "ε" */
    {"equiv",    5, "\xe2\x89\xa1", 3},    /* "≡" */
    {"eta",      3, "\xce\xb7",     2},    /* "η" */
    {"eth",      3, "\xc3\xb0",     2},    /* "ð" */
    {"euml",     4, "\xc3\xab",     2},    /* "ë" */
    {"euro",     4, "\xe2\x82\xac", 3},    /* "€" */
    {"exist",    5, "\xe2\x88\x83", 3},    /* "∃" */
    {"fnof",     4, "\xc6\x92",     2},    /* "ƒ" */
    {"forall",   6, "\xe2\x88\x80", 3},    /* "∀" */
    {"frac12",   6, "\xc2\xbd",     2},    /* "½" */
    {"frac14",   6, "\xc2\xbc",     2},    /* "¼" */
    {"frac34",   6, "\xc2\xbe",     2},    /* "¾" */
    {"frasl",    5, "\xe2\x81\x84", 3},    /* "⁄" */
    {"gamma",    5, "\xce\xb3",     2},    /* "γ" */
    {"ge",       2, "\xe2\x89\xa5", 3},    /* "≥" */
    {"gt",       2, "\x3e",         1},    /* ">" */
    {"hArr",     4, "\xe2\x87\x94", 3},    /* "⇔" */
    {"harr",     4, "\xe2\x86\x94", 3},    /* "↔" */
    {"hearts",   6, "\xe2\x99\xa5", 3},    /* "♥" */
    {"hellip",   6, "\xe2\x80\xa6", 3},    /* "…" */
    {"iacute",   6, "\xc3\xad",     2},    /* "í" */
    {"icirc",    5, "\xc3\xae",     2},    /* "î" */
    {"iexcl",    5, "\xc2\xa1",     2},    /* "¡" */
    {"igrave",   6, "\xc3\xac",     2},    /* "ì" */
    {"image",    5, "\xe2\x84\x91", 3},    /* "ℑ" */
    {"infin",    5, "\xe2\x88\x9e", 3},    /* "∞" */
    {"int",      3, "\xe2\x88\xab", 3},    /* "∫" */
    {"iota",     4, "\xce\xb9",     2},    /* "ι" */
    {"iquest",   6, "\xc2\xbf",     2},    /* "¿" */
    {"isin",     4, "\xe2\x88\x88", 3},    /* "∈" */
    {"iuml",     4, "\xc3\xaf",     2},    /* "ï" */
    {"kappa",    5, "\xce\xba",     2},    /* "κ" */
    {"lArr",     4, "\xe2\x87\x90", 3},    /* "⇐" */
    {"lambda",   6, "\xce\xbb",     2},    /* "λ" */
    {"lang",     4, "\xe3\x80\x88", 3},    /* "〈" */
    {"laquo",    5, "\xc2\xab",     2},    /* "«" */
    {"larr",     4, "\xe2\x86\x90", 3},    /* "←" */
    {"lceil",    5, "\xe2\x8c\x88", 3},    /* "⌈" */
    {"le",       2, "\xe2\x89\xa4", 3},    /* "≤" */
    {"lfloor",   6, "\xe2\x8c\x8a", 3},    /* "⌊" */
    {"lowast",   6, "\xe2\x88\x97", 3},    /* "∗" */
    {"loz",      3, "\xe2\x97\x8a", 3},    /* "◊" */
    {"lt",       2, "\x3c",         1},    /* "<" */
    {"macr",     4, "\xc2\xaf",     2},    /* "¯" */
    {"micro",    5, "\xc2\xb5",     2},    /* "µ" */
    {"middot",   6, "\xc2\xb7",     2},    /* "·" */
    {"minus",    5, "\xe2\x88\x92", 3},    /* "−" */
    {"mu",       2, "\xce\xbc",     2},    /* "μ" */
    {"nabla",    5, "\xe2\x88\x87", 3},    /* "∇" */
    {"nbsp",     4, "\x20",         1},    /* " " */
    {"ne",       2, "\xe2\x89\xa0", 3},    /* "≠" */
    {"ni",       2, "\xe2\x88\x8b", 3},    /* "∋" */
    {"not",      3, "\xc2\xac",     2},    /* "¬" */
    {"notin",    5, "\xe2\x88\x89", 3},    /* "∉" */
    {"nsub",     4, "\xe2\x8a\x84", 3},    /* "⊄" */
    {"ntilde",   6, "\xc3\xb1",     2},    /* "ñ" */
    {"nu",       2, "\xce\xbd",     2},    /* "ν" */
    {"oacute",   6, "\xc3\xb3",     2},    /* "ó" */
    {"ocirc",    5, "\xc3\xb4",     2},    /* "ô" */
    {"ograve",   6, "\xc3\xb2",     2},    /* "ò" */
    {"oline",    5, "\xe2\x80\xbe", 3},    /* "‾" */
    {"omega",    5, "\xcf\x89",     2},    /* "ω" */
    {"omicron",  7, "\xce\xbf",     2},    /* "ο" */
    {"oplus",    5, "\xe2\x8a\x95", 3},    /* "⊕" */
    {"or",       2, "\xe2\x88\xa8", 3},    /* "∨" */
    {"ordf",     4, "\xc2\xaa",     2},    /* "ª" */
    {"ordm",     4, "\xc2\xba",     2},    /* "º" */
    {"oslash",   6, "\xc3\xb8",     2},    /* "ø" */
    {"otilde",   6, "\xc3\xb5",     2},    /* "õ" */
    {"otimes",   6, "\xe2\x8a\x97", 3},    /* "⊗" */
    {"ouml",     4, "\xc3\xb6",     2},    /* "ö" */
    {"para",     4, "\xc2\xb6",     2},    /* "¶" */
    {"part",     4, "\xe2\x88\x82", 3},    /* "∂" */
    {"perp",     4, "\xe2\x8a\xa5", 3},    /* "⊥" */
    {"phi",      3, "\xcf\x86",     2},    /* "φ" */
    {"pi",       2, "\xcf\x80",     2},    /* "π" */
    {"piv",      3, "\xcf\x96",     2},    /* "ϖ" */
    {"plusmn",   6, "\xc2\xb1",     2},    /* "±" */
    {"pound",    5, "\xc2\xa3",     2},    /* "£" */
    {"prime",    5, "\xe2\x80\xb2", 3},    /* "′" */
    {"prod",     4, "\xe2\x88\x8f", 3},    /* "∏" */
    {"prop",     4, "\xe2\x88\x9d", 3},    /* "∝" */
    {"psi",      3, "\xcf\x88",     2},    /* "ψ" */
    {"rArr",     4, "\xe2\x87\x92", 3},    /* "⇒" */
    {"radic",    5, "\xe2\x88\x9a", 3},    /* "√" */
    {"rang",     4, "\xe3\x80\x89", 3},    /* "〉" */
    {"raquo",    5, "\xc2\xbb",     2},    /* "»" */
    {"rarr",     4, "\xe2\x86\x92", 3},    /* "→" */
    {"rceil",    5, "\xe2\x8c\x89", 3},    /* "⌉" */
    {"real",     4, "\xe2\x84\x9c", 3},    /* "ℜ" */
    {"reg",      3, "\xc2\xae",     2},    /* "®" */
    {"rfloor",   6, "\xe2\x8c\x8b", 3},    /* "⌋" */
    {"rho",      3, "\xcf\x81",     2},    /* "ρ" */
    {"sdot",     4, "\xe2\x8b\x85", 3},    /* "⋅" */
    {"sect",     4, "\xc2\xa7",     2},    /* "§" */
    {"shy",      3, "\xc2\xad",     2},    /* "­" */
    {"sigma",    5, "\xcf\x83",     2},    /* "σ" */
    {"sigmaf",   6, "\xcf\x82",     2},    /* "ς" */
    {"sim",      3, "\xe2\x88\xbc", 3},    /* "∼" */
    {"spades",   6, "\xe2\x99\xa0", 3},    /* "♠" */
    {"sub",      3, "\xe2\x8a\x82", 3},    /* "⊂" */
    {"sube",     4, "\xe2\x8a\x86", 3},    /* "⊆" */
    {"sum",      3, "\xe2\x88\x91", 3},    /* "∑" */
    {"sup",      3, "\xe2\x8a\x83", 3},    /* "⊃" */
    {"sup1",     4, "\xc2\xb9",     2},    /* "¹" */
    {"sup2",     4, "\xc2\xb2",     2},    /* "²" */
    {"sup3",     4, "\xc2\xb3",     2},    /* "³" */
    {"supe",     4, "\xe2\x8a\x87", 3},    /* "⊇" */
    {"szlig",    5, "\xc3\x9f",     2},    /* "ß" */
    {"tau",      3, "\xcf\x84",     2},    /* "τ" */
    {"there4",   6, "\xe2\x88\xb4", 3},    /* "∴" */
    {"theta",    5, "\xce\xb8",     2},    /* "θ" */
    {"thetasym", 8, "\xcf\x91",     2},    /* "ϑ" */
    {"thorn",    5, "\xc3\xbe",     2},    /* "þ" */
    {"times",    5, "\xc3\x97",     2},    /* "×" */
    {"trade",    5, "\xe2\x84\xa2", 3},    /* "™" */
    {"uArr",     4, "\xe2\x87\x91", 3},    /* "⇑" */
    {"uacute",   6, "\xc3\xba",     2},    /* "ú" */
    {"uarr",     4, "\xe2\x86\x91", 3},    /* "↑" */
    {"ucirc",    5, "\xc3\xbb",     2},    /* "û" */
    {"ugrave",   6, "\xc3\xb9",     2},    /* "ù" */
    {"uml",      3, "\xc2\xa8",     2},    /* "¨" */
    {"upsih",    5, "\xcf\x92",     2},    /* "ϒ" */
    {"upsilon",  7, "\xcf\x85",     2},    /* "υ" */
    {"uuml",     4, "\xc3\xbc",     2},    /* "ü" */
    {"weierp",   6, "\xe2\x84\x98", 3},    /* "℘" */
    {"xi",       2, "\xce\xbe",     2},    /* "ξ" */
    {"yacute",   6, "\xc3\xbd",     2},    /* "ý" */
    {"yen",      3, "\xc2\xa5",     2},    /* "¥" */
    {"yuml",     4, "\xc3\xbf",     2},    /* "ÿ" */
    {"zeta",     4, "\xce\xb6",     2},    /* "ζ" */
    {NULL,       0, "",             0}
};


static size_t
EntityDecode(const char *entity, size_t length, bool *needEncodePtr, char *outPtr)
{
    size_t decoded = 0u;

    NS_NONNULL_ASSERT(entity != NULL);
    NS_NONNULL_ASSERT(output != NULL);
    NS_NONNULL_ASSERT(needEncodePtr != NULL);

    /*
     * Handle numeric entities.
     */
    if (*entity == '#') {
        long value;

        if (CHARTYPE(digit, *(entity + 1)) != 0) {
            /*
             * Decimal numeric entity.
             */
            value = strtol(entity + 1, NULL, 10);

        } else if (*(entity + 1) == 'x' && length >= 3 && length <= 8) {
            /*
             * Hexadecimal numeric entity.
             */
            value = strtol(entity + 2, NULL, 16);

        } else {
            Ns_Log(Warning, "invalid numeric entity: '%s'", entity);
            value = 0;
        }

        if (value >= 32) {
            int outLength;

            outLength = ToUTF8(value, outPtr);
            decoded += (size_t)outLength;

            Ns_Log(Debug, "entity decode: code point %.2lx %.2lx "
                   "corresponds to %d UTF-8 characters",
                   ((value >> 8) & 0xff), (value & 0xff), outLength);

            if (value > 127) {
                *needEncodePtr = NS_TRUE;
            }
        } else {
            /*
             * ASCII device control characters should not be present in HTML.
             */
            Ns_Log(Notice, "entity decode: ignore numeric entity with value %ld", value);
        }
    } else {
        size_t i;

        for (i = 0; namedEntities[i].name != NULL; i++) {
            char firstChar = *namedEntities[i].name;

            if (firstChar == *entity
                && length == namedEntities[i].length
                && strncmp(entity, namedEntities[i].name, length) == 0) {

                /*if (strlen(entities[i].value) != entities[i].outputLength) {
                  fprintf(stderr, "--> name %s found l = %lu\n",
                  entities[i].name, strlen(entities[i].value));
                  }*/
                if (namedEntities[i].outputLength > 1) {

                    memcpy(outPtr, namedEntities[i].value, namedEntities[i].outputLength);
                    decoded += namedEntities[i].outputLength;
                } else {
                    *outPtr = *namedEntities[i].value;
                    decoded++;
                }
                break;
            }

            if (firstChar > *entity) {
                Ns_Log(Warning, "ignore unknown named entity '%s'", entity);
                break;
            }
        }
    }

    return decoded;
}


/*
 *----------------------------------------------------------------------
 *
 * WordEndsInSemi --
 *
 *      Does this word end in a semicolon or a space?
 *
 * Results:
 *      Returns true if the word endes with a semicolon.
 *
 * Side effects:
 *      Undefined behavior if string does not end in null
 *
 *----------------------------------------------------------------------
 */

static bool
WordEndsInSemi(const char *word, size_t *lengthPtr)
{
    const char *start;

    NS_NONNULL_ASSERT(word != NULL);

    /*
     * Advance past the first '&' so we can check for a second
     *  (i.e. to handle "ben&jerry&nbsp;")
     */
    if (*word == '&') {
        word++;
    }
    start = word;
    while((*word != '\0') && (*word != ' ') && (*word != ';') && (*word != '&')) {
        word++;
    }
    *lengthPtr = (size_t)(word - start);

    return (*word == ';');
}



/*
 *----------------------------------------------------------------------
 *
 * NsTclStripHtmlObjCmd --
 *
 *      Implements "ns_striphtml".
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclStripHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK;
    char        *htmlString = (char *)NS_EMPTY_STRING;
    Ns_ObjvSpec  args[] = {
        {"html", Ns_ObjvString,  &htmlString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        bool        intag;     /* flag to see if are we inside a tag */
        bool        incomment; /* flag to see if we are inside a comment */
        char       *inString;  /* copy of input string */
        char       *outPtr;    /* moving pointer to output string */
        const char *inPtr;     /* moving pointer to input string */
        bool        needEncode;

        /*
         * Make a copy of the input and point the moving and output ptrs to it.
         */
        inString   = ns_strdup(htmlString);
        inPtr      = inString;
        outPtr     = inString;
        intag      = NS_FALSE;
        incomment  = NS_FALSE;
        needEncode = NS_FALSE;

        while (*inPtr != '\0') {

            Ns_Log(Debug, "inptr %c intag %d incomment %d string <%s>",
                   *inPtr, intag, incomment, inPtr);

            if (*inPtr == '<') {
                intag = NS_TRUE;
                if ((*(inPtr + 1) == '!')
                    && (*(inPtr + 2) == '-')
                    && (*(inPtr + 3) == '-')) {
                    incomment = NS_TRUE;
                }
            } else if (incomment) {
                if ((*(inPtr) == '-')
                    && (*(inPtr + 1) == '-')
                    && (*(inPtr + 2) == '>')) {
                    incomment  = NS_FALSE;
                }
            } else if (intag && (*inPtr == '>')) {
                /*
                 * Closing a tag.
                 */
                intag = NS_FALSE;

            } else if (!intag) {
                /*
                 * Regular text
                 */

                if (*inPtr == '&') {
                    size_t length = 0u;

                    /*
                     * Starting an entity.
                     */
                    if (WordEndsInSemi(inPtr, &length)) {
                        size_t decoded = EntityDecode(inPtr + 1u, length, &needEncode, outPtr);

                        inPtr += (length + 1u);
                        outPtr += decoded;
                    }
                    Ns_Log(Debug, "...... after entity inptr '%c' intag %d incomment %d string <%s> needEncode %d",
                           *inPtr, intag, incomment, inPtr, needEncode);
                } else {
                    /*
                     * Plain Text output
                     */
                    *outPtr++ = *inPtr;
                }

            } else {
                /*
                 * Must be intag
                 */
            }
            ++inPtr;
        }

        /*
         * Terminate output string.
         */
        *outPtr = '\0';

        if (needEncode) {
            Tcl_DString ds;

            (void)Tcl_ExternalToUtfDString(Ns_GetCharsetEncoding("utf-8"),
                                           inString, (int)strlen(inString), &ds);
            Tcl_DStringResult(interp, &ds);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(inString, -1));
        }
        ns_free(inString);
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
