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

static void
HtmlFinishElement(Tcl_Obj *listObj, const char* what, const char *lastStart,
                  const char *currentPtr, bool noAngle,  bool onlyTags, Tcl_Obj *contentObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);
static Tcl_Obj *
HtmlParseTagAtts(const char *string, ptrdiff_t length)
    NS_GNUC_NONNULL(1);


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
        Ns_DStringNAppend(dsPtr, toProcess, (TCL_SIZE_T)(breakChar - toProcess));
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
NsTclQuoteHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
NsTclUnquoteHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
                    size_t     length = 0u;
                    TCL_SIZE_T prefixLength = (TCL_SIZE_T)(possibleEntity - htmlString);

                    /*
                     * Add the string leading to the ampersand to the output
                     * and proceed in the string by this amount of bytes.
                     */
                    if (possibleEntity != htmlString) {
                        Ns_DStringNAppend(dsPtr, htmlString, prefixLength);
                        htmlString += prefixLength;
                    }

                    if (WordEndsInSemi(possibleEntity, &length)) {
                        size_t     decoded;
                        TCL_SIZE_T oldLength = dsPtr->length;

                        /*
                         * The appended characters are max 4 bytes; make sure, we
                         * have this space in the Tcl_DString.
                         */
                        Tcl_DStringSetLength(dsPtr, oldLength + 4);
                        decoded = EntityDecode(possibleEntity + 1u, length, &needEncode,
                                               dsPtr->string + oldLength);
                        Tcl_DStringSetLength(dsPtr, oldLength + (TCL_SIZE_T)decoded);

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
            Ns_DStringNAppend(dsPtr, htmlString, TCL_INDEX_NONE);

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
 *      sign) or non-numeric. The named entity list contains the HTML5 named
 *      entities.
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
    {"AElig",                            5, "\xc3\x86",                    2},    /* "Æ" */
    {"AMP",                              3, "\x26",                        1},    /* "&" */
    {"Aacute",                           6, "\xc3\x81",                    2},    /* "Á" */
    {"Abreve",                           6, "\xc4\x82",                    2},    /* "Ă" */
    {"Acirc",                            5, "\xc3\x82",                    2},    /* "Â" */
    {"Acy",                              3, "\xd0\x90",                    2},    /* "А" */
    {"Afr",                              3, "\xf0\x9d\x94\x84",            4},    /* "𝔄" */
    {"Agrave",                           6, "\xc3\x80",                    2},    /* "À" */
    {"Alpha",                            5, "\xce\x91",                    2},    /* "Α" */
    {"Amacr",                            5, "\xc4\x80",                    2},    /* "Ā" */
    {"And",                              3, "\xe2\xa9\x93",                3},    /* "⩓" */
    {"Aogon",                            5, "\xc4\x84",                    2},    /* "Ą" */
    {"Aopf",                             4, "\xf0\x9d\x94\xb8",            4},    /* "𝔸" */
    {"ApplyFunction",                   13, "\xe2\x81\xa1",                3},    /* ApplyFunction */
    {"Aring",                            5, "\xc3\x85",                    2},    /* "Å" */
    {"Ascr",                             4, "\xf0\x9d\x92\x9c",            4},    /* "𝒜" */
    {"Assign",                           6, "\xe2\x89\x94",                3},    /* "≔" */
    {"Atilde",                           6, "\xc3\x83",                    2},    /* "Ã" */
    {"Auml",                             4, "\xc3\x84",                    2},    /* "Ä" */
    {"Backslash",                        9, "\xe2\x88\x96",                3},    /* "∖" */
    {"Barv",                             4, "\xe2\xab\xa7",                3},    /* "⫧" */
    {"Barwed",                           6, "\xe2\x8c\x86",                3},    /* "⌆" */
    {"Bcy",                              3, "\xd0\x91",                    2},    /* "Б" */
    {"Because",                          7, "\xe2\x88\xb5",                3},    /* "∵" */
    {"Bernoullis",                      10, "\xe2\x84\xac",                3},    /* "ℬ" */
    {"Beta",                             4, "\xce\x92",                    2},    /* "Β" */
    {"Bfr",                              3, "\xf0\x9d\x94\x85",            4},    /* "𝔅" */
    {"Bopf",                             4, "\xf0\x9d\x94\xb9",            4},    /* "𝔹" */
    {"Breve",                            5, "\xcb\x98",                    2},    /* "˘" */
    {"Bscr",                             4, "\xe2\x84\xac",                3},    /* "ℬ" */
    {"Bumpeq",                           6, "\xe2\x89\x8e",                3},    /* "≎" */
    {"CHcy",                             4, "\xd0\xa7",                    2},    /* "Ч" */
    {"COPY",                             4, "\xc2\xa9",                    2},    /* "©" */
    {"Cacute",                           6, "\xc4\x86",                    2},    /* "Ć" */
    {"Cap",                              3, "\xe2\x8b\x92",                3},    /* "⋒" */
    {"CapitalDifferentialD",            20, "\xe2\x85\x85",                3},    /* "ⅅ" */
    {"Cayleys",                          7, "\xe2\x84\xad",                3},    /* "ℭ" */
    {"Ccaron",                           6, "\xc4\x8c",                    2},    /* "Č" */
    {"Ccedil",                           6, "\xc3\x87",                    2},    /* "Ç" */
    {"Ccirc",                            5, "\xc4\x88",                    2},    /* "Ĉ" */
    {"Cconint",                          7, "\xe2\x88\xb0",                3},    /* "∰" */
    {"Cdot",                             4, "\xc4\x8a",                    2},    /* "Ċ" */
    {"Cedilla",                          7, "\xc2\xb8",                    2},    /* "¸" */
    {"CenterDot",                        9, "\xc2\xb7",                    2},    /* "·" */
    {"Cfr",                              3, "\xe2\x84\xad",                3},    /* "ℭ" */
    {"Chi",                              3, "\xce\xa7",                    2},    /* "Χ" */
    {"CircleDot",                        9, "\xe2\x8a\x99",                3},    /* "⊙" */
    {"CircleMinus",                     11, "\xe2\x8a\x96",                3},    /* "⊖" */
    {"CirclePlus",                      10, "\xe2\x8a\x95",                3},    /* "⊕" */
    {"CircleTimes",                     11, "\xe2\x8a\x97",                3},    /* "⊗" */
    {"ClockwiseContourIntegral",        24, "\xe2\x88\xb2",                3},    /* "∲" */
    {"CloseCurlyDoubleQuote",           21, "\xe2\x80\x9d",                3},    /* "”" */
    {"CloseCurlyQuote",                 15, "\xe2\x80\x99",                3},    /* "’" */
    {"Colon",                            5, "\xe2\x88\xb7",                3},    /* "∷" */
    {"Colone",                           6, "\xe2\xa9\xb4",                3},    /* "⩴" */
    {"Congruent",                        9, "\xe2\x89\xa1",                3},    /* "≡" */
    {"Conint",                           6, "\xe2\x88\xaf",                3},    /* "∯" */
    {"ContourIntegral",                 15, "\xe2\x88\xae",                3},    /* "∮" */
    {"Copf",                             4, "\xe2\x84\x82",                3},    /* "ℂ" */
    {"Coproduct",                        9, "\xe2\x88\x90",                3},    /* "∐" */
    {"CounterClockwiseContourIntegral", 31, "\xe2\x88\xb3",                3},    /* "∳" */
    {"Cross",                            5, "\xe2\xa8\xaf",                3},    /* "⨯" */
    {"Cscr",                             4, "\xf0\x9d\x92\x9e",            4},    /* "𝒞" */
    {"Cup",                              3, "\xe2\x8b\x93",                3},    /* "⋓" */
    {"CupCap",                           6, "\xe2\x89\x8d",                3},    /* "≍" */
    {"DD",                               2, "\xe2\x85\x85",                3},    /* "ⅅ" */
    {"DDotrahd",                         8, "\xe2\xa4\x91",                3},    /* "⤑" */
    {"DJcy",                             4, "\xd0\x82",                    2},    /* "Ђ" */
    {"DScy",                             4, "\xd0\x85",                    2},    /* "Ѕ" */
    {"DZcy",                             4, "\xd0\x8f",                    2},    /* "Џ" */
    {"Dagger",                           6, "\xe2\x80\xa1",                3},    /* "‡" */
    {"Darr",                             4, "\xe2\x86\xa1",                3},    /* "↡" */
    {"Dashv",                            5, "\xe2\xab\xa4",                3},    /* "⫤" */
    {"Dcaron",                           6, "\xc4\x8e",                    2},    /* "Ď" */
    {"Dcy",                              3, "\xd0\x94",                    2},    /* "Д" */
    {"Del",                              3, "\xe2\x88\x87",                3},    /* "∇" */
    {"Delta",                            5, "\xce\x94",                    2},    /* "Δ" */
    {"Dfr",                              3, "\xf0\x9d\x94\x87",            4},    /* "𝔇" */
    {"DiacriticalAcute",                16, "\xc2\xb4",                    2},    /* "´" */
    {"DiacriticalDot",                  14, "\xcb\x99",                    2},    /* "˙" */
    {"DiacriticalDoubleAcute",          22, "\xcb\x9d",                    2},    /* "˝" */
    {"DiacriticalGrave",                16, "\x60",                        1},    /* "`" */
    {"DiacriticalTilde",                16, "\xcb\x9c",                    2},    /* "˜" */
    {"Diamond",                          7, "\xe2\x8b\x84",                3},    /* "⋄" */
    {"DifferentialD",                   13, "\xe2\x85\x86",                3},    /* "ⅆ" */
    {"Dopf",                             4, "\xf0\x9d\x94\xbb",            4},    /* "𝔻" */
    {"Dot",                              3, "\xc2\xa8",                    2},    /* "¨" */
    {"DotDot",                           6, "\xe2\x83\x9c",                3},    /* "⃜" */
    {"DotEqual",                         8, "\xe2\x89\x90",                3},    /* "≐" */
    {"DoubleContourIntegral",           21, "\xe2\x88\xaf",                3},    /* "∯" */
    {"DoubleDot",                        9, "\xc2\xa8",                    2},    /* "¨" */
    {"DoubleDownArrow",                 15, "\xe2\x87\x93",                3},    /* "⇓" */
    {"DoubleLeftArrow",                 15, "\xe2\x87\x90",                3},    /* "⇐" */
    {"DoubleLeftRightArrow",            20, "\xe2\x87\x94",                3},    /* "⇔" */
    {"DoubleLeftTee",                   13, "\xe2\xab\xa4",                3},    /* "⫤" */
    {"DoubleLongLeftArrow",             19, "\xe2\x9f\xb8",                3},    /* "⟸" */
    {"DoubleLongLeftRightArrow",        24, "\xe2\x9f\xba",                3},    /* "⟺" */
    {"DoubleLongRightArrow",            20, "\xe2\x9f\xb9",                3},    /* "⟹" */
    {"DoubleRightArrow",                16, "\xe2\x87\x92",                3},    /* "⇒" */
    {"DoubleRightTee",                  14, "\xe2\x8a\xa8",                3},    /* "⊨" */
    {"DoubleUpArrow",                   13, "\xe2\x87\x91",                3},    /* "⇑" */
    {"DoubleUpDownArrow",               17, "\xe2\x87\x95",                3},    /* "⇕" */
    {"DoubleVerticalBar",               17, "\xe2\x88\xa5",                3},    /* "∥" */
    {"DownArrow",                        9, "\xe2\x86\x93",                3},    /* "↓" */
    {"DownArrowBar",                    12, "\xe2\xa4\x93",                3},    /* "⤓" */
    {"DownArrowUpArrow",                16, "\xe2\x87\xb5",                3},    /* "⇵" */
    {"DownBreve",                        9, "\xcc\x91",                    2},    /* "̑" */
    {"DownLeftRightVector",             19, "\xe2\xa5\x90",                3},    /* "⥐" */
    {"DownLeftTeeVector",               17, "\xe2\xa5\x9e",                3},    /* "⥞" */
    {"DownLeftVector",                  14, "\xe2\x86\xbd",                3},    /* "↽" */
    {"DownLeftVectorBar",               17, "\xe2\xa5\x96",                3},    /* "⥖" */
    {"DownRightTeeVector",              18, "\xe2\xa5\x9f",                3},    /* "⥟" */
    {"DownRightVector",                 15, "\xe2\x87\x81",                3},    /* "⇁" */
    {"DownRightVectorBar",              18, "\xe2\xa5\x97",                3},    /* "⥗" */
    {"DownTee",                          7, "\xe2\x8a\xa4",                3},    /* "⊤" */
    {"DownTeeArrow",                    12, "\xe2\x86\xa7",                3},    /* "↧" */
    {"Downarrow",                        9, "\xe2\x87\x93",                3},    /* "⇓" */
    {"Dscr",                             4, "\xf0\x9d\x92\x9f",            4},    /* "𝒟" */
    {"Dstrok",                           6, "\xc4\x90",                    2},    /* "Đ" */
    {"ENG",                              3, "\xc5\x8a",                    2},    /* "Ŋ" */
    {"ETH",                              3, "\xc3\x90",                    2},    /* "Ð" */
    {"Eacute",                           6, "\xc3\x89",                    2},    /* "É" */
    {"Ecaron",                           6, "\xc4\x9a",                    2},    /* "Ě" */
    {"Ecirc",                            5, "\xc3\x8a",                    2},    /* "Ê" */
    {"Ecy",                              3, "\xd0\xad",                    2},    /* "Э" */
    {"Edot",                             4, "\xc4\x96",                    2},    /* "Ė" */
    {"Efr",                              3, "\xf0\x9d\x94\x88",            4},    /* "𝔈" */
    {"Egrave",                           6, "\xc3\x88",                    2},    /* "È" */
    {"Element",                          7, "\xe2\x88\x88",                3},    /* "∈" */
    {"Emacr",                            5, "\xc4\x92",                    2},    /* "Ē" */
    {"EmptySmallSquare",                16, "\xe2\x97\xbb",                3},    /* "◻" */
    {"EmptyVerySmallSquare",            20, "\xe2\x96\xab",                3},    /* "▫" */
    {"Eogon",                            5, "\xc4\x98",                    2},    /* "Ę" */
    {"Eopf",                             4, "\xf0\x9d\x94\xbc",            4},    /* "𝔼" */
    {"Epsilon",                          7, "\xce\x95",                    2},    /* "Ε" */
    {"Equal",                            5, "\xe2\xa9\xb5",                3},    /* "⩵" */
    {"EqualTilde",                      10, "\xe2\x89\x82",                3},    /* "≂" */
    {"Equilibrium",                     11, "\xe2\x87\x8c",                3},    /* "⇌" */
    {"Escr",                             4, "\xe2\x84\xb0",                3},    /* "ℰ" */
    {"Esim",                             4, "\xe2\xa9\xb3",                3},    /* "⩳" */
    {"Eta",                              3, "\xce\x97",                    2},    /* "Η" */
    {"Euml",                             4, "\xc3\x8b",                    2},    /* "Ë" */
    {"Exists",                           6, "\xe2\x88\x83",                3},    /* "∃" */
    {"ExponentialE",                    12, "\xe2\x85\x87",                3},    /* "ⅇ" */
    {"Fcy",                              3, "\xd0\xa4",                    2},    /* "Ф" */
    {"Ffr",                              3, "\xf0\x9d\x94\x89",            4},    /* "𝔉" */
    {"FilledSmallSquare",               17, "\xe2\x97\xbc",                3},    /* "◼" */
    {"FilledVerySmallSquare",           21, "\xe2\x96\xaa",                3},    /* "▪" */
    {"Fopf",                             4, "\xf0\x9d\x94\xbd",            4},    /* "𝔽" */
    {"ForAll",                           6, "\xe2\x88\x80",                3},    /* "∀" */
    {"Fouriertrf",                      10, "\xe2\x84\xb1",                3},    /* "ℱ" */
    {"Fscr",                             4, "\xe2\x84\xb1",                3},    /* "ℱ" */
    {"GJcy",                             4, "\xd0\x83",                    2},    /* "Ѓ" */
    {"GT",                               2, "\x3e",                        1},    /* ">" */
    {"Gamma",                            5, "\xce\x93",                    2},    /* "Γ" */
    {"Gammad",                           6, "\xcf\x9c",                    2},    /* "Ϝ" */
    {"Gbreve",                           6, "\xc4\x9e",                    2},    /* "Ğ" */
    {"Gcedil",                           6, "\xc4\xa2",                    2},    /* "Ģ" */
    {"Gcirc",                            5, "\xc4\x9c",                    2},    /* "Ĝ" */
    {"Gcy",                              3, "\xd0\x93",                    2},    /* "Г" */
    {"Gdot",                             4, "\xc4\xa0",                    2},    /* "Ġ" */
    {"Gfr",                              3, "\xf0\x9d\x94\x8a",            4},    /* "𝔊" */
    {"Gg",                               2, "\xe2\x8b\x99",                3},    /* "⋙" */
    {"Gopf",                             4, "\xf0\x9d\x94\xbe",            4},    /* "𝔾" */
    {"GreaterEqual",                    12, "\xe2\x89\xa5",                3},    /* "≥" */
    {"GreaterEqualLess",                16, "\xe2\x8b\x9b",                3},    /* "⋛" */
    {"GreaterFullEqual",                16, "\xe2\x89\xa7",                3},    /* "≧" */
    {"GreaterGreater",                  14, "\xe2\xaa\xa2",                3},    /* "⪢" */
    {"GreaterLess",                     11, "\xe2\x89\xb7",                3},    /* "≷" */
    {"GreaterSlantEqual",               17, "\xe2\xa9\xbe",                3},    /* "⩾" */
    {"GreaterTilde",                    12, "\xe2\x89\xb3",                3},    /* "≳" */
    {"Gscr",                             4, "\xf0\x9d\x92\xa2",            4},    /* "𝒢" */
    {"Gt",                               2, "\xe2\x89\xab",                3},    /* "≫" */
    {"HARDcy",                           6, "\xd0\xaa",                    2},    /* "Ъ" */
    {"Hacek",                            5, "\xcb\x87",                    2},    /* "ˇ" */
    {"Hat",                              3, "\x5e",                        1},    /* "^" */
    {"Hcirc",                            5, "\xc4\xa4",                    2},    /* "Ĥ" */
    {"Hfr",                              3, "\xe2\x84\x8c",                3},    /* "ℌ" */
    {"HilbertSpace",                    12, "\xe2\x84\x8b",                3},    /* "ℋ" */
    {"Hopf",                             4, "\xe2\x84\x8d",                3},    /* "ℍ" */
    {"HorizontalLine",                  14, "\xe2\x94\x80",                3},    /* "─" */
    {"Hscr",                             4, "\xe2\x84\x8b",                3},    /* "ℋ" */
    {"Hstrok",                           6, "\xc4\xa6",                    2},    /* "Ħ" */
    {"HumpDownHump",                    12, "\xe2\x89\x8e",                3},    /* "≎" */
    {"HumpEqual",                        9, "\xe2\x89\x8f",                3},    /* "≏" */
    {"IEcy",                             4, "\xd0\x95",                    2},    /* "Е" */
    {"IJlig",                            5, "\xc4\xb2",                    2},    /* "Ĳ" */
    {"IOcy",                             4, "\xd0\x81",                    2},    /* "Ё" */
    {"Iacute",                           6, "\xc3\x8d",                    2},    /* "Í" */
    {"Icirc",                            5, "\xc3\x8e",                    2},    /* "Î" */
    {"Icy",                              3, "\xd0\x98",                    2},    /* "И" */
    {"Idot",                             4, "\xc4\xb0",                    2},    /* "İ" */
    {"Ifr",                              3, "\xe2\x84\x91",                3},    /* "ℑ" */
    {"Igrave",                           6, "\xc3\x8c",                    2},    /* "Ì" */
    {"Im",                               2, "\xe2\x84\x91",                3},    /* "ℑ" */
    {"Imacr",                            5, "\xc4\xaa",                    2},    /* "Ī" */
    {"ImaginaryI",                      10, "\xe2\x85\x88",                3},    /* "ⅈ" */
    {"Implies",                          7, "\xe2\x87\x92",                3},    /* "⇒" */
    {"Int",                              3, "\xe2\x88\xac",                3},    /* "∬" */
    {"Integral",                         8, "\xe2\x88\xab",                3},    /* "∫" */
    {"Intersection",                    12, "\xe2\x8b\x82",                3},    /* "⋂" */
    {"InvisibleComma",                  14, "\xe2\x81\xa3",                3},    /* InvisibleComma */
    {"InvisibleTimes",                  14, "\xe2\x81\xa2",                3},    /* InvisibleTimes */
    {"Iogon",                            5, "\xc4\xae",                    2},    /* "Į" */
    {"Iopf",                             4, "\xf0\x9d\x95\x80",            4},    /* "𝕀" */
    {"Iota",                             4, "\xce\x99",                    2},    /* "Ι" */
    {"Iscr",                             4, "\xe2\x84\x90",                3},    /* "ℐ" */
    {"Itilde",                           6, "\xc4\xa8",                    2},    /* "Ĩ" */
    {"Iukcy",                            5, "\xd0\x86",                    2},    /* "І" */
    {"Iuml",                             4, "\xc3\x8f",                    2},    /* "Ï" */
    {"Jcirc",                            5, "\xc4\xb4",                    2},    /* "Ĵ" */
    {"Jcy",                              3, "\xd0\x99",                    2},    /* "Й" */
    {"Jfr",                              3, "\xf0\x9d\x94\x8d",            4},    /* "𝔍" */
    {"Jopf",                             4, "\xf0\x9d\x95\x81",            4},    /* "𝕁" */
    {"Jscr",                             4, "\xf0\x9d\x92\xa5",            4},    /* "𝒥" */
    {"Jsercy",                           6, "\xd0\x88",                    2},    /* "Ј" */
    {"Jukcy",                            5, "\xd0\x84",                    2},    /* "Є" */
    {"KHcy",                             4, "\xd0\xa5",                    2},    /* "Х" */
    {"KJcy",                             4, "\xd0\x8c",                    2},    /* "Ќ" */
    {"Kappa",                            5, "\xce\x9a",                    2},    /* "Κ" */
    {"Kcedil",                           6, "\xc4\xb6",                    2},    /* "Ķ" */
    {"Kcy",                              3, "\xd0\x9a",                    2},    /* "К" */
    {"Kfr",                              3, "\xf0\x9d\x94\x8e",            4},    /* "𝔎" */
    {"Kopf",                             4, "\xf0\x9d\x95\x82",            4},    /* "𝕂" */
    {"Kscr",                             4, "\xf0\x9d\x92\xa6",            4},    /* "𝒦" */
    {"LJcy",                             4, "\xd0\x89",                    2},    /* "Љ" */
    {"LT",                               2, "\x3c",                        1},    /* "<" */
    {"Lacute",                           6, "\xc4\xb9",                    2},    /* "Ĺ" */
    {"Lambda",                           6, "\xce\x9b",                    2},    /* "Λ" */
    {"Lang",                             4, "\xe2\x9f\xaa",                3},    /* "⟪" */
    {"Laplacetrf",                      10, "\xe2\x84\x92",                3},    /* "ℒ" */
    {"Larr",                             4, "\xe2\x86\x9e",                3},    /* "↞" */
    {"Lcaron",                           6, "\xc4\xbd",                    2},    /* "Ľ" */
    {"Lcedil",                           6, "\xc4\xbb",                    2},    /* "Ļ" */
    {"Lcy",                              3, "\xd0\x9b",                    2},    /* "Л" */
    {"LeftAngleBracket",                16, "\xe2\x9f\xa8",                3},    /* "⟨" */
    {"LeftArrow",                        9, "\xe2\x86\x90",                3},    /* "←" */
    {"LeftArrowBar",                    12, "\xe2\x87\xa4",                3},    /* "⇤" */
    {"LeftArrowRightArrow",             19, "\xe2\x87\x86",                3},    /* "⇆" */
    {"LeftCeiling",                     11, "\xe2\x8c\x88",                3},    /* "⌈" */
    {"LeftDoubleBracket",               17, "\xe2\x9f\xa6",                3},    /* "⟦" */
    {"LeftDownTeeVector",               17, "\xe2\xa5\xa1",                3},    /* "⥡" */
    {"LeftDownVector",                  14, "\xe2\x87\x83",                3},    /* "⇃" */
    {"LeftDownVectorBar",               17, "\xe2\xa5\x99",                3},    /* "⥙" */
    {"LeftFloor",                        9, "\xe2\x8c\x8a",                3},    /* "⌊" */
    {"LeftRightArrow",                  14, "\xe2\x86\x94",                3},    /* "↔" */
    {"LeftRightVector",                 15, "\xe2\xa5\x8e",                3},    /* "⥎" */
    {"LeftTee",                          7, "\xe2\x8a\xa3",                3},    /* "⊣" */
    {"LeftTeeArrow",                    12, "\xe2\x86\xa4",                3},    /* "↤" */
    {"LeftTeeVector",                   13, "\xe2\xa5\x9a",                3},    /* "⥚" */
    {"LeftTriangle",                    12, "\xe2\x8a\xb2",                3},    /* "⊲" */
    {"LeftTriangleBar",                 15, "\xe2\xa7\x8f",                3},    /* "⧏" */
    {"LeftTriangleEqual",               17, "\xe2\x8a\xb4",                3},    /* "⊴" */
    {"LeftUpDownVector",                16, "\xe2\xa5\x91",                3},    /* "⥑" */
    {"LeftUpTeeVector",                 15, "\xe2\xa5\xa0",                3},    /* "⥠" */
    {"LeftUpVector",                    12, "\xe2\x86\xbf",                3},    /* "↿" */
    {"LeftUpVectorBar",                 15, "\xe2\xa5\x98",                3},    /* "⥘" */
    {"LeftVector",                      10, "\xe2\x86\xbc",                3},    /* "↼" */
    {"LeftVectorBar",                   13, "\xe2\xa5\x92",                3},    /* "⥒" */
    {"Leftarrow",                        9, "\xe2\x87\x90",                3},    /* "⇐" */
    {"Leftrightarrow",                  14, "\xe2\x87\x94",                3},    /* "⇔" */
    {"LessEqualGreater",                16, "\xe2\x8b\x9a",                3},    /* "⋚" */
    {"LessFullEqual",                   13, "\xe2\x89\xa6",                3},    /* "≦" */
    {"LessGreater",                     11, "\xe2\x89\xb6",                3},    /* "≶" */
    {"LessLess",                         8, "\xe2\xaa\xa1",                3},    /* "⪡" */
    {"LessSlantEqual",                  14, "\xe2\xa9\xbd",                3},    /* "⩽" */
    {"LessTilde",                        9, "\xe2\x89\xb2",                3},    /* "≲" */
    {"Lfr",                              3, "\xf0\x9d\x94\x8f",            4},    /* "𝔏" */
    {"Ll",                               2, "\xe2\x8b\x98",                3},    /* "⋘" */
    {"Lleftarrow",                      10, "\xe2\x87\x9a",                3},    /* "⇚" */
    {"Lmidot",                           6, "\xc4\xbf",                    2},    /* "Ŀ" */
    {"LongLeftArrow",                   13, "\xe2\x9f\xb5",                3},    /* "⟵" */
    {"LongLeftRightArrow",              18, "\xe2\x9f\xb7",                3},    /* "⟷" */
    {"LongRightArrow",                  14, "\xe2\x9f\xb6",                3},    /* "⟶" */
    {"Longleftarrow",                   13, "\xe2\x9f\xb8",                3},    /* "⟸" */
    {"Longleftrightarrow",              18, "\xe2\x9f\xba",                3},    /* "⟺" */
    {"Longrightarrow",                  14, "\xe2\x9f\xb9",                3},    /* "⟹" */
    {"Lopf",                             4, "\xf0\x9d\x95\x83",            4},    /* "𝕃" */
    {"LowerLeftArrow",                  14, "\xe2\x86\x99",                3},    /* "↙" */
    {"LowerRightArrow",                 15, "\xe2\x86\x98",                3},    /* "↘" */
    {"Lscr",                             4, "\xe2\x84\x92",                3},    /* "ℒ" */
    {"Lsh",                              3, "\xe2\x86\xb0",                3},    /* "↰" */
    {"Lstrok",                           6, "\xc5\x81",                    2},    /* "Ł" */
    {"Lt",                               2, "\xe2\x89\xaa",                3},    /* "≪" */
    {"Map",                              3, "\xe2\xa4\x85",                3},    /* "⤅" */
    {"Mcy",                              3, "\xd0\x9c",                    2},    /* "М" */
    {"MediumSpace",                     11, "\xe2\x81\x9f",                3},    /* " " */
    {"Mellintrf",                        9, "\xe2\x84\xb3",                3},    /* "ℳ" */
    {"Mfr",                              3, "\xf0\x9d\x94\x90",            4},    /* "𝔐" */
    {"MinusPlus",                        9, "\xe2\x88\x93",                3},    /* "∓" */
    {"Mopf",                             4, "\xf0\x9d\x95\x84",            4},    /* "𝕄" */
    {"Mscr",                             4, "\xe2\x84\xb3",                3},    /* "ℳ" */
    {"Mu",                               2, "\xce\x9c",                    2},    /* "Μ" */
    {"NJcy",                             4, "\xd0\x8a",                    2},    /* "Њ" */
    {"Nacute",                           6, "\xc5\x83",                    2},    /* "Ń" */
    {"Ncaron",                           6, "\xc5\x87",                    2},    /* "Ň" */
    {"Ncedil",                           6, "\xc5\x85",                    2},    /* "Ņ" */
    {"Ncy",                              3, "\xd0\x9d",                    2},    /* "Н" */
    {"NegativeMediumSpace",             19, "\xe2\x80\x8b",                3},    /* ZeroWidthSpace */
    {"NegativeThickSpace",              18, "\xe2\x80\x8b",                3},    /* ZeroWidthSpace */
    {"NegativeThinSpace",               17, "\xe2\x80\x8b",                3},    /* ZeroWidthSpace */
    {"NegativeVeryThinSpace",           21, "\xe2\x80\x8b",                3},    /* ZeroWidthSpace */
    {"NestedGreaterGreater",            20, "\xe2\x89\xab",                3},    /* "≫" */
    {"NestedLessLess",                  14, "\xe2\x89\xaa",                3},    /* "≪" */
    {"NewLine",                          7, "\x0a",                        1},    /* NewLine */
    {"Nfr",                              3, "\xf0\x9d\x94\x91",            4},    /* "𝔑" */
    {"NoBreak",                          7, "\xe2\x81\xa0",                3},    /* NoBreak */
    {"NonBreakingSpace",                16, "\xc2\xa0",                    2},    /* " " */
    {"Nopf",                             4, "\xe2\x84\x95",                3},    /* "ℕ" */
    {"Not",                              3, "\xe2\xab\xac",                3},    /* "⫬" */
    {"NotCongruent",                    12, "\xe2\x89\xa2",                3},    /* "≢" */
    {"NotCupCap",                        9, "\xe2\x89\xad",                3},    /* "≭" */
    {"NotDoubleVerticalBar",            20, "\xe2\x88\xa6",                3},    /* "∦" */
    {"NotElement",                      10, "\xe2\x88\x89",                3},    /* "∉" */
    {"NotEqual",                         8, "\xe2\x89\xa0",                3},    /* "≠" */
    {"NotEqualTilde",                   13, "\xe2\x89\x82\xcc\xb8",        5},    /* "≂̸" */
    {"NotExists",                        9, "\xe2\x88\x84",                3},    /* "∄" */
    {"NotGreater",                      10, "\xe2\x89\xaf",                3},    /* "≯" */
    {"NotGreaterEqual",                 15, "\xe2\x89\xb1",                3},    /* "≱" */
    {"NotGreaterFullEqual",             19, "\xe2\x89\xa7\xcc\xb8",        5},    /* "≧̸" */
    {"NotGreaterGreater",               17, "\xe2\x89\xab\xcc\xb8",        5},    /* "≫̸" */
    {"NotGreaterLess",                  14, "\xe2\x89\xb9",                3},    /* "≹" */
    {"NotGreaterSlantEqual",            20, "\xe2\xa9\xbe\xcc\xb8",        5},    /* "⩾̸" */
    {"NotGreaterTilde",                 15, "\xe2\x89\xb5",                3},    /* "≵" */
    {"NotHumpDownHump",                 15, "\xe2\x89\x8e\xcc\xb8",        5},    /* "≎̸" */
    {"NotHumpEqual",                    12, "\xe2\x89\x8f\xcc\xb8",        5},    /* "≏̸" */
    {"NotLeftTriangle",                 15, "\xe2\x8b\xaa",                3},    /* "⋪" */
    {"NotLeftTriangleBar",              18, "\xe2\xa7\x8f\xcc\xb8",        5},    /* "⧏̸" */
    {"NotLeftTriangleEqual",            20, "\xe2\x8b\xac",                3},    /* "⋬" */
    {"NotLess",                          7, "\xe2\x89\xae",                3},    /* "≮" */
    {"NotLessEqual",                    12, "\xe2\x89\xb0",                3},    /* "≰" */
    {"NotLessGreater",                  14, "\xe2\x89\xb8",                3},    /* "≸" */
    {"NotLessLess",                     11, "\xe2\x89\xaa\xcc\xb8",        5},    /* "≪̸" */
    {"NotLessSlantEqual",               17, "\xe2\xa9\xbd\xcc\xb8",        5},    /* "⩽̸" */
    {"NotLessTilde",                    12, "\xe2\x89\xb4",                3},    /* "≴" */
    {"NotNestedGreaterGreater",         23, "\xe2\xaa\xa2\xcc\xb8",        5},    /* "⪢̸" */
    {"NotNestedLessLess",               17, "\xe2\xaa\xa1\xcc\xb8",        5},    /* "⪡̸" */
    {"NotPrecedes",                     11, "\xe2\x8a\x80",                3},    /* "⊀" */
    {"NotPrecedesEqual",                16, "\xe2\xaa\xaf\xcc\xb8",        5},    /* "⪯̸" */
    {"NotPrecedesSlantEqual",           21, "\xe2\x8b\xa0",                3},    /* "⋠" */
    {"NotReverseElement",               17, "\xe2\x88\x8c",                3},    /* "∌" */
    {"NotRightTriangle",                16, "\xe2\x8b\xab",                3},    /* "⋫" */
    {"NotRightTriangleBar",             19, "\xe2\xa7\x90\xcc\xb8",        5},    /* "⧐̸" */
    {"NotRightTriangleEqual",           21, "\xe2\x8b\xad",                3},    /* "⋭" */
    {"NotSquareSubset",                 15, "\xe2\x8a\x8f\xcc\xb8",        5},    /* "⊏̸" */
    {"NotSquareSubsetEqual",            20, "\xe2\x8b\xa2",                3},    /* "⋢" */
    {"NotSquareSuperset",               17, "\xe2\x8a\x90\xcc\xb8",        5},    /* "⊐̸" */
    {"NotSquareSupersetEqual",          22, "\xe2\x8b\xa3",                3},    /* "⋣" */
    {"NotSubset",                        9, "\xe2\x8a\x82\xe2\x83\x92",    6},    /* "⊂⃒" */
    {"NotSubsetEqual",                  14, "\xe2\x8a\x88",                3},    /* "⊈" */
    {"NotSucceeds",                     11, "\xe2\x8a\x81",                3},    /* "⊁" */
    {"NotSucceedsEqual",                16, "\xe2\xaa\xb0\xcc\xb8",        5},    /* "⪰̸" */
    {"NotSucceedsSlantEqual",           21, "\xe2\x8b\xa1",                3},    /* "⋡" */
    {"NotSucceedsTilde",                16, "\xe2\x89\xbf\xcc\xb8",        5},    /* "≿̸" */
    {"NotSuperset",                     11, "\xe2\x8a\x83\xe2\x83\x92",    6},    /* "⊃⃒" */
    {"NotSupersetEqual",                16, "\xe2\x8a\x89",                3},    /* "⊉" */
    {"NotTilde",                         8, "\xe2\x89\x81",                3},    /* "≁" */
    {"NotTildeEqual",                   13, "\xe2\x89\x84",                3},    /* "≄" */
    {"NotTildeFullEqual",               17, "\xe2\x89\x87",                3},    /* "≇" */
    {"NotTildeTilde",                   13, "\xe2\x89\x89",                3},    /* "≉" */
    {"NotVerticalBar",                  14, "\xe2\x88\xa4",                3},    /* "∤" */
    {"Nscr",                             4, "\xf0\x9d\x92\xa9",            4},    /* "𝒩" */
    {"Ntilde",                           6, "\xc3\x91",                    2},    /* "Ñ" */
    {"Nu",                               2, "\xce\x9d",                    2},    /* "Ν" */
    {"OElig",                            5, "\xc5\x92",                    2},    /* "Œ" */
    {"Oacute",                           6, "\xc3\x93",                    2},    /* "Ó" */
    {"Ocirc",                            5, "\xc3\x94",                    2},    /* "Ô" */
    {"Ocy",                              3, "\xd0\x9e",                    2},    /* "О" */
    {"Odblac",                           6, "\xc5\x90",                    2},    /* "Ő" */
    {"Ofr",                              3, "\xf0\x9d\x94\x92",            4},    /* "𝔒" */
    {"Ograve",                           6, "\xc3\x92",                    2},    /* "Ò" */
    {"Omacr",                            5, "\xc5\x8c",                    2},    /* "Ō" */
    {"Omega",                            5, "\xce\xa9",                    2},    /* "Ω" */
    {"Omicron",                          7, "\xce\x9f",                    2},    /* "Ο" */
    {"Oopf",                             4, "\xf0\x9d\x95\x86",            4},    /* "𝕆" */
    {"OpenCurlyDoubleQuote",            20, "\xe2\x80\x9c",                3},    /* "“" */
    {"OpenCurlyQuote",                  14, "\xe2\x80\x98",                3},    /* "‘" */
    {"Or",                               2, "\xe2\xa9\x94",                3},    /* "⩔" */
    {"Oscr",                             4, "\xf0\x9d\x92\xaa",            4},    /* "𝒪" */
    {"Oslash",                           6, "\xc3\x98",                    2},    /* "Ø" */
    {"Otilde",                           6, "\xc3\x95",                    2},    /* "Õ" */
    {"Otimes",                           6, "\xe2\xa8\xb7",                3},    /* "⨷" */
    {"Ouml",                             4, "\xc3\x96",                    2},    /* "Ö" */
    {"OverBar",                          7, "\xe2\x80\xbe",                3},    /* "‾" */
    {"OverBrace",                        9, "\xe2\x8f\x9e",                3},    /* "⏞" */
    {"OverBracket",                     11, "\xe2\x8e\xb4",                3},    /* "⎴" */
    {"OverParenthesis",                 15, "\xe2\x8f\x9c",                3},    /* "⏜" */
    {"PartialD",                         8, "\xe2\x88\x82",                3},    /* "∂" */
    {"Pcy",                              3, "\xd0\x9f",                    2},    /* "П" */
    {"Pfr",                              3, "\xf0\x9d\x94\x93",            4},    /* "𝔓" */
    {"Phi",                              3, "\xce\xa6",                    2},    /* "Φ" */
    {"Pi",                               2, "\xce\xa0",                    2},    /* "Π" */
    {"PlusMinus",                        9, "\xc2\xb1",                    2},    /* "±" */
    {"Poincareplane",                   13, "\xe2\x84\x8c",                3},    /* "ℌ" */
    {"Popf",                             4, "\xe2\x84\x99",                3},    /* "ℙ" */
    {"Pr",                               2, "\xe2\xaa\xbb",                3},    /* "⪻" */
    {"Precedes",                         8, "\xe2\x89\xba",                3},    /* "≺" */
    {"PrecedesEqual",                   13, "\xe2\xaa\xaf",                3},    /* "⪯" */
    {"PrecedesSlantEqual",              18, "\xe2\x89\xbc",                3},    /* "≼" */
    {"PrecedesTilde",                   13, "\xe2\x89\xbe",                3},    /* "≾" */
    {"Prime",                            5, "\xe2\x80\xb3",                3},    /* "″" */
    {"Product",                          7, "\xe2\x88\x8f",                3},    /* "∏" */
    {"Proportion",                      10, "\xe2\x88\xb7",                3},    /* "∷" */
    {"Proportional",                    12, "\xe2\x88\x9d",                3},    /* "∝" */
    {"Pscr",                             4, "\xf0\x9d\x92\xab",            4},    /* "𝒫" */
    {"Psi",                              3, "\xce\xa8",                    2},    /* "Ψ" */
    {"QUOT",                             4, "\x22",                        1},    /* """ */
    {"Qfr",                              3, "\xf0\x9d\x94\x94",            4},    /* "𝔔" */
    {"Qopf",                             4, "\xe2\x84\x9a",                3},    /* "ℚ" */
    {"Qscr",                             4, "\xf0\x9d\x92\xac",            4},    /* "𝒬" */
    {"RBarr",                            5, "\xe2\xa4\x90",                3},    /* "⤐" */
    {"REG",                              3, "\xc2\xae",                    2},    /* "®" */
    {"Racute",                           6, "\xc5\x94",                    2},    /* "Ŕ" */
    {"Rang",                             4, "\xe2\x9f\xab",                3},    /* "⟫" */
    {"Rarr",                             4, "\xe2\x86\xa0",                3},    /* "↠" */
    {"Rarrtl",                           6, "\xe2\xa4\x96",                3},    /* "⤖" */
    {"Rcaron",                           6, "\xc5\x98",                    2},    /* "Ř" */
    {"Rcedil",                           6, "\xc5\x96",                    2},    /* "Ŗ" */
    {"Rcy",                              3, "\xd0\xa0",                    2},    /* "Р" */
    {"Re",                               2, "\xe2\x84\x9c",                3},    /* "ℜ" */
    {"ReverseElement",                  14, "\xe2\x88\x8b",                3},    /* "∋" */
    {"ReverseEquilibrium",              18, "\xe2\x87\x8b",                3},    /* "⇋" */
    {"ReverseUpEquilibrium",            20, "\xe2\xa5\xaf",                3},    /* "⥯" */
    {"Rfr",                              3, "\xe2\x84\x9c",                3},    /* "ℜ" */
    {"Rho",                              3, "\xce\xa1",                    2},    /* "Ρ" */
    {"RightAngleBracket",               17, "\xe2\x9f\xa9",                3},    /* "⟩" */
    {"RightArrow",                      10, "\xe2\x86\x92",                3},    /* "→" */
    {"RightArrowBar",                   13, "\xe2\x87\xa5",                3},    /* "⇥" */
    {"RightArrowLeftArrow",             19, "\xe2\x87\x84",                3},    /* "⇄" */
    {"RightCeiling",                    12, "\xe2\x8c\x89",                3},    /* "⌉" */
    {"RightDoubleBracket",              18, "\xe2\x9f\xa7",                3},    /* "⟧" */
    {"RightDownTeeVector",              18, "\xe2\xa5\x9d",                3},    /* "⥝" */
    {"RightDownVector",                 15, "\xe2\x87\x82",                3},    /* "⇂" */
    {"RightDownVectorBar",              18, "\xe2\xa5\x95",                3},    /* "⥕" */
    {"RightFloor",                      10, "\xe2\x8c\x8b",                3},    /* "⌋" */
    {"RightTee",                         8, "\xe2\x8a\xa2",                3},    /* "⊢" */
    {"RightTeeArrow",                   13, "\xe2\x86\xa6",                3},    /* "↦" */
    {"RightTeeVector",                  14, "\xe2\xa5\x9b",                3},    /* "⥛" */
    {"RightTriangle",                   13, "\xe2\x8a\xb3",                3},    /* "⊳" */
    {"RightTriangleBar",                16, "\xe2\xa7\x90",                3},    /* "⧐" */
    {"RightTriangleEqual",              18, "\xe2\x8a\xb5",                3},    /* "⊵" */
    {"RightUpDownVector",               17, "\xe2\xa5\x8f",                3},    /* "⥏" */
    {"RightUpTeeVector",                16, "\xe2\xa5\x9c",                3},    /* "⥜" */
    {"RightUpVector",                   13, "\xe2\x86\xbe",                3},    /* "↾" */
    {"RightUpVectorBar",                16, "\xe2\xa5\x94",                3},    /* "⥔" */
    {"RightVector",                     11, "\xe2\x87\x80",                3},    /* "⇀" */
    {"RightVectorBar",                  14, "\xe2\xa5\x93",                3},    /* "⥓" */
    {"Rightarrow",                      10, "\xe2\x87\x92",                3},    /* "⇒" */
    {"Ropf",                             4, "\xe2\x84\x9d",                3},    /* "ℝ" */
    {"RoundImplies",                    12, "\xe2\xa5\xb0",                3},    /* "⥰" */
    {"Rrightarrow",                     11, "\xe2\x87\x9b",                3},    /* "⇛" */
    {"Rscr",                             4, "\xe2\x84\x9b",                3},    /* "ℛ" */
    {"Rsh",                              3, "\xe2\x86\xb1",                3},    /* "↱" */
    {"RuleDelayed",                     11, "\xe2\xa7\xb4",                3},    /* "⧴" */
    {"SHCHcy",                           6, "\xd0\xa9",                    2},    /* "Щ" */
    {"SHcy",                             4, "\xd0\xa8",                    2},    /* "Ш" */
    {"SOFTcy",                           6, "\xd0\xac",                    2},    /* "Ь" */
    {"Sacute",                           6, "\xc5\x9a",                    2},    /* "Ś" */
    {"Sc",                               2, "\xe2\xaa\xbc",                3},    /* "⪼" */
    {"Scaron",                           6, "\xc5\xa0",                    2},    /* "Š" */
    {"Scedil",                           6, "\xc5\x9e",                    2},    /* "Ş" */
    {"Scirc",                            5, "\xc5\x9c",                    2},    /* "Ŝ" */
    {"Scy",                              3, "\xd0\xa1",                    2},    /* "С" */
    {"Sfr",                              3, "\xf0\x9d\x94\x96",            4},    /* "𝔖" */
    {"ShortDownArrow",                  14, "\xe2\x86\x93",                3},    /* "↓" */
    {"ShortLeftArrow",                  14, "\xe2\x86\x90",                3},    /* "←" */
    {"ShortRightArrow",                 15, "\xe2\x86\x92",                3},    /* "→" */
    {"ShortUpArrow",                    12, "\xe2\x86\x91",                3},    /* "↑" */
    {"Sigma",                            5, "\xce\xa3",                    2},    /* "Σ" */
    {"SmallCircle",                     11, "\xe2\x88\x98",                3},    /* "∘" */
    {"Sopf",                             4, "\xf0\x9d\x95\x8a",            4},    /* "𝕊" */
    {"Sqrt",                             4, "\xe2\x88\x9a",                3},    /* "√" */
    {"Square",                           6, "\xe2\x96\xa1",                3},    /* "□" */
    {"SquareIntersection",              18, "\xe2\x8a\x93",                3},    /* "⊓" */
    {"SquareSubset",                    12, "\xe2\x8a\x8f",                3},    /* "⊏" */
    {"SquareSubsetEqual",               17, "\xe2\x8a\x91",                3},    /* "⊑" */
    {"SquareSuperset",                  14, "\xe2\x8a\x90",                3},    /* "⊐" */
    {"SquareSupersetEqual",             19, "\xe2\x8a\x92",                3},    /* "⊒" */
    {"SquareUnion",                     11, "\xe2\x8a\x94",                3},    /* "⊔" */
    {"Sscr",                             4, "\xf0\x9d\x92\xae",            4},    /* "𝒮" */
    {"Star",                             4, "\xe2\x8b\x86",                3},    /* "⋆" */
    {"Sub",                              3, "\xe2\x8b\x90",                3},    /* "⋐" */
    {"Subset",                           6, "\xe2\x8b\x90",                3},    /* "⋐" */
    {"SubsetEqual",                     11, "\xe2\x8a\x86",                3},    /* "⊆" */
    {"Succeeds",                         8, "\xe2\x89\xbb",                3},    /* "≻" */
    {"SucceedsEqual",                   13, "\xe2\xaa\xb0",                3},    /* "⪰" */
    {"SucceedsSlantEqual",              18, "\xe2\x89\xbd",                3},    /* "≽" */
    {"SucceedsTilde",                   13, "\xe2\x89\xbf",                3},    /* "≿" */
    {"SuchThat",                         8, "\xe2\x88\x8b",                3},    /* "∋" */
    {"Sum",                              3, "\xe2\x88\x91",                3},    /* "∑" */
    {"Sup",                              3, "\xe2\x8b\x91",                3},    /* "⋑" */
    {"Superset",                         8, "\xe2\x8a\x83",                3},    /* "⊃" */
    {"SupersetEqual",                   13, "\xe2\x8a\x87",                3},    /* "⊇" */
    {"Supset",                           6, "\xe2\x8b\x91",                3},    /* "⋑" */
    {"THORN",                            5, "\xc3\x9e",                    2},    /* "Þ" */
    {"TRADE",                            5, "\xe2\x84\xa2",                3},    /* "™" */
    {"TSHcy",                            5, "\xd0\x8b",                    2},    /* "Ћ" */
    {"TScy",                             4, "\xd0\xa6",                    2},    /* "Ц" */
    {"Tab",                              3, "\x09",                        1},    /* Tab */
    {"Tau",                              3, "\xce\xa4",                    2},    /* "Τ" */
    {"Tcaron",                           6, "\xc5\xa4",                    2},    /* "Ť" */
    {"Tcedil",                           6, "\xc5\xa2",                    2},    /* "Ţ" */
    {"Tcy",                              3, "\xd0\xa2",                    2},    /* "Т" */
    {"Tfr",                              3, "\xf0\x9d\x94\x97",            4},    /* "𝔗" */
    {"Therefore",                        9, "\xe2\x88\xb4",                3},    /* "∴" */
    {"Theta",                            5, "\xce\x98",                    2},    /* "Θ" */
    {"ThickSpace",                      10, "\xe2\x81\x9f\xe2\x80\x8a",    6},    /* ThickSpace */
    {"ThinSpace",                        9, "\xe2\x80\x89",                3},    /* " " */
    {"Tilde",                            5, "\xe2\x88\xbc",                3},    /* "∼" */
    {"TildeEqual",                      10, "\xe2\x89\x83",                3},    /* "≃" */
    {"TildeFullEqual",                  14, "\xe2\x89\x85",                3},    /* "≅" */
    {"TildeTilde",                      10, "\xe2\x89\x88",                3},    /* "≈" */
    {"Topf",                             4, "\xf0\x9d\x95\x8b",            4},    /* "𝕋" */
    {"TripleDot",                        9, "\xe2\x83\x9b",                3},    /* "⃛" */
    {"Tscr",                             4, "\xf0\x9d\x92\xaf",            4},    /* "𝒯" */
    {"Tstrok",                           6, "\xc5\xa6",                    2},    /* "Ŧ" */
    {"Uacute",                           6, "\xc3\x9a",                    2},    /* "Ú" */
    {"Uarr",                             4, "\xe2\x86\x9f",                3},    /* "↟" */
    {"Uarrocir",                         8, "\xe2\xa5\x89",                3},    /* "⥉" */
    {"Ubrcy",                            5, "\xd0\x8e",                    2},    /* "Ў" */
    {"Ubreve",                           6, "\xc5\xac",                    2},    /* "Ŭ" */
    {"Ucirc",                            5, "\xc3\x9b",                    2},    /* "Û" */
    {"Ucy",                              3, "\xd0\xa3",                    2},    /* "У" */
    {"Udblac",                           6, "\xc5\xb0",                    2},    /* "Ű" */
    {"Ufr",                              3, "\xf0\x9d\x94\x98",            4},    /* "𝔘" */
    {"Ugrave",                           6, "\xc3\x99",                    2},    /* "Ù" */
    {"Umacr",                            5, "\xc5\xaa",                    2},    /* "Ū" */
    {"UnderBar",                         8, "\x5f",                        1},    /* "_" */
    {"UnderBrace",                      10, "\xe2\x8f\x9f",                3},    /* "⏟" */
    {"UnderBracket",                    12, "\xe2\x8e\xb5",                3},    /* "⎵" */
    {"UnderParenthesis",                16, "\xe2\x8f\x9d",                3},    /* "⏝" */
    {"Union",                            5, "\xe2\x8b\x83",                3},    /* "⋃" */
    {"UnionPlus",                        9, "\xe2\x8a\x8e",                3},    /* "⊎" */
    {"Uogon",                            5, "\xc5\xb2",                    2},    /* "Ų" */
    {"Uopf",                             4, "\xf0\x9d\x95\x8c",            4},    /* "𝕌" */
    {"UpArrow",                          7, "\xe2\x86\x91",                3},    /* "↑" */
    {"UpArrowBar",                      10, "\xe2\xa4\x92",                3},    /* "⤒" */
    {"UpArrowDownArrow",                16, "\xe2\x87\x85",                3},    /* "⇅" */
    {"UpDownArrow",                     11, "\xe2\x86\x95",                3},    /* "↕" */
    {"UpEquilibrium",                   13, "\xe2\xa5\xae",                3},    /* "⥮" */
    {"UpTee",                            5, "\xe2\x8a\xa5",                3},    /* "⊥" */
    {"UpTeeArrow",                      10, "\xe2\x86\xa5",                3},    /* "↥" */
    {"Uparrow",                          7, "\xe2\x87\x91",                3},    /* "⇑" */
    {"Updownarrow",                     11, "\xe2\x87\x95",                3},    /* "⇕" */
    {"UpperLeftArrow",                  14, "\xe2\x86\x96",                3},    /* "↖" */
    {"UpperRightArrow",                 15, "\xe2\x86\x97",                3},    /* "↗" */
    {"Upsi",                             4, "\xcf\x92",                    2},    /* "ϒ" */
    {"Upsilon",                          7, "\xce\xa5",                    2},    /* "Υ" */
    {"Uring",                            5, "\xc5\xae",                    2},    /* "Ů" */
    {"Uscr",                             4, "\xf0\x9d\x92\xb0",            4},    /* "𝒰" */
    {"Utilde",                           6, "\xc5\xa8",                    2},    /* "Ũ" */
    {"Uuml",                             4, "\xc3\x9c",                    2},    /* "Ü" */
    {"VDash",                            5, "\xe2\x8a\xab",                3},    /* "⊫" */
    {"Vbar",                             4, "\xe2\xab\xab",                3},    /* "⫫" */
    {"Vcy",                              3, "\xd0\x92",                    2},    /* "В" */
    {"Vdash",                            5, "\xe2\x8a\xa9",                3},    /* "⊩" */
    {"Vdashl",                           6, "\xe2\xab\xa6",                3},    /* "⫦" */
    {"Vee",                              3, "\xe2\x8b\x81",                3},    /* "⋁" */
    {"Verbar",                           6, "\xe2\x80\x96",                3},    /* "‖" */
    {"Vert",                             4, "\xe2\x80\x96",                3},    /* "‖" */
    {"VerticalBar",                     11, "\xe2\x88\xa3",                3},    /* "∣" */
    {"VerticalLine",                    12, "\x7c",                        1},    /* "|" */
    {"VerticalSeparator",               17, "\xe2\x9d\x98",                3},    /* "❘" */
    {"VerticalTilde",                   13, "\xe2\x89\x80",                3},    /* "≀" */
    {"VeryThinSpace",                   13, "\xe2\x80\x8a",                3},    /* " " */
    {"Vfr",                              3, "\xf0\x9d\x94\x99",            4},    /* "𝔙" */
    {"Vopf",                             4, "\xf0\x9d\x95\x8d",            4},    /* "𝕍" */
    {"Vscr",                             4, "\xf0\x9d\x92\xb1",            4},    /* "𝒱" */
    {"Vvdash",                           6, "\xe2\x8a\xaa",                3},    /* "⊪" */
    {"Wcirc",                            5, "\xc5\xb4",                    2},    /* "Ŵ" */
    {"Wedge",                            5, "\xe2\x8b\x80",                3},    /* "⋀" */
    {"Wfr",                              3, "\xf0\x9d\x94\x9a",            4},    /* "𝔚" */
    {"Wopf",                             4, "\xf0\x9d\x95\x8e",            4},    /* "𝕎" */
    {"Wscr",                             4, "\xf0\x9d\x92\xb2",            4},    /* "𝒲" */
    {"Xfr",                              3, "\xf0\x9d\x94\x9b",            4},    /* "𝔛" */
    {"Xi",                               2, "\xce\x9e",                    2},    /* "Ξ" */
    {"Xopf",                             4, "\xf0\x9d\x95\x8f",            4},    /* "𝕏" */
    {"Xscr",                             4, "\xf0\x9d\x92\xb3",            4},    /* "𝒳" */
    {"YAcy",                             4, "\xd0\xaf",                    2},    /* "Я" */
    {"YIcy",                             4, "\xd0\x87",                    2},    /* "Ї" */
    {"YUcy",                             4, "\xd0\xae",                    2},    /* "Ю" */
    {"Yacute",                           6, "\xc3\x9d",                    2},    /* "Ý" */
    {"Ycirc",                            5, "\xc5\xb6",                    2},    /* "Ŷ" */
    {"Ycy",                              3, "\xd0\xab",                    2},    /* "Ы" */
    {"Yfr",                              3, "\xf0\x9d\x94\x9c",            4},    /* "𝔜" */
    {"Yopf",                             4, "\xf0\x9d\x95\x90",            4},    /* "𝕐" */
    {"Yscr",                             4, "\xf0\x9d\x92\xb4",            4},    /* "𝒴" */
    {"Yuml",                             4, "\xc5\xb8",                    2},    /* "Ÿ" */
    {"ZHcy",                             4, "\xd0\x96",                    2},    /* "Ж" */
    {"Zacute",                           6, "\xc5\xb9",                    2},    /* "Ź" */
    {"Zcaron",                           6, "\xc5\xbd",                    2},    /* "Ž" */
    {"Zcy",                              3, "\xd0\x97",                    2},    /* "З" */
    {"Zdot",                             4, "\xc5\xbb",                    2},    /* "Ż" */
    {"ZeroWidthSpace",                  14, "\xe2\x80\x8b",                3},    /* ZeroWidthSpace */
    {"Zeta",                             4, "\xce\x96",                    2},    /* "Ζ" */
    {"Zfr",                              3, "\xe2\x84\xa8",                3},    /* "ℨ" */
    {"Zopf",                             4, "\xe2\x84\xa4",                3},    /* "ℤ" */
    {"Zscr",                             4, "\xf0\x9d\x92\xb5",            4},    /* "𝒵" */
    {"aacute",                           6, "\xc3\xa1",                    2},    /* "á" */
    {"abreve",                           6, "\xc4\x83",                    2},    /* "ă" */
    {"ac",                               2, "\xe2\x88\xbe",                3},    /* "∾" */
    {"acE",                              3, "\xe2\x88\xbe\xcc\xb3",        5},    /* "∾̳" */
    {"acd",                              3, "\xe2\x88\xbf",                3},    /* "∿" */
    {"acirc",                            5, "\xc3\xa2",                    2},    /* "â" */
    {"acute",                            5, "\xc2\xb4",                    2},    /* "´" */
    {"acy",                              3, "\xd0\xb0",                    2},    /* "а" */
    {"aelig",                            5, "\xc3\xa6",                    2},    /* "æ" */
    {"af",                               2, "\xe2\x81\xa1",                3},    /* ApplyFunction */
    {"afr",                              3, "\xf0\x9d\x94\x9e",            4},    /* "𝔞" */
    {"agrave",                           6, "\xc3\xa0",                    2},    /* "à" */
    {"alefsym",                          7, "\xe2\x84\xb5",                3},    /* "ℵ" */
    {"aleph",                            5, "\xe2\x84\xb5",                3},    /* "ℵ" */
    {"alpha",                            5, "\xce\xb1",                    2},    /* "α" */
    {"amacr",                            5, "\xc4\x81",                    2},    /* "ā" */
    {"amalg",                            5, "\xe2\xa8\xbf",                3},    /* "⨿" */
    {"amp",                              3, "\x26",                        1},    /* "&" */
    {"and",                              3, "\xe2\x88\xa7",                3},    /* "∧" */
    {"andand",                           6, "\xe2\xa9\x95",                3},    /* "⩕" */
    {"andd",                             4, "\xe2\xa9\x9c",                3},    /* "⩜" */
    {"andslope",                         8, "\xe2\xa9\x98",                3},    /* "⩘" */
    {"andv",                             4, "\xe2\xa9\x9a",                3},    /* "⩚" */
    {"ang",                              3, "\xe2\x88\xa0",                3},    /* "∠" */
    {"ange",                             4, "\xe2\xa6\xa4",                3},    /* "⦤" */
    {"angle",                            5, "\xe2\x88\xa0",                3},    /* "∠" */
    {"angmsd",                           6, "\xe2\x88\xa1",                3},    /* "∡" */
    {"angmsdaa",                         8, "\xe2\xa6\xa8",                3},    /* "⦨" */
    {"angmsdab",                         8, "\xe2\xa6\xa9",                3},    /* "⦩" */
    {"angmsdac",                         8, "\xe2\xa6\xaa",                3},    /* "⦪" */
    {"angmsdad",                         8, "\xe2\xa6\xab",                3},    /* "⦫" */
    {"angmsdae",                         8, "\xe2\xa6\xac",                3},    /* "⦬" */
    {"angmsdaf",                         8, "\xe2\xa6\xad",                3},    /* "⦭" */
    {"angmsdag",                         8, "\xe2\xa6\xae",                3},    /* "⦮" */
    {"angmsdah",                         8, "\xe2\xa6\xaf",                3},    /* "⦯" */
    {"angrt",                            5, "\xe2\x88\x9f",                3},    /* "∟" */
    {"angrtvb",                          7, "\xe2\x8a\xbe",                3},    /* "⊾" */
    {"angrtvbd",                         8, "\xe2\xa6\x9d",                3},    /* "⦝" */
    {"angsph",                           6, "\xe2\x88\xa2",                3},    /* "∢" */
    {"angst",                            5, "\xc3\x85",                    2},    /* "Å" */
    {"angzarr",                          7, "\xe2\x8d\xbc",                3},    /* "⍼" */
    {"aogon",                            5, "\xc4\x85",                    2},    /* "ą" */
    {"aopf",                             4, "\xf0\x9d\x95\x92",            4},    /* "𝕒" */
    {"ap",                               2, "\xe2\x89\x88",                3},    /* "≈" */
    {"apE",                              3, "\xe2\xa9\xb0",                3},    /* "⩰" */
    {"apacir",                           6, "\xe2\xa9\xaf",                3},    /* "⩯" */
    {"ape",                              3, "\xe2\x89\x8a",                3},    /* "≊" */
    {"apid",                             4, "\xe2\x89\x8b",                3},    /* "≋" */
    {"apos",                             4, "\x27",                        1},    /* "'" */
    {"approx",                           6, "\xe2\x89\x88",                3},    /* "≈" */
    {"approxeq",                         8, "\xe2\x89\x8a",                3},    /* "≊" */
    {"aring",                            5, "\xc3\xa5",                    2},    /* "å" */
    {"ascr",                             4, "\xf0\x9d\x92\xb6",            4},    /* "𝒶" */
    {"ast",                              3, "\x2a",                        1},    /* "*" */
    {"asymp",                            5, "\xe2\x89\x88",                3},    /* "≈" */
    {"asympeq",                          7, "\xe2\x89\x8d",                3},    /* "≍" */
    {"atilde",                           6, "\xc3\xa3",                    2},    /* "ã" */
    {"auml",                             4, "\xc3\xa4",                    2},    /* "ä" */
    {"awconint",                         8, "\xe2\x88\xb3",                3},    /* "∳" */
    {"awint",                            5, "\xe2\xa8\x91",                3},    /* "⨑" */
    {"bNot",                             4, "\xe2\xab\xad",                3},    /* "⫭" */
    {"backcong",                         8, "\xe2\x89\x8c",                3},    /* "≌" */
    {"backepsilon",                     11, "\xcf\xb6",                    2},    /* "϶" */
    {"backprime",                        9, "\xe2\x80\xb5",                3},    /* "‵" */
    {"backsim",                          7, "\xe2\x88\xbd",                3},    /* "∽" */
    {"backsimeq",                        9, "\xe2\x8b\x8d",                3},    /* "⋍" */
    {"barvee",                           6, "\xe2\x8a\xbd",                3},    /* "⊽" */
    {"barwed",                           6, "\xe2\x8c\x85",                3},    /* "⌅" */
    {"barwedge",                         8, "\xe2\x8c\x85",                3},    /* "⌅" */
    {"bbrk",                             4, "\xe2\x8e\xb5",                3},    /* "⎵" */
    {"bbrktbrk",                         8, "\xe2\x8e\xb6",                3},    /* "⎶" */
    {"bcong",                            5, "\xe2\x89\x8c",                3},    /* "≌" */
    {"bcy",                              3, "\xd0\xb1",                    2},    /* "б" */
    {"bdquo",                            5, "\xe2\x80\x9e",                3},    /* "„" */
    {"becaus",                           6, "\xe2\x88\xb5",                3},    /* "∵" */
    {"because",                          7, "\xe2\x88\xb5",                3},    /* "∵" */
    {"bemptyv",                          7, "\xe2\xa6\xb0",                3},    /* "⦰" */
    {"bepsi",                            5, "\xcf\xb6",                    2},    /* "϶" */
    {"bernou",                           6, "\xe2\x84\xac",                3},    /* "ℬ" */
    {"beta",                             4, "\xce\xb2",                    2},    /* "β" */
    {"beth",                             4, "\xe2\x84\xb6",                3},    /* "ℶ" */
    {"between",                          7, "\xe2\x89\xac",                3},    /* "≬" */
    {"bfr",                              3, "\xf0\x9d\x94\x9f",            4},    /* "𝔟" */
    {"bigcap",                           6, "\xe2\x8b\x82",                3},    /* "⋂" */
    {"bigcirc",                          7, "\xe2\x97\xaf",                3},    /* "◯" */
    {"bigcup",                           6, "\xe2\x8b\x83",                3},    /* "⋃" */
    {"bigodot",                          7, "\xe2\xa8\x80",                3},    /* "⨀" */
    {"bigoplus",                         8, "\xe2\xa8\x81",                3},    /* "⨁" */
    {"bigotimes",                        9, "\xe2\xa8\x82",                3},    /* "⨂" */
    {"bigsqcup",                         8, "\xe2\xa8\x86",                3},    /* "⨆" */
    {"bigstar",                          7, "\xe2\x98\x85",                3},    /* "★" */
    {"bigtriangledown",                 15, "\xe2\x96\xbd",                3},    /* "▽" */
    {"bigtriangleup",                   13, "\xe2\x96\xb3",                3},    /* "△" */
    {"biguplus",                         8, "\xe2\xa8\x84",                3},    /* "⨄" */
    {"bigvee",                           6, "\xe2\x8b\x81",                3},    /* "⋁" */
    {"bigwedge",                         8, "\xe2\x8b\x80",                3},    /* "⋀" */
    {"bkarow",                           6, "\xe2\xa4\x8d",                3},    /* "⤍" */
    {"blacklozenge",                    12, "\xe2\xa7\xab",                3},    /* "⧫" */
    {"blacksquare",                     11, "\xe2\x96\xaa",                3},    /* "▪" */
    {"blacktriangle",                   13, "\xe2\x96\xb4",                3},    /* "▴" */
    {"blacktriangledown",               17, "\xe2\x96\xbe",                3},    /* "▾" */
    {"blacktriangleleft",               17, "\xe2\x97\x82",                3},    /* "◂" */
    {"blacktriangleright",              18, "\xe2\x96\xb8",                3},    /* "▸" */
    {"blank",                            5, "\xe2\x90\xa3",                3},    /* "␣" */
    {"blk12",                            5, "\xe2\x96\x92",                3},    /* "▒" */
    {"blk14",                            5, "\xe2\x96\x91",                3},    /* "░" */
    {"blk34",                            5, "\xe2\x96\x93",                3},    /* "▓" */
    {"block",                            5, "\xe2\x96\x88",                3},    /* "█" */
    {"bne",                              3, "\x3d\xe2\x83\xa5",            4},    /* "=⃥" */
    {"bnequiv",                          7, "\xe2\x89\xa1\xe2\x83\xa5",    6},    /* "≡⃥" */
    {"bnot",                             4, "\xe2\x8c\x90",                3},    /* "⌐" */
    {"bopf",                             4, "\xf0\x9d\x95\x93",            4},    /* "𝕓" */
    {"bot",                              3, "\xe2\x8a\xa5",                3},    /* "⊥" */
    {"bottom",                           6, "\xe2\x8a\xa5",                3},    /* "⊥" */
    {"bowtie",                           6, "\xe2\x8b\x88",                3},    /* "⋈" */
    {"boxDL",                            5, "\xe2\x95\x97",                3},    /* "╗" */
    {"boxDR",                            5, "\xe2\x95\x94",                3},    /* "╔" */
    {"boxDl",                            5, "\xe2\x95\x96",                3},    /* "╖" */
    {"boxDr",                            5, "\xe2\x95\x93",                3},    /* "╓" */
    {"boxH",                             4, "\xe2\x95\x90",                3},    /* "═" */
    {"boxHD",                            5, "\xe2\x95\xa6",                3},    /* "╦" */
    {"boxHU",                            5, "\xe2\x95\xa9",                3},    /* "╩" */
    {"boxHd",                            5, "\xe2\x95\xa4",                3},    /* "╤" */
    {"boxHu",                            5, "\xe2\x95\xa7",                3},    /* "╧" */
    {"boxUL",                            5, "\xe2\x95\x9d",                3},    /* "╝" */
    {"boxUR",                            5, "\xe2\x95\x9a",                3},    /* "╚" */
    {"boxUl",                            5, "\xe2\x95\x9c",                3},    /* "╜" */
    {"boxUr",                            5, "\xe2\x95\x99",                3},    /* "╙" */
    {"boxV",                             4, "\xe2\x95\x91",                3},    /* "║" */
    {"boxVH",                            5, "\xe2\x95\xac",                3},    /* "╬" */
    {"boxVL",                            5, "\xe2\x95\xa3",                3},    /* "╣" */
    {"boxVR",                            5, "\xe2\x95\xa0",                3},    /* "╠" */
    {"boxVh",                            5, "\xe2\x95\xab",                3},    /* "╫" */
    {"boxVl",                            5, "\xe2\x95\xa2",                3},    /* "╢" */
    {"boxVr",                            5, "\xe2\x95\x9f",                3},    /* "╟" */
    {"boxbox",                           6, "\xe2\xa7\x89",                3},    /* "⧉" */
    {"boxdL",                            5, "\xe2\x95\x95",                3},    /* "╕" */
    {"boxdR",                            5, "\xe2\x95\x92",                3},    /* "╒" */
    {"boxdl",                            5, "\xe2\x94\x90",                3},    /* "┐" */
    {"boxdr",                            5, "\xe2\x94\x8c",                3},    /* "┌" */
    {"boxh",                             4, "\xe2\x94\x80",                3},    /* "─" */
    {"boxhD",                            5, "\xe2\x95\xa5",                3},    /* "╥" */
    {"boxhU",                            5, "\xe2\x95\xa8",                3},    /* "╨" */
    {"boxhd",                            5, "\xe2\x94\xac",                3},    /* "┬" */
    {"boxhu",                            5, "\xe2\x94\xb4",                3},    /* "┴" */
    {"boxminus",                         8, "\xe2\x8a\x9f",                3},    /* "⊟" */
    {"boxplus",                          7, "\xe2\x8a\x9e",                3},    /* "⊞" */
    {"boxtimes",                         8, "\xe2\x8a\xa0",                3},    /* "⊠" */
    {"boxuL",                            5, "\xe2\x95\x9b",                3},    /* "╛" */
    {"boxuR",                            5, "\xe2\x95\x98",                3},    /* "╘" */
    {"boxul",                            5, "\xe2\x94\x98",                3},    /* "┘" */
    {"boxur",                            5, "\xe2\x94\x94",                3},    /* "└" */
    {"boxv",                             4, "\xe2\x94\x82",                3},    /* "│" */
    {"boxvH",                            5, "\xe2\x95\xaa",                3},    /* "╪" */
    {"boxvL",                            5, "\xe2\x95\xa1",                3},    /* "╡" */
    {"boxvR",                            5, "\xe2\x95\x9e",                3},    /* "╞" */
    {"boxvh",                            5, "\xe2\x94\xbc",                3},    /* "┼" */
    {"boxvl",                            5, "\xe2\x94\xa4",                3},    /* "┤" */
    {"boxvr",                            5, "\xe2\x94\x9c",                3},    /* "├" */
    {"bprime",                           6, "\xe2\x80\xb5",                3},    /* "‵" */
    {"breve",                            5, "\xcb\x98",                    2},    /* "˘" */
    {"brvbar",                           6, "\xc2\xa6",                    2},    /* "¦" */
    {"bscr",                             4, "\xf0\x9d\x92\xb7",            4},    /* "𝒷" */
    {"bsemi",                            5, "\xe2\x81\x8f",                3},    /* "⁏" */
    {"bsim",                             4, "\xe2\x88\xbd",                3},    /* "∽" */
    {"bsime",                            5, "\xe2\x8b\x8d",                3},    /* "⋍" */
    {"bsol",                             4, "\x5c",                        1},    /* "\" */
    {"bsolb",                            5, "\xe2\xa7\x85",                3},    /* "⧅" */
    {"bsolhsub",                         8, "\xe2\x9f\x88",                3},    /* "⟈" */
    {"bull",                             4, "\xe2\x80\xa2",                3},    /* "•" */
    {"bullet",                           6, "\xe2\x80\xa2",                3},    /* "•" */
    {"bump",                             4, "\xe2\x89\x8e",                3},    /* "≎" */
    {"bumpE",                            5, "\xe2\xaa\xae",                3},    /* "⪮" */
    {"bumpe",                            5, "\xe2\x89\x8f",                3},    /* "≏" */
    {"bumpeq",                           6, "\xe2\x89\x8f",                3},    /* "≏" */
    {"cacute",                           6, "\xc4\x87",                    2},    /* "ć" */
    {"cap",                              3, "\xe2\x88\xa9",                3},    /* "∩" */
    {"capand",                           6, "\xe2\xa9\x84",                3},    /* "⩄" */
    {"capbrcup",                         8, "\xe2\xa9\x89",                3},    /* "⩉" */
    {"capcap",                           6, "\xe2\xa9\x8b",                3},    /* "⩋" */
    {"capcup",                           6, "\xe2\xa9\x87",                3},    /* "⩇" */
    {"capdot",                           6, "\xe2\xa9\x80",                3},    /* "⩀" */
    {"caps",                             4, "\xe2\x88\xa9\xef\xb8\x80",    6},    /* "∩︀" */
    {"caret",                            5, "\xe2\x81\x81",                3},    /* "⁁" */
    {"caron",                            5, "\xcb\x87",                    2},    /* "ˇ" */
    {"ccaps",                            5, "\xe2\xa9\x8d",                3},    /* "⩍" */
    {"ccaron",                           6, "\xc4\x8d",                    2},    /* "č" */
    {"ccedil",                           6, "\xc3\xa7",                    2},    /* "ç" */
    {"ccirc",                            5, "\xc4\x89",                    2},    /* "ĉ" */
    {"ccups",                            5, "\xe2\xa9\x8c",                3},    /* "⩌" */
    {"ccupssm",                          7, "\xe2\xa9\x90",                3},    /* "⩐" */
    {"cdot",                             4, "\xc4\x8b",                    2},    /* "ċ" */
    {"cedil",                            5, "\xc2\xb8",                    2},    /* "¸" */
    {"cemptyv",                          7, "\xe2\xa6\xb2",                3},    /* "⦲" */
    {"cent",                             4, "\xc2\xa2",                    2},    /* "¢" */
    {"centerdot",                        9, "\xc2\xb7",                    2},    /* "·" */
    {"cfr",                              3, "\xf0\x9d\x94\xa0",            4},    /* "𝔠" */
    {"chcy",                             4, "\xd1\x87",                    2},    /* "ч" */
    {"check",                            5, "\xe2\x9c\x93",                3},    /* "✓" */
    {"checkmark",                        9, "\xe2\x9c\x93",                3},    /* "✓" */
    {"chi",                              3, "\xcf\x87",                    2},    /* "χ" */
    {"cir",                              3, "\xe2\x97\x8b",                3},    /* "○" */
    {"cirE",                             4, "\xe2\xa7\x83",                3},    /* "⧃" */
    {"circ",                             4, "\xcb\x86",                    2},    /* "ˆ" */
    {"circeq",                           6, "\xe2\x89\x97",                3},    /* "≗" */
    {"circlearrowleft",                 15, "\xe2\x86\xba",                3},    /* "↺" */
    {"circlearrowright",                16, "\xe2\x86\xbb",                3},    /* "↻" */
    {"circledR",                         8, "\xc2\xae",                    2},    /* "®" */
    {"circledS",                         8, "\xe2\x93\x88",                3},    /* "Ⓢ" */
    {"circledast",                      10, "\xe2\x8a\x9b",                3},    /* "⊛" */
    {"circledcirc",                     11, "\xe2\x8a\x9a",                3},    /* "⊚" */
    {"circleddash",                     11, "\xe2\x8a\x9d",                3},    /* "⊝" */
    {"cire",                             4, "\xe2\x89\x97",                3},    /* "≗" */
    {"cirfnint",                         8, "\xe2\xa8\x90",                3},    /* "⨐" */
    {"cirmid",                           6, "\xe2\xab\xaf",                3},    /* "⫯" */
    {"cirscir",                          7, "\xe2\xa7\x82",                3},    /* "⧂" */
    {"clubs",                            5, "\xe2\x99\xa3",                3},    /* "♣" */
    {"clubsuit",                         8, "\xe2\x99\xa3",                3},    /* "♣" */
    {"colon",                            5, "\x3a",                        1},    /* ":" */
    {"colone",                           6, "\xe2\x89\x94",                3},    /* "≔" */
    {"coloneq",                          7, "\xe2\x89\x94",                3},    /* "≔" */
    {"comma",                            5, "\x2c",                        1},    /* "," */
    {"commat",                           6, "\x40",                        1},    /* "@" */
    {"comp",                             4, "\xe2\x88\x81",                3},    /* "∁" */
    {"compfn",                           6, "\xe2\x88\x98",                3},    /* "∘" */
    {"complement",                      10, "\xe2\x88\x81",                3},    /* "∁" */
    {"complexes",                        9, "\xe2\x84\x82",                3},    /* "ℂ" */
    {"cong",                             4, "\xe2\x89\x85",                3},    /* "≅" */
    {"congdot",                          7, "\xe2\xa9\xad",                3},    /* "⩭" */
    {"conint",                           6, "\xe2\x88\xae",                3},    /* "∮" */
    {"copf",                             4, "\xf0\x9d\x95\x94",            4},    /* "𝕔" */
    {"coprod",                           6, "\xe2\x88\x90",                3},    /* "∐" */
    {"copy",                             4, "\xc2\xa9",                    2},    /* "©" */
    {"copysr",                           6, "\xe2\x84\x97",                3},    /* "℗" */
    {"crarr",                            5, "\xe2\x86\xb5",                3},    /* "↵" */
    {"cross",                            5, "\xe2\x9c\x97",                3},    /* "✗" */
    {"cscr",                             4, "\xf0\x9d\x92\xb8",            4},    /* "𝒸" */
    {"csub",                             4, "\xe2\xab\x8f",                3},    /* "⫏" */
    {"csube",                            5, "\xe2\xab\x91",                3},    /* "⫑" */
    {"csup",                             4, "\xe2\xab\x90",                3},    /* "⫐" */
    {"csupe",                            5, "\xe2\xab\x92",                3},    /* "⫒" */
    {"ctdot",                            5, "\xe2\x8b\xaf",                3},    /* "⋯" */
    {"cudarrl",                          7, "\xe2\xa4\xb8",                3},    /* "⤸" */
    {"cudarrr",                          7, "\xe2\xa4\xb5",                3},    /* "⤵" */
    {"cuepr",                            5, "\xe2\x8b\x9e",                3},    /* "⋞" */
    {"cuesc",                            5, "\xe2\x8b\x9f",                3},    /* "⋟" */
    {"cularr",                           6, "\xe2\x86\xb6",                3},    /* "↶" */
    {"cularrp",                          7, "\xe2\xa4\xbd",                3},    /* "⤽" */
    {"cup",                              3, "\xe2\x88\xaa",                3},    /* "∪" */
    {"cupbrcap",                         8, "\xe2\xa9\x88",                3},    /* "⩈" */
    {"cupcap",                           6, "\xe2\xa9\x86",                3},    /* "⩆" */
    {"cupcup",                           6, "\xe2\xa9\x8a",                3},    /* "⩊" */
    {"cupdot",                           6, "\xe2\x8a\x8d",                3},    /* "⊍" */
    {"cupor",                            5, "\xe2\xa9\x85",                3},    /* "⩅" */
    {"cups",                             4, "\xe2\x88\xaa\xef\xb8\x80",    6},    /* "∪︀" */
    {"curarr",                           6, "\xe2\x86\xb7",                3},    /* "↷" */
    {"curarrm",                          7, "\xe2\xa4\xbc",                3},    /* "⤼" */
    {"curlyeqprec",                     11, "\xe2\x8b\x9e",                3},    /* "⋞" */
    {"curlyeqsucc",                     11, "\xe2\x8b\x9f",                3},    /* "⋟" */
    {"curlyvee",                         8, "\xe2\x8b\x8e",                3},    /* "⋎" */
    {"curlywedge",                      10, "\xe2\x8b\x8f",                3},    /* "⋏" */
    {"curren",                           6, "\xc2\xa4",                    2},    /* "¤" */
    {"curvearrowleft",                  14, "\xe2\x86\xb6",                3},    /* "↶" */
    {"curvearrowright",                 15, "\xe2\x86\xb7",                3},    /* "↷" */
    {"cuvee",                            5, "\xe2\x8b\x8e",                3},    /* "⋎" */
    {"cuwed",                            5, "\xe2\x8b\x8f",                3},    /* "⋏" */
    {"cwconint",                         8, "\xe2\x88\xb2",                3},    /* "∲" */
    {"cwint",                            5, "\xe2\x88\xb1",                3},    /* "∱" */
    {"cylcty",                           6, "\xe2\x8c\xad",                3},    /* "⌭" */
    {"dArr",                             4, "\xe2\x87\x93",                3},    /* "⇓" */
    {"dHar",                             4, "\xe2\xa5\xa5",                3},    /* "⥥" */
    {"dagger",                           6, "\xe2\x80\xa0",                3},    /* "†" */
    {"daleth",                           6, "\xe2\x84\xb8",                3},    /* "ℸ" */
    {"darr",                             4, "\xe2\x86\x93",                3},    /* "↓" */
    {"dash",                             4, "\xe2\x80\x90",                3},    /* "‐" */
    {"dashv",                            5, "\xe2\x8a\xa3",                3},    /* "⊣" */
    {"dbkarow",                          7, "\xe2\xa4\x8f",                3},    /* "⤏" */
    {"dblac",                            5, "\xcb\x9d",                    2},    /* "˝" */
    {"dcaron",                           6, "\xc4\x8f",                    2},    /* "ď" */
    {"dcy",                              3, "\xd0\xb4",                    2},    /* "д" */
    {"dd",                               2, "\xe2\x85\x86",                3},    /* "ⅆ" */
    {"ddagger",                          7, "\xe2\x80\xa1",                3},    /* "‡" */
    {"ddarr",                            5, "\xe2\x87\x8a",                3},    /* "⇊" */
    {"ddotseq",                          7, "\xe2\xa9\xb7",                3},    /* "⩷" */
    {"deg",                              3, "\xc2\xb0",                    2},    /* "°" */
    {"delta",                            5, "\xce\xb4",                    2},    /* "δ" */
    {"demptyv",                          7, "\xe2\xa6\xb1",                3},    /* "⦱" */
    {"dfisht",                           6, "\xe2\xa5\xbf",                3},    /* "⥿" */
    {"dfr",                              3, "\xf0\x9d\x94\xa1",            4},    /* "𝔡" */
    {"dharl",                            5, "\xe2\x87\x83",                3},    /* "⇃" */
    {"dharr",                            5, "\xe2\x87\x82",                3},    /* "⇂" */
    {"diam",                             4, "\xe2\x8b\x84",                3},    /* "⋄" */
    {"diamond",                          7, "\xe2\x8b\x84",                3},    /* "⋄" */
    {"diamondsuit",                     11, "\xe2\x99\xa6",                3},    /* "♦" */
    {"diams",                            5, "\xe2\x99\xa6",                3},    /* "♦" */
    {"die",                              3, "\xc2\xa8",                    2},    /* "¨" */
    {"digamma",                          7, "\xcf\x9d",                    2},    /* "ϝ" */
    {"disin",                            5, "\xe2\x8b\xb2",                3},    /* "⋲" */
    {"div",                              3, "\xc3\xb7",                    2},    /* "÷" */
    {"divide",                           6, "\xc3\xb7",                    2},    /* "÷" */
    {"divideontimes",                   13, "\xe2\x8b\x87",                3},    /* "⋇" */
    {"divonx",                           6, "\xe2\x8b\x87",                3},    /* "⋇" */
    {"djcy",                             4, "\xd1\x92",                    2},    /* "ђ" */
    {"dlcorn",                           6, "\xe2\x8c\x9e",                3},    /* "⌞" */
    {"dlcrop",                           6, "\xe2\x8c\x8d",                3},    /* "⌍" */
    {"dollar",                           6, "\x24",                        1},    /* "$" */
    {"dopf",                             4, "\xf0\x9d\x95\x95",            4},    /* "𝕕" */
    {"dot",                              3, "\xcb\x99",                    2},    /* "˙" */
    {"doteq",                            5, "\xe2\x89\x90",                3},    /* "≐" */
    {"doteqdot",                         8, "\xe2\x89\x91",                3},    /* "≑" */
    {"dotminus",                         8, "\xe2\x88\xb8",                3},    /* "∸" */
    {"dotplus",                          7, "\xe2\x88\x94",                3},    /* "∔" */
    {"dotsquare",                        9, "\xe2\x8a\xa1",                3},    /* "⊡" */
    {"doublebarwedge",                  14, "\xe2\x8c\x86",                3},    /* "⌆" */
    {"downarrow",                        9, "\xe2\x86\x93",                3},    /* "↓" */
    {"downdownarrows",                  14, "\xe2\x87\x8a",                3},    /* "⇊" */
    {"downharpoonleft",                 15, "\xe2\x87\x83",                3},    /* "⇃" */
    {"downharpoonright",                16, "\xe2\x87\x82",                3},    /* "⇂" */
    {"drbkarow",                         8, "\xe2\xa4\x90",                3},    /* "⤐" */
    {"drcorn",                           6, "\xe2\x8c\x9f",                3},    /* "⌟" */
    {"drcrop",                           6, "\xe2\x8c\x8c",                3},    /* "⌌" */
    {"dscr",                             4, "\xf0\x9d\x92\xb9",            4},    /* "𝒹" */
    {"dscy",                             4, "\xd1\x95",                    2},    /* "ѕ" */
    {"dsol",                             4, "\xe2\xa7\xb6",                3},    /* "⧶" */
    {"dstrok",                           6, "\xc4\x91",                    2},    /* "đ" */
    {"dtdot",                            5, "\xe2\x8b\xb1",                3},    /* "⋱" */
    {"dtri",                             4, "\xe2\x96\xbf",                3},    /* "▿" */
    {"dtrif",                            5, "\xe2\x96\xbe",                3},    /* "▾" */
    {"duarr",                            5, "\xe2\x87\xb5",                3},    /* "⇵" */
    {"duhar",                            5, "\xe2\xa5\xaf",                3},    /* "⥯" */
    {"dwangle",                          7, "\xe2\xa6\xa6",                3},    /* "⦦" */
    {"dzcy",                             4, "\xd1\x9f",                    2},    /* "џ" */
    {"dzigrarr",                         8, "\xe2\x9f\xbf",                3},    /* "⟿" */
    {"eDDot",                            5, "\xe2\xa9\xb7",                3},    /* "⩷" */
    {"eDot",                             4, "\xe2\x89\x91",                3},    /* "≑" */
    {"eacute",                           6, "\xc3\xa9",                    2},    /* "é" */
    {"easter",                           6, "\xe2\xa9\xae",                3},    /* "⩮" */
    {"ecaron",                           6, "\xc4\x9b",                    2},    /* "ě" */
    {"ecir",                             4, "\xe2\x89\x96",                3},    /* "≖" */
    {"ecirc",                            5, "\xc3\xaa",                    2},    /* "ê" */
    {"ecolon",                           6, "\xe2\x89\x95",                3},    /* "≕" */
    {"ecy",                              3, "\xd1\x8d",                    2},    /* "э" */
    {"edot",                             4, "\xc4\x97",                    2},    /* "ė" */
    {"ee",                               2, "\xe2\x85\x87",                3},    /* "ⅇ" */
    {"efDot",                            5, "\xe2\x89\x92",                3},    /* "≒" */
    {"efr",                              3, "\xf0\x9d\x94\xa2",            4},    /* "𝔢" */
    {"eg",                               2, "\xe2\xaa\x9a",                3},    /* "⪚" */
    {"egrave",                           6, "\xc3\xa8",                    2},    /* "è" */
    {"egs",                              3, "\xe2\xaa\x96",                3},    /* "⪖" */
    {"egsdot",                           6, "\xe2\xaa\x98",                3},    /* "⪘" */
    {"el",                               2, "\xe2\xaa\x99",                3},    /* "⪙" */
    {"elinters",                         8, "\xe2\x8f\xa7",                3},    /* "⏧" */
    {"ell",                              3, "\xe2\x84\x93",                3},    /* "ℓ" */
    {"els",                              3, "\xe2\xaa\x95",                3},    /* "⪕" */
    {"elsdot",                           6, "\xe2\xaa\x97",                3},    /* "⪗" */
    {"emacr",                            5, "\xc4\x93",                    2},    /* "ē" */
    {"empty",                            5, "\xe2\x88\x85",                3},    /* "∅" */
    {"emptyset",                         8, "\xe2\x88\x85",                3},    /* "∅" */
    {"emptyv",                           6, "\xe2\x88\x85",                3},    /* "∅" */
    {"emsp",                             4, "\xe2\x80\x83",                3},    /* " " */
    {"emsp13",                           6, "\xe2\x80\x84",                3},    /* " " */
    {"emsp14",                           6, "\xe2\x80\x85",                3},    /* " " */
    {"eng",                              3, "\xc5\x8b",                    2},    /* "ŋ" */
    {"ensp",                             4, "\xe2\x80\x82",                3},    /* " " */
    {"eogon",                            5, "\xc4\x99",                    2},    /* "ę" */
    {"eopf",                             4, "\xf0\x9d\x95\x96",            4},    /* "𝕖" */
    {"epar",                             4, "\xe2\x8b\x95",                3},    /* "⋕" */
    {"eparsl",                           6, "\xe2\xa7\xa3",                3},    /* "⧣" */
    {"eplus",                            5, "\xe2\xa9\xb1",                3},    /* "⩱" */
    {"epsi",                             4, "\xce\xb5",                    2},    /* "ε" */
    {"epsilon",                          7, "\xce\xb5",                    2},    /* "ε" */
    {"epsiv",                            5, "\xcf\xb5",                    2},    /* "ϵ" */
    {"eqcirc",                           6, "\xe2\x89\x96",                3},    /* "≖" */
    {"eqcolon",                          7, "\xe2\x89\x95",                3},    /* "≕" */
    {"eqsim",                            5, "\xe2\x89\x82",                3},    /* "≂" */
    {"eqslantgtr",                      10, "\xe2\xaa\x96",                3},    /* "⪖" */
    {"eqslantless",                     11, "\xe2\xaa\x95",                3},    /* "⪕" */
    {"equals",                           6, "\x3d",                        1},    /* "=" */
    {"equest",                           6, "\xe2\x89\x9f",                3},    /* "≟" */
    {"equiv",                            5, "\xe2\x89\xa1",                3},    /* "≡" */
    {"equivDD",                          7, "\xe2\xa9\xb8",                3},    /* "⩸" */
    {"eqvparsl",                         8, "\xe2\xa7\xa5",                3},    /* "⧥" */
    {"erDot",                            5, "\xe2\x89\x93",                3},    /* "≓" */
    {"erarr",                            5, "\xe2\xa5\xb1",                3},    /* "⥱" */
    {"escr",                             4, "\xe2\x84\xaf",                3},    /* "ℯ" */
    {"esdot",                            5, "\xe2\x89\x90",                3},    /* "≐" */
    {"esim",                             4, "\xe2\x89\x82",                3},    /* "≂" */
    {"eta",                              3, "\xce\xb7",                    2},    /* "η" */
    {"eth",                              3, "\xc3\xb0",                    2},    /* "ð" */
    {"euml",                             4, "\xc3\xab",                    2},    /* "ë" */
    {"euro",                             4, "\xe2\x82\xac",                3},    /* "€" */
    {"excl",                             4, "\x21",                        1},    /* "!" */
    {"exist",                            5, "\xe2\x88\x83",                3},    /* "∃" */
    {"expectation",                     11, "\xe2\x84\xb0",                3},    /* "ℰ" */
    {"exponentiale",                    12, "\xe2\x85\x87",                3},    /* "ⅇ" */
    {"fallingdotseq",                   13, "\xe2\x89\x92",                3},    /* "≒" */
    {"fcy",                              3, "\xd1\x84",                    2},    /* "ф" */
    {"female",                           6, "\xe2\x99\x80",                3},    /* "♀" */
    {"ffilig",                           6, "\xef\xac\x83",                3},    /* "ﬃ" */
    {"fflig",                            5, "\xef\xac\x80",                3},    /* "ﬀ" */
    {"ffllig",                           6, "\xef\xac\x84",                3},    /* "ﬄ" */
    {"ffr",                              3, "\xf0\x9d\x94\xa3",            4},    /* "𝔣" */
    {"filig",                            5, "\xef\xac\x81",                3},    /* "ﬁ" */
    {"fjlig",                            5, "\x66\x6a",                    2},    /* "fj" */
    {"flat",                             4, "\xe2\x99\xad",                3},    /* "♭" */
    {"fllig",                            5, "\xef\xac\x82",                3},    /* "ﬂ" */
    {"fltns",                            5, "\xe2\x96\xb1",                3},    /* "▱" */
    {"fnof",                             4, "\xc6\x92",                    2},    /* "ƒ" */
    {"fopf",                             4, "\xf0\x9d\x95\x97",            4},    /* "𝕗" */
    {"forall",                           6, "\xe2\x88\x80",                3},    /* "∀" */
    {"fork",                             4, "\xe2\x8b\x94",                3},    /* "⋔" */
    {"forkv",                            5, "\xe2\xab\x99",                3},    /* "⫙" */
    {"fpartint",                         8, "\xe2\xa8\x8d",                3},    /* "⨍" */
    {"frac12",                           6, "\xc2\xbd",                    2},    /* "½" */
    {"frac13",                           6, "\xe2\x85\x93",                3},    /* "⅓" */
    {"frac14",                           6, "\xc2\xbc",                    2},    /* "¼" */
    {"frac15",                           6, "\xe2\x85\x95",                3},    /* "⅕" */
    {"frac16",                           6, "\xe2\x85\x99",                3},    /* "⅙" */
    {"frac18",                           6, "\xe2\x85\x9b",                3},    /* "⅛" */
    {"frac23",                           6, "\xe2\x85\x94",                3},    /* "⅔" */
    {"frac25",                           6, "\xe2\x85\x96",                3},    /* "⅖" */
    {"frac34",                           6, "\xc2\xbe",                    2},    /* "¾" */
    {"frac35",                           6, "\xe2\x85\x97",                3},    /* "⅗" */
    {"frac38",                           6, "\xe2\x85\x9c",                3},    /* "⅜" */
    {"frac45",                           6, "\xe2\x85\x98",                3},    /* "⅘" */
    {"frac56",                           6, "\xe2\x85\x9a",                3},    /* "⅚" */
    {"frac58",                           6, "\xe2\x85\x9d",                3},    /* "⅝" */
    {"frac78",                           6, "\xe2\x85\x9e",                3},    /* "⅞" */
    {"frasl",                            5, "\xe2\x81\x84",                3},    /* "⁄" */
    {"frown",                            5, "\xe2\x8c\xa2",                3},    /* "⌢" */
    {"fscr",                             4, "\xf0\x9d\x92\xbb",            4},    /* "𝒻" */
    {"gE",                               2, "\xe2\x89\xa7",                3},    /* "≧" */
    {"gEl",                              3, "\xe2\xaa\x8c",                3},    /* "⪌" */
    {"gacute",                           6, "\xc7\xb5",                    2},    /* "ǵ" */
    {"gamma",                            5, "\xce\xb3",                    2},    /* "γ" */
    {"gammad",                           6, "\xcf\x9d",                    2},    /* "ϝ" */
    {"gap",                              3, "\xe2\xaa\x86",                3},    /* "⪆" */
    {"gbreve",                           6, "\xc4\x9f",                    2},    /* "ğ" */
    {"gcirc",                            5, "\xc4\x9d",                    2},    /* "ĝ" */
    {"gcy",                              3, "\xd0\xb3",                    2},    /* "г" */
    {"gdot",                             4, "\xc4\xa1",                    2},    /* "ġ" */
    {"ge",                               2, "\xe2\x89\xa5",                3},    /* "≥" */
    {"gel",                              3, "\xe2\x8b\x9b",                3},    /* "⋛" */
    {"geq",                              3, "\xe2\x89\xa5",                3},    /* "≥" */
    {"geqq",                             4, "\xe2\x89\xa7",                3},    /* "≧" */
    {"geqslant",                         8, "\xe2\xa9\xbe",                3},    /* "⩾" */
    {"ges",                              3, "\xe2\xa9\xbe",                3},    /* "⩾" */
    {"gescc",                            5, "\xe2\xaa\xa9",                3},    /* "⪩" */
    {"gesdot",                           6, "\xe2\xaa\x80",                3},    /* "⪀" */
    {"gesdoto",                          7, "\xe2\xaa\x82",                3},    /* "⪂" */
    {"gesdotol",                         8, "\xe2\xaa\x84",                3},    /* "⪄" */
    {"gesl",                             4, "\xe2\x8b\x9b\xef\xb8\x80",    6},    /* "⋛︀" */
    {"gesles",                           6, "\xe2\xaa\x94",                3},    /* "⪔" */
    {"gfr",                              3, "\xf0\x9d\x94\xa4",            4},    /* "𝔤" */
    {"gg",                               2, "\xe2\x89\xab",                3},    /* "≫" */
    {"ggg",                              3, "\xe2\x8b\x99",                3},    /* "⋙" */
    {"gimel",                            5, "\xe2\x84\xb7",                3},    /* "ℷ" */
    {"gjcy",                             4, "\xd1\x93",                    2},    /* "ѓ" */
    {"gl",                               2, "\xe2\x89\xb7",                3},    /* "≷" */
    {"glE",                              3, "\xe2\xaa\x92",                3},    /* "⪒" */
    {"gla",                              3, "\xe2\xaa\xa5",                3},    /* "⪥" */
    {"glj",                              3, "\xe2\xaa\xa4",                3},    /* "⪤" */
    {"gnE",                              3, "\xe2\x89\xa9",                3},    /* "≩" */
    {"gnap",                             4, "\xe2\xaa\x8a",                3},    /* "⪊" */
    {"gnapprox",                         8, "\xe2\xaa\x8a",                3},    /* "⪊" */
    {"gne",                              3, "\xe2\xaa\x88",                3},    /* "⪈" */
    {"gneq",                             4, "\xe2\xaa\x88",                3},    /* "⪈" */
    {"gneqq",                            5, "\xe2\x89\xa9",                3},    /* "≩" */
    {"gnsim",                            5, "\xe2\x8b\xa7",                3},    /* "⋧" */
    {"gopf",                             4, "\xf0\x9d\x95\x98",            4},    /* "𝕘" */
    {"grave",                            5, "\x60",                        1},    /* "`" */
    {"gscr",                             4, "\xe2\x84\x8a",                3},    /* "ℊ" */
    {"gsim",                             4, "\xe2\x89\xb3",                3},    /* "≳" */
    {"gsime",                            5, "\xe2\xaa\x8e",                3},    /* "⪎" */
    {"gsiml",                            5, "\xe2\xaa\x90",                3},    /* "⪐" */
    {"gt",                               2, "\x3e",                        1},    /* ">" */
    {"gtcc",                             4, "\xe2\xaa\xa7",                3},    /* "⪧" */
    {"gtcir",                            5, "\xe2\xa9\xba",                3},    /* "⩺" */
    {"gtdot",                            5, "\xe2\x8b\x97",                3},    /* "⋗" */
    {"gtlPar",                           6, "\xe2\xa6\x95",                3},    /* "⦕" */
    {"gtquest",                          7, "\xe2\xa9\xbc",                3},    /* "⩼" */
    {"gtrapprox",                        9, "\xe2\xaa\x86",                3},    /* "⪆" */
    {"gtrarr",                           6, "\xe2\xa5\xb8",                3},    /* "⥸" */
    {"gtrdot",                           6, "\xe2\x8b\x97",                3},    /* "⋗" */
    {"gtreqless",                        9, "\xe2\x8b\x9b",                3},    /* "⋛" */
    {"gtreqqless",                      10, "\xe2\xaa\x8c",                3},    /* "⪌" */
    {"gtrless",                          7, "\xe2\x89\xb7",                3},    /* "≷" */
    {"gtrsim",                           6, "\xe2\x89\xb3",                3},    /* "≳" */
    {"gvertneqq",                        9, "\xe2\x89\xa9\xef\xb8\x80",    6},    /* "≩︀" */
    {"gvnE",                             4, "\xe2\x89\xa9\xef\xb8\x80",    6},    /* "≩︀" */
    {"hArr",                             4, "\xe2\x87\x94",                3},    /* "⇔" */
    {"hairsp",                           6, "\xe2\x80\x8a",                3},    /* " " */
    {"half",                             4, "\xc2\xbd",                    2},    /* "½" */
    {"hamilt",                           6, "\xe2\x84\x8b",                3},    /* "ℋ" */
    {"hardcy",                           6, "\xd1\x8a",                    2},    /* "ъ" */
    {"harr",                             4, "\xe2\x86\x94",                3},    /* "↔" */
    {"harrcir",                          7, "\xe2\xa5\x88",                3},    /* "⥈" */
    {"harrw",                            5, "\xe2\x86\xad",                3},    /* "↭" */
    {"hbar",                             4, "\xe2\x84\x8f",                3},    /* "ℏ" */
    {"hcirc",                            5, "\xc4\xa5",                    2},    /* "ĥ" */
    {"hearts",                           6, "\xe2\x99\xa5",                3},    /* "♥" */
    {"heartsuit",                        9, "\xe2\x99\xa5",                3},    /* "♥" */
    {"hellip",                           6, "\xe2\x80\xa6",                3},    /* "…" */
    {"hercon",                           6, "\xe2\x8a\xb9",                3},    /* "⊹" */
    {"hfr",                              3, "\xf0\x9d\x94\xa5",            4},    /* "𝔥" */
    {"hksearow",                         8, "\xe2\xa4\xa5",                3},    /* "⤥" */
    {"hkswarow",                         8, "\xe2\xa4\xa6",                3},    /* "⤦" */
    {"hoarr",                            5, "\xe2\x87\xbf",                3},    /* "⇿" */
    {"homtht",                           6, "\xe2\x88\xbb",                3},    /* "∻" */
    {"hookleftarrow",                   13, "\xe2\x86\xa9",                3},    /* "↩" */
    {"hookrightarrow",                  14, "\xe2\x86\xaa",                3},    /* "↪" */
    {"hopf",                             4, "\xf0\x9d\x95\x99",            4},    /* "𝕙" */
    {"horbar",                           6, "\xe2\x80\x95",                3},    /* "―" */
    {"hscr",                             4, "\xf0\x9d\x92\xbd",            4},    /* "𝒽" */
    {"hslash",                           6, "\xe2\x84\x8f",                3},    /* "ℏ" */
    {"hstrok",                           6, "\xc4\xa7",                    2},    /* "ħ" */
    {"hybull",                           6, "\xe2\x81\x83",                3},    /* "⁃" */
    {"hyphen",                           6, "\xe2\x80\x90",                3},    /* "‐" */
    {"iacute",                           6, "\xc3\xad",                    2},    /* "í" */
    {"ic",                               2, "\xe2\x81\xa3",                3},    /* InvisibleComma */
    {"icirc",                            5, "\xc3\xae",                    2},    /* "î" */
    {"icy",                              3, "\xd0\xb8",                    2},    /* "и" */
    {"iecy",                             4, "\xd0\xb5",                    2},    /* "е" */
    {"iexcl",                            5, "\xc2\xa1",                    2},    /* "¡" */
    {"iff",                              3, "\xe2\x87\x94",                3},    /* "⇔" */
    {"ifr",                              3, "\xf0\x9d\x94\xa6",            4},    /* "𝔦" */
    {"igrave",                           6, "\xc3\xac",                    2},    /* "ì" */
    {"ii",                               2, "\xe2\x85\x88",                3},    /* "ⅈ" */
    {"iiiint",                           6, "\xe2\xa8\x8c",                3},    /* "⨌" */
    {"iiint",                            5, "\xe2\x88\xad",                3},    /* "∭" */
    {"iinfin",                           6, "\xe2\xa7\x9c",                3},    /* "⧜" */
    {"iiota",                            5, "\xe2\x84\xa9",                3},    /* "℩" */
    {"ijlig",                            5, "\xc4\xb3",                    2},    /* "ĳ" */
    {"imacr",                            5, "\xc4\xab",                    2},    /* "ī" */
    {"image",                            5, "\xe2\x84\x91",                3},    /* "ℑ" */
    {"imagline",                         8, "\xe2\x84\x90",                3},    /* "ℐ" */
    {"imagpart",                         8, "\xe2\x84\x91",                3},    /* "ℑ" */
    {"imath",                            5, "\xc4\xb1",                    2},    /* "ı" */
    {"imof",                             4, "\xe2\x8a\xb7",                3},    /* "⊷" */
    {"imped",                            5, "\xc6\xb5",                    2},    /* "Ƶ" */
    {"in",                               2, "\xe2\x88\x88",                3},    /* "∈" */
    {"incare",                           6, "\xe2\x84\x85",                3},    /* "℅" */
    {"infin",                            5, "\xe2\x88\x9e",                3},    /* "∞" */
    {"infintie",                         8, "\xe2\xa7\x9d",                3},    /* "⧝" */
    {"inodot",                           6, "\xc4\xb1",                    2},    /* "ı" */
    {"int",                              3, "\xe2\x88\xab",                3},    /* "∫" */
    {"intcal",                           6, "\xe2\x8a\xba",                3},    /* "⊺" */
    {"integers",                         8, "\xe2\x84\xa4",                3},    /* "ℤ" */
    {"intercal",                         8, "\xe2\x8a\xba",                3},    /* "⊺" */
    {"intlarhk",                         8, "\xe2\xa8\x97",                3},    /* "⨗" */
    {"intprod",                          7, "\xe2\xa8\xbc",                3},    /* "⨼" */
    {"iocy",                             4, "\xd1\x91",                    2},    /* "ё" */
    {"iogon",                            5, "\xc4\xaf",                    2},    /* "į" */
    {"iopf",                             4, "\xf0\x9d\x95\x9a",            4},    /* "𝕚" */
    {"iota",                             4, "\xce\xb9",                    2},    /* "ι" */
    {"iprod",                            5, "\xe2\xa8\xbc",                3},    /* "⨼" */
    {"iquest",                           6, "\xc2\xbf",                    2},    /* "¿" */
    {"iscr",                             4, "\xf0\x9d\x92\xbe",            4},    /* "𝒾" */
    {"isin",                             4, "\xe2\x88\x88",                3},    /* "∈" */
    {"isinE",                            5, "\xe2\x8b\xb9",                3},    /* "⋹" */
    {"isindot",                          7, "\xe2\x8b\xb5",                3},    /* "⋵" */
    {"isins",                            5, "\xe2\x8b\xb4",                3},    /* "⋴" */
    {"isinsv",                           6, "\xe2\x8b\xb3",                3},    /* "⋳" */
    {"isinv",                            5, "\xe2\x88\x88",                3},    /* "∈" */
    {"it",                               2, "\xe2\x81\xa2",                3},    /* InvisibleTimes */
    {"itilde",                           6, "\xc4\xa9",                    2},    /* "ĩ" */
    {"iukcy",                            5, "\xd1\x96",                    2},    /* "і" */
    {"iuml",                             4, "\xc3\xaf",                    2},    /* "ï" */
    {"jcirc",                            5, "\xc4\xb5",                    2},    /* "ĵ" */
    {"jcy",                              3, "\xd0\xb9",                    2},    /* "й" */
    {"jfr",                              3, "\xf0\x9d\x94\xa7",            4},    /* "𝔧" */
    {"jmath",                            5, "\xc8\xb7",                    2},    /* "ȷ" */
    {"jopf",                             4, "\xf0\x9d\x95\x9b",            4},    /* "𝕛" */
    {"jscr",                             4, "\xf0\x9d\x92\xbf",            4},    /* "𝒿" */
    {"jsercy",                           6, "\xd1\x98",                    2},    /* "ј" */
    {"jukcy",                            5, "\xd1\x94",                    2},    /* "є" */
    {"kappa",                            5, "\xce\xba",                    2},    /* "κ" */
    {"kappav",                           6, "\xcf\xb0",                    2},    /* "ϰ" */
    {"kcedil",                           6, "\xc4\xb7",                    2},    /* "ķ" */
    {"kcy",                              3, "\xd0\xba",                    2},    /* "к" */
    {"kfr",                              3, "\xf0\x9d\x94\xa8",            4},    /* "𝔨" */
    {"kgreen",                           6, "\xc4\xb8",                    2},    /* "ĸ" */
    {"khcy",                             4, "\xd1\x85",                    2},    /* "х" */
    {"kjcy",                             4, "\xd1\x9c",                    2},    /* "ќ" */
    {"kopf",                             4, "\xf0\x9d\x95\x9c",            4},    /* "𝕜" */
    {"kscr",                             4, "\xf0\x9d\x93\x80",            4},    /* "𝓀" */
    {"lAarr",                            5, "\xe2\x87\x9a",                3},    /* "⇚" */
    {"lArr",                             4, "\xe2\x87\x90",                3},    /* "⇐" */
    {"lAtail",                           6, "\xe2\xa4\x9b",                3},    /* "⤛" */
    {"lBarr",                            5, "\xe2\xa4\x8e",                3},    /* "⤎" */
    {"lE",                               2, "\xe2\x89\xa6",                3},    /* "≦" */
    {"lEg",                              3, "\xe2\xaa\x8b",                3},    /* "⪋" */
    {"lHar",                             4, "\xe2\xa5\xa2",                3},    /* "⥢" */
    {"lacute",                           6, "\xc4\xba",                    2},    /* "ĺ" */
    {"laemptyv",                         8, "\xe2\xa6\xb4",                3},    /* "⦴" */
    {"lagran",                           6, "\xe2\x84\x92",                3},    /* "ℒ" */
    {"lambda",                           6, "\xce\xbb",                    2},    /* "λ" */
    {"lang",                             4, "\xe2\x9f\xa8",                3},    /* "⟨" */
    {"langd",                            5, "\xe2\xa6\x91",                3},    /* "⦑" */
    {"langle",                           6, "\xe2\x9f\xa8",                3},    /* "⟨" */
    {"lap",                              3, "\xe2\xaa\x85",                3},    /* "⪅" */
    {"laquo",                            5, "\xc2\xab",                    2},    /* "«" */
    {"larr",                             4, "\xe2\x86\x90",                3},    /* "←" */
    {"larrb",                            5, "\xe2\x87\xa4",                3},    /* "⇤" */
    {"larrbfs",                          7, "\xe2\xa4\x9f",                3},    /* "⤟" */
    {"larrfs",                           6, "\xe2\xa4\x9d",                3},    /* "⤝" */
    {"larrhk",                           6, "\xe2\x86\xa9",                3},    /* "↩" */
    {"larrlp",                           6, "\xe2\x86\xab",                3},    /* "↫" */
    {"larrpl",                           6, "\xe2\xa4\xb9",                3},    /* "⤹" */
    {"larrsim",                          7, "\xe2\xa5\xb3",                3},    /* "⥳" */
    {"larrtl",                           6, "\xe2\x86\xa2",                3},    /* "↢" */
    {"lat",                              3, "\xe2\xaa\xab",                3},    /* "⪫" */
    {"latail",                           6, "\xe2\xa4\x99",                3},    /* "⤙" */
    {"late",                             4, "\xe2\xaa\xad",                3},    /* "⪭" */
    {"lates",                            5, "\xe2\xaa\xad\xef\xb8\x80",    6},    /* "⪭︀" */
    {"lbarr",                            5, "\xe2\xa4\x8c",                3},    /* "⤌" */
    {"lbbrk",                            5, "\xe2\x9d\xb2",                3},    /* "❲" */
    {"lbrace",                           6, "\x7b",                        1},    /* "{" */
    {"lbrack",                           6, "\x5b",                        1},    /* "[" */
    {"lbrke",                            5, "\xe2\xa6\x8b",                3},    /* "⦋" */
    {"lbrksld",                          7, "\xe2\xa6\x8f",                3},    /* "⦏" */
    {"lbrkslu",                          7, "\xe2\xa6\x8d",                3},    /* "⦍" */
    {"lcaron",                           6, "\xc4\xbe",                    2},    /* "ľ" */
    {"lcedil",                           6, "\xc4\xbc",                    2},    /* "ļ" */
    {"lceil",                            5, "\xe2\x8c\x88",                3},    /* "⌈" */
    {"lcub",                             4, "\x7b",                        1},    /* "{" */
    {"lcy",                              3, "\xd0\xbb",                    2},    /* "л" */
    {"ldca",                             4, "\xe2\xa4\xb6",                3},    /* "⤶" */
    {"ldquo",                            5, "\xe2\x80\x9c",                3},    /* "“" */
    {"ldquor",                           6, "\xe2\x80\x9e",                3},    /* "„" */
    {"ldrdhar",                          7, "\xe2\xa5\xa7",                3},    /* "⥧" */
    {"ldrushar",                         8, "\xe2\xa5\x8b",                3},    /* "⥋" */
    {"ldsh",                             4, "\xe2\x86\xb2",                3},    /* "↲" */
    {"le",                               2, "\xe2\x89\xa4",                3},    /* "≤" */
    {"leftarrow",                        9, "\xe2\x86\x90",                3},    /* "←" */
    {"leftarrowtail",                   13, "\xe2\x86\xa2",                3},    /* "↢" */
    {"leftharpoondown",                 15, "\xe2\x86\xbd",                3},    /* "↽" */
    {"leftharpoonup",                   13, "\xe2\x86\xbc",                3},    /* "↼" */
    {"leftleftarrows",                  14, "\xe2\x87\x87",                3},    /* "⇇" */
    {"leftrightarrow",                  14, "\xe2\x86\x94",                3},    /* "↔" */
    {"leftrightarrows",                 15, "\xe2\x87\x86",                3},    /* "⇆" */
    {"leftrightharpoons",               17, "\xe2\x87\x8b",                3},    /* "⇋" */
    {"leftrightsquigarrow",             19, "\xe2\x86\xad",                3},    /* "↭" */
    {"leftthreetimes",                  14, "\xe2\x8b\x8b",                3},    /* "⋋" */
    {"leg",                              3, "\xe2\x8b\x9a",                3},    /* "⋚" */
    {"leq",                              3, "\xe2\x89\xa4",                3},    /* "≤" */
    {"leqq",                             4, "\xe2\x89\xa6",                3},    /* "≦" */
    {"leqslant",                         8, "\xe2\xa9\xbd",                3},    /* "⩽" */
    {"les",                              3, "\xe2\xa9\xbd",                3},    /* "⩽" */
    {"lescc",                            5, "\xe2\xaa\xa8",                3},    /* "⪨" */
    {"lesdot",                           6, "\xe2\xa9\xbf",                3},    /* "⩿" */
    {"lesdoto",                          7, "\xe2\xaa\x81",                3},    /* "⪁" */
    {"lesdotor",                         8, "\xe2\xaa\x83",                3},    /* "⪃" */
    {"lesg",                             4, "\xe2\x8b\x9a\xef\xb8\x80",    6},    /* "⋚︀" */
    {"lesges",                           6, "\xe2\xaa\x93",                3},    /* "⪓" */
    {"lessapprox",                      10, "\xe2\xaa\x85",                3},    /* "⪅" */
    {"lessdot",                          7, "\xe2\x8b\x96",                3},    /* "⋖" */
    {"lesseqgtr",                        9, "\xe2\x8b\x9a",                3},    /* "⋚" */
    {"lesseqqgtr",                      10, "\xe2\xaa\x8b",                3},    /* "⪋" */
    {"lessgtr",                          7, "\xe2\x89\xb6",                3},    /* "≶" */
    {"lesssim",                          7, "\xe2\x89\xb2",                3},    /* "≲" */
    {"lfisht",                           6, "\xe2\xa5\xbc",                3},    /* "⥼" */
    {"lfloor",                           6, "\xe2\x8c\x8a",                3},    /* "⌊" */
    {"lfr",                              3, "\xf0\x9d\x94\xa9",            4},    /* "𝔩" */
    {"lg",                               2, "\xe2\x89\xb6",                3},    /* "≶" */
    {"lgE",                              3, "\xe2\xaa\x91",                3},    /* "⪑" */
    {"lhard",                            5, "\xe2\x86\xbd",                3},    /* "↽" */
    {"lharu",                            5, "\xe2\x86\xbc",                3},    /* "↼" */
    {"lharul",                           6, "\xe2\xa5\xaa",                3},    /* "⥪" */
    {"lhblk",                            5, "\xe2\x96\x84",                3},    /* "▄" */
    {"ljcy",                             4, "\xd1\x99",                    2},    /* "љ" */
    {"ll",                               2, "\xe2\x89\xaa",                3},    /* "≪" */
    {"llarr",                            5, "\xe2\x87\x87",                3},    /* "⇇" */
    {"llcorner",                         8, "\xe2\x8c\x9e",                3},    /* "⌞" */
    {"llhard",                           6, "\xe2\xa5\xab",                3},    /* "⥫" */
    {"lltri",                            5, "\xe2\x97\xba",                3},    /* "◺" */
    {"lmidot",                           6, "\xc5\x80",                    2},    /* "ŀ" */
    {"lmoust",                           6, "\xe2\x8e\xb0",                3},    /* "⎰" */
    {"lmoustache",                      10, "\xe2\x8e\xb0",                3},    /* "⎰" */
    {"lnE",                              3, "\xe2\x89\xa8",                3},    /* "≨" */
    {"lnap",                             4, "\xe2\xaa\x89",                3},    /* "⪉" */
    {"lnapprox",                         8, "\xe2\xaa\x89",                3},    /* "⪉" */
    {"lne",                              3, "\xe2\xaa\x87",                3},    /* "⪇" */
    {"lneq",                             4, "\xe2\xaa\x87",                3},    /* "⪇" */
    {"lneqq",                            5, "\xe2\x89\xa8",                3},    /* "≨" */
    {"lnsim",                            5, "\xe2\x8b\xa6",                3},    /* "⋦" */
    {"loang",                            5, "\xe2\x9f\xac",                3},    /* "⟬" */
    {"loarr",                            5, "\xe2\x87\xbd",                3},    /* "⇽" */
    {"lobrk",                            5, "\xe2\x9f\xa6",                3},    /* "⟦" */
    {"longleftarrow",                   13, "\xe2\x9f\xb5",                3},    /* "⟵" */
    {"longleftrightarrow",              18, "\xe2\x9f\xb7",                3},    /* "⟷" */
    {"longmapsto",                      10, "\xe2\x9f\xbc",                3},    /* "⟼" */
    {"longrightarrow",                  14, "\xe2\x9f\xb6",                3},    /* "⟶" */
    {"looparrowleft",                   13, "\xe2\x86\xab",                3},    /* "↫" */
    {"looparrowright",                  14, "\xe2\x86\xac",                3},    /* "↬" */
    {"lopar",                            5, "\xe2\xa6\x85",                3},    /* "⦅" */
    {"lopf",                             4, "\xf0\x9d\x95\x9d",            4},    /* "𝕝" */
    {"loplus",                           6, "\xe2\xa8\xad",                3},    /* "⨭" */
    {"lotimes",                          7, "\xe2\xa8\xb4",                3},    /* "⨴" */
    {"lowast",                           6, "\xe2\x88\x97",                3},    /* "∗" */
    {"lowbar",                           6, "\x5f",                        1},    /* "_" */
    {"loz",                              3, "\xe2\x97\x8a",                3},    /* "◊" */
    {"lozenge",                          7, "\xe2\x97\x8a",                3},    /* "◊" */
    {"lozf",                             4, "\xe2\xa7\xab",                3},    /* "⧫" */
    {"lpar",                             4, "\x28",                        1},    /* "(" */
    {"lparlt",                           6, "\xe2\xa6\x93",                3},    /* "⦓" */
    {"lrarr",                            5, "\xe2\x87\x86",                3},    /* "⇆" */
    {"lrcorner",                         8, "\xe2\x8c\x9f",                3},    /* "⌟" */
    {"lrhar",                            5, "\xe2\x87\x8b",                3},    /* "⇋" */
    {"lrhard",                           6, "\xe2\xa5\xad",                3},    /* "⥭" */
    {"lrm",                              3, "\xe2\x80\x8e",                3},    /* lrm */
    {"lrtri",                            5, "\xe2\x8a\xbf",                3},    /* "⊿" */
    {"lsaquo",                           6, "\xe2\x80\xb9",                3},    /* "‹" */
    {"lscr",                             4, "\xf0\x9d\x93\x81",            4},    /* "𝓁" */
    {"lsh",                              3, "\xe2\x86\xb0",                3},    /* "↰" */
    {"lsim",                             4, "\xe2\x89\xb2",                3},    /* "≲" */
    {"lsime",                            5, "\xe2\xaa\x8d",                3},    /* "⪍" */
    {"lsimg",                            5, "\xe2\xaa\x8f",                3},    /* "⪏" */
    {"lsqb",                             4, "\x5b",                        1},    /* "[" */
    {"lsquo",                            5, "\xe2\x80\x98",                3},    /* "‘" */
    {"lsquor",                           6, "\xe2\x80\x9a",                3},    /* "‚" */
    {"lstrok",                           6, "\xc5\x82",                    2},    /* "ł" */
    {"lt",                               2, "\x3c",                        1},    /* "<" */
    {"ltcc",                             4, "\xe2\xaa\xa6",                3},    /* "⪦" */
    {"ltcir",                            5, "\xe2\xa9\xb9",                3},    /* "⩹" */
    {"ltdot",                            5, "\xe2\x8b\x96",                3},    /* "⋖" */
    {"lthree",                           6, "\xe2\x8b\x8b",                3},    /* "⋋" */
    {"ltimes",                           6, "\xe2\x8b\x89",                3},    /* "⋉" */
    {"ltlarr",                           6, "\xe2\xa5\xb6",                3},    /* "⥶" */
    {"ltquest",                          7, "\xe2\xa9\xbb",                3},    /* "⩻" */
    {"ltrPar",                           6, "\xe2\xa6\x96",                3},    /* "⦖" */
    {"ltri",                             4, "\xe2\x97\x83",                3},    /* "◃" */
    {"ltrie",                            5, "\xe2\x8a\xb4",                3},    /* "⊴" */
    {"ltrif",                            5, "\xe2\x97\x82",                3},    /* "◂" */
    {"lurdshar",                         8, "\xe2\xa5\x8a",                3},    /* "⥊" */
    {"luruhar",                          7, "\xe2\xa5\xa6",                3},    /* "⥦" */
    {"lvertneqq",                        9, "\xe2\x89\xa8\xef\xb8\x80",    6},    /* "≨︀" */
    {"lvnE",                             4, "\xe2\x89\xa8\xef\xb8\x80",    6},    /* "≨︀" */
    {"mDDot",                            5, "\xe2\x88\xba",                3},    /* "∺" */
    {"macr",                             4, "\xc2\xaf",                    2},    /* "¯" */
    {"male",                             4, "\xe2\x99\x82",                3},    /* "♂" */
    {"malt",                             4, "\xe2\x9c\xa0",                3},    /* "✠" */
    {"maltese",                          7, "\xe2\x9c\xa0",                3},    /* "✠" */
    {"map",                              3, "\xe2\x86\xa6",                3},    /* "↦" */
    {"mapsto",                           6, "\xe2\x86\xa6",                3},    /* "↦" */
    {"mapstodown",                      10, "\xe2\x86\xa7",                3},    /* "↧" */
    {"mapstoleft",                      10, "\xe2\x86\xa4",                3},    /* "↤" */
    {"mapstoup",                         8, "\xe2\x86\xa5",                3},    /* "↥" */
    {"marker",                           6, "\xe2\x96\xae",                3},    /* "▮" */
    {"mcomma",                           6, "\xe2\xa8\xa9",                3},    /* "⨩" */
    {"mcy",                              3, "\xd0\xbc",                    2},    /* "м" */
    {"mdash",                            5, "\xe2\x80\x94",                3},    /* "—" */
    {"measuredangle",                   13, "\xe2\x88\xa1",                3},    /* "∡" */
    {"mfr",                              3, "\xf0\x9d\x94\xaa",            4},    /* "𝔪" */
    {"mho",                              3, "\xe2\x84\xa7",                3},    /* "℧" */
    {"micro",                            5, "\xc2\xb5",                    2},    /* "µ" */
    {"mid",                              3, "\xe2\x88\xa3",                3},    /* "∣" */
    {"midast",                           6, "\x2a",                        1},    /* "*" */
    {"midcir",                           6, "\xe2\xab\xb0",                3},    /* "⫰" */
    {"middot",                           6, "\xc2\xb7",                    2},    /* "·" */
    {"minus",                            5, "\xe2\x88\x92",                3},    /* "−" */
    {"minusb",                           6, "\xe2\x8a\x9f",                3},    /* "⊟" */
    {"minusd",                           6, "\xe2\x88\xb8",                3},    /* "∸" */
    {"minusdu",                          7, "\xe2\xa8\xaa",                3},    /* "⨪" */
    {"mlcp",                             4, "\xe2\xab\x9b",                3},    /* "⫛" */
    {"mldr",                             4, "\xe2\x80\xa6",                3},    /* "…" */
    {"mnplus",                           6, "\xe2\x88\x93",                3},    /* "∓" */
    {"models",                           6, "\xe2\x8a\xa7",                3},    /* "⊧" */
    {"mopf",                             4, "\xf0\x9d\x95\x9e",            4},    /* "𝕞" */
    {"mp",                               2, "\xe2\x88\x93",                3},    /* "∓" */
    {"mscr",                             4, "\xf0\x9d\x93\x82",            4},    /* "𝓂" */
    {"mstpos",                           6, "\xe2\x88\xbe",                3},    /* "∾" */
    {"mu",                               2, "\xce\xbc",                    2},    /* "μ" */
    {"multimap",                         8, "\xe2\x8a\xb8",                3},    /* "⊸" */
    {"mumap",                            5, "\xe2\x8a\xb8",                3},    /* "⊸" */
    {"nGg",                              3, "\xe2\x8b\x99\xcc\xb8",        5},    /* "⋙̸" */
    {"nGt",                              3, "\xe2\x89\xab\xe2\x83\x92",    6},    /* "≫⃒" */
    {"nGtv",                             4, "\xe2\x89\xab\xcc\xb8",        5},    /* "≫̸" */
    {"nLeftarrow",                      10, "\xe2\x87\x8d",                3},    /* "⇍" */
    {"nLeftrightarrow",                 15, "\xe2\x87\x8e",                3},    /* "⇎" */
    {"nLl",                              3, "\xe2\x8b\x98\xcc\xb8",        5},    /* "⋘̸" */
    {"nLt",                              3, "\xe2\x89\xaa\xe2\x83\x92",    6},    /* "≪⃒" */
    {"nLtv",                             4, "\xe2\x89\xaa\xcc\xb8",        5},    /* "≪̸" */
    {"nRightarrow",                     11, "\xe2\x87\x8f",                3},    /* "⇏" */
    {"nVDash",                           6, "\xe2\x8a\xaf",                3},    /* "⊯" */
    {"nVdash",                           6, "\xe2\x8a\xae",                3},    /* "⊮" */
    {"nabla",                            5, "\xe2\x88\x87",                3},    /* "∇" */
    {"nacute",                           6, "\xc5\x84",                    2},    /* "ń" */
    {"nang",                             4, "\xe2\x88\xa0\xe2\x83\x92",    6},    /* "∠⃒" */
    {"nap",                              3, "\xe2\x89\x89",                3},    /* "≉" */
    {"napE",                             4, "\xe2\xa9\xb0\xcc\xb8",        5},    /* "⩰̸" */
    {"napid",                            5, "\xe2\x89\x8b\xcc\xb8",        5},    /* "≋̸" */
    {"napos",                            5, "\xc5\x89",                    2},    /* "ŉ" */
    {"napprox",                          7, "\xe2\x89\x89",                3},    /* "≉" */
    {"natur",                            5, "\xe2\x99\xae",                3},    /* "♮" */
    {"natural",                          7, "\xe2\x99\xae",                3},    /* "♮" */
    {"naturals",                         8, "\xe2\x84\x95",                3},    /* "ℕ" */
    {"nbsp",                             4, "\xc2\xa0",                    2},    /* " " */
    {"nbump",                            5, "\xe2\x89\x8e\xcc\xb8",        5},    /* "≎̸" */
    {"nbumpe",                           6, "\xe2\x89\x8f\xcc\xb8",        5},    /* "≏̸" */
    {"ncap",                             4, "\xe2\xa9\x83",                3},    /* "⩃" */
    {"ncaron",                           6, "\xc5\x88",                    2},    /* "ň" */
    {"ncedil",                           6, "\xc5\x86",                    2},    /* "ņ" */
    {"ncong",                            5, "\xe2\x89\x87",                3},    /* "≇" */
    {"ncongdot",                         8, "\xe2\xa9\xad\xcc\xb8",        5},    /* "⩭̸" */
    {"ncup",                             4, "\xe2\xa9\x82",                3},    /* "⩂" */
    {"ncy",                              3, "\xd0\xbd",                    2},    /* "н" */
    {"ndash",                            5, "\xe2\x80\x93",                3},    /* "–" */
    {"ne",                               2, "\xe2\x89\xa0",                3},    /* "≠" */
    {"neArr",                            5, "\xe2\x87\x97",                3},    /* "⇗" */
    {"nearhk",                           6, "\xe2\xa4\xa4",                3},    /* "⤤" */
    {"nearr",                            5, "\xe2\x86\x97",                3},    /* "↗" */
    {"nearrow",                          7, "\xe2\x86\x97",                3},    /* "↗" */
    {"nedot",                            5, "\xe2\x89\x90\xcc\xb8",        5},    /* "≐̸" */
    {"nequiv",                           6, "\xe2\x89\xa2",                3},    /* "≢" */
    {"nesear",                           6, "\xe2\xa4\xa8",                3},    /* "⤨" */
    {"nesim",                            5, "\xe2\x89\x82\xcc\xb8",        5},    /* "≂̸" */
    {"nexist",                           6, "\xe2\x88\x84",                3},    /* "∄" */
    {"nexists",                          7, "\xe2\x88\x84",                3},    /* "∄" */
    {"nfr",                              3, "\xf0\x9d\x94\xab",            4},    /* "𝔫" */
    {"ngE",                              3, "\xe2\x89\xa7\xcc\xb8",        5},    /* "≧̸" */
    {"nge",                              3, "\xe2\x89\xb1",                3},    /* "≱" */
    {"ngeq",                             4, "\xe2\x89\xb1",                3},    /* "≱" */
    {"ngeqq",                            5, "\xe2\x89\xa7\xcc\xb8",        5},    /* "≧̸" */
    {"ngeqslant",                        9, "\xe2\xa9\xbe\xcc\xb8",        5},    /* "⩾̸" */
    {"nges",                             4, "\xe2\xa9\xbe\xcc\xb8",        5},    /* "⩾̸" */
    {"ngsim",                            5, "\xe2\x89\xb5",                3},    /* "≵" */
    {"ngt",                              3, "\xe2\x89\xaf",                3},    /* "≯" */
    {"ngtr",                             4, "\xe2\x89\xaf",                3},    /* "≯" */
    {"nhArr",                            5, "\xe2\x87\x8e",                3},    /* "⇎" */
    {"nharr",                            5, "\xe2\x86\xae",                3},    /* "↮" */
    {"nhpar",                            5, "\xe2\xab\xb2",                3},    /* "⫲" */
    {"ni",                               2, "\xe2\x88\x8b",                3},    /* "∋" */
    {"nis",                              3, "\xe2\x8b\xbc",                3},    /* "⋼" */
    {"nisd",                             4, "\xe2\x8b\xba",                3},    /* "⋺" */
    {"niv",                              3, "\xe2\x88\x8b",                3},    /* "∋" */
    {"njcy",                             4, "\xd1\x9a",                    2},    /* "њ" */
    {"nlArr",                            5, "\xe2\x87\x8d",                3},    /* "⇍" */
    {"nlE",                              3, "\xe2\x89\xa6\xcc\xb8",        5},    /* "≦̸" */
    {"nlarr",                            5, "\xe2\x86\x9a",                3},    /* "↚" */
    {"nldr",                             4, "\xe2\x80\xa5",                3},    /* "‥" */
    {"nle",                              3, "\xe2\x89\xb0",                3},    /* "≰" */
    {"nleftarrow",                      10, "\xe2\x86\x9a",                3},    /* "↚" */
    {"nleftrightarrow",                 15, "\xe2\x86\xae",                3},    /* "↮" */
    {"nleq",                             4, "\xe2\x89\xb0",                3},    /* "≰" */
    {"nleqq",                            5, "\xe2\x89\xa6\xcc\xb8",        5},    /* "≦̸" */
    {"nleqslant",                        9, "\xe2\xa9\xbd\xcc\xb8",        5},    /* "⩽̸" */
    {"nles",                             4, "\xe2\xa9\xbd\xcc\xb8",        5},    /* "⩽̸" */
    {"nless",                            5, "\xe2\x89\xae",                3},    /* "≮" */
    {"nlsim",                            5, "\xe2\x89\xb4",                3},    /* "≴" */
    {"nlt",                              3, "\xe2\x89\xae",                3},    /* "≮" */
    {"nltri",                            5, "\xe2\x8b\xaa",                3},    /* "⋪" */
    {"nltrie",                           6, "\xe2\x8b\xac",                3},    /* "⋬" */
    {"nmid",                             4, "\xe2\x88\xa4",                3},    /* "∤" */
    {"nopf",                             4, "\xf0\x9d\x95\x9f",            4},    /* "𝕟" */
    {"not",                              3, "\xc2\xac",                    2},    /* "¬" */
    {"notin",                            5, "\xe2\x88\x89",                3},    /* "∉" */
    {"notinE",                           6, "\xe2\x8b\xb9\xcc\xb8",        5},    /* "⋹̸" */
    {"notindot",                         8, "\xe2\x8b\xb5\xcc\xb8",        5},    /* "⋵̸" */
    {"notinva",                          7, "\xe2\x88\x89",                3},    /* "∉" */
    {"notinvb",                          7, "\xe2\x8b\xb7",                3},    /* "⋷" */
    {"notinvc",                          7, "\xe2\x8b\xb6",                3},    /* "⋶" */
    {"notni",                            5, "\xe2\x88\x8c",                3},    /* "∌" */
    {"notniva",                          7, "\xe2\x88\x8c",                3},    /* "∌" */
    {"notnivb",                          7, "\xe2\x8b\xbe",                3},    /* "⋾" */
    {"notnivc",                          7, "\xe2\x8b\xbd",                3},    /* "⋽" */
    {"npar",                             4, "\xe2\x88\xa6",                3},    /* "∦" */
    {"nparallel",                        9, "\xe2\x88\xa6",                3},    /* "∦" */
    {"nparsl",                           6, "\xe2\xab\xbd\xe2\x83\xa5",    6},    /* "⫽⃥" */
    {"npart",                            5, "\xe2\x88\x82\xcc\xb8",        5},    /* "∂̸" */
    {"npolint",                          7, "\xe2\xa8\x94",                3},    /* "⨔" */
    {"npr",                              3, "\xe2\x8a\x80",                3},    /* "⊀" */
    {"nprcue",                           6, "\xe2\x8b\xa0",                3},    /* "⋠" */
    {"npre",                             4, "\xe2\xaa\xaf\xcc\xb8",        5},    /* "⪯̸" */
    {"nprec",                            5, "\xe2\x8a\x80",                3},    /* "⊀" */
    {"npreceq",                          7, "\xe2\xaa\xaf\xcc\xb8",        5},    /* "⪯̸" */
    {"nrArr",                            5, "\xe2\x87\x8f",                3},    /* "⇏" */
    {"nrarr",                            5, "\xe2\x86\x9b",                3},    /* "↛" */
    {"nrarrc",                           6, "\xe2\xa4\xb3\xcc\xb8",        5},    /* "⤳̸" */
    {"nrarrw",                           6, "\xe2\x86\x9d\xcc\xb8",        5},    /* "↝̸" */
    {"nrightarrow",                     11, "\xe2\x86\x9b",                3},    /* "↛" */
    {"nrtri",                            5, "\xe2\x8b\xab",                3},    /* "⋫" */
    {"nrtrie",                           6, "\xe2\x8b\xad",                3},    /* "⋭" */
    {"nsc",                              3, "\xe2\x8a\x81",                3},    /* "⊁" */
    {"nsccue",                           6, "\xe2\x8b\xa1",                3},    /* "⋡" */
    {"nsce",                             4, "\xe2\xaa\xb0\xcc\xb8",        5},    /* "⪰̸" */
    {"nscr",                             4, "\xf0\x9d\x93\x83",            4},    /* "𝓃" */
    {"nshortmid",                        9, "\xe2\x88\xa4",                3},    /* "∤" */
    {"nshortparallel",                  14, "\xe2\x88\xa6",                3},    /* "∦" */
    {"nsim",                             4, "\xe2\x89\x81",                3},    /* "≁" */
    {"nsime",                            5, "\xe2\x89\x84",                3},    /* "≄" */
    {"nsimeq",                           6, "\xe2\x89\x84",                3},    /* "≄" */
    {"nsmid",                            5, "\xe2\x88\xa4",                3},    /* "∤" */
    {"nspar",                            5, "\xe2\x88\xa6",                3},    /* "∦" */
    {"nsqsube",                          7, "\xe2\x8b\xa2",                3},    /* "⋢" */
    {"nsqsupe",                          7, "\xe2\x8b\xa3",                3},    /* "⋣" */
    {"nsub",                             4, "\xe2\x8a\x84",                3},    /* "⊄" */
    {"nsubE",                            5, "\xe2\xab\x85\xcc\xb8",        5},    /* "⫅̸" */
    {"nsube",                            5, "\xe2\x8a\x88",                3},    /* "⊈" */
    {"nsubset",                          7, "\xe2\x8a\x82\xe2\x83\x92",    6},    /* "⊂⃒" */
    {"nsubseteq",                        9, "\xe2\x8a\x88",                3},    /* "⊈" */
    {"nsubseteqq",                      10, "\xe2\xab\x85\xcc\xb8",        5},    /* "⫅̸" */
    {"nsucc",                            5, "\xe2\x8a\x81",                3},    /* "⊁" */
    {"nsucceq",                          7, "\xe2\xaa\xb0\xcc\xb8",        5},    /* "⪰̸" */
    {"nsup",                             4, "\xe2\x8a\x85",                3},    /* "⊅" */
    {"nsupE",                            5, "\xe2\xab\x86\xcc\xb8",        5},    /* "⫆̸" */
    {"nsupe",                            5, "\xe2\x8a\x89",                3},    /* "⊉" */
    {"nsupset",                          7, "\xe2\x8a\x83\xe2\x83\x92",    6},    /* "⊃⃒" */
    {"nsupseteq",                        9, "\xe2\x8a\x89",                3},    /* "⊉" */
    {"nsupseteqq",                      10, "\xe2\xab\x86\xcc\xb8",        5},    /* "⫆̸" */
    {"ntgl",                             4, "\xe2\x89\xb9",                3},    /* "≹" */
    {"ntilde",                           6, "\xc3\xb1",                    2},    /* "ñ" */
    {"ntlg",                             4, "\xe2\x89\xb8",                3},    /* "≸" */
    {"ntriangleleft",                   13, "\xe2\x8b\xaa",                3},    /* "⋪" */
    {"ntrianglelefteq",                 15, "\xe2\x8b\xac",                3},    /* "⋬" */
    {"ntriangleright",                  14, "\xe2\x8b\xab",                3},    /* "⋫" */
    {"ntrianglerighteq",                16, "\xe2\x8b\xad",                3},    /* "⋭" */
    {"nu",                               2, "\xce\xbd",                    2},    /* "ν" */
    {"num",                              3, "\x23",                        1},    /* "#" */
    {"numero",                           6, "\xe2\x84\x96",                3},    /* "№" */
    {"numsp",                            5, "\xe2\x80\x87",                3},    /* " " */
    {"nvDash",                           6, "\xe2\x8a\xad",                3},    /* "⊭" */
    {"nvHarr",                           6, "\xe2\xa4\x84",                3},    /* "⤄" */
    {"nvap",                             4, "\xe2\x89\x8d\xe2\x83\x92",    6},    /* "≍⃒" */
    {"nvdash",                           6, "\xe2\x8a\xac",                3},    /* "⊬" */
    {"nvge",                             4, "\xe2\x89\xa5\xe2\x83\x92",    6},    /* "≥⃒" */
    {"nvgt",                             4, "\x3e\xe2\x83\x92",            4},    /* ">⃒" */
    {"nvinfin",                          7, "\xe2\xa7\x9e",                3},    /* "⧞" */
    {"nvlArr",                           6, "\xe2\xa4\x82",                3},    /* "⤂" */
    {"nvle",                             4, "\xe2\x89\xa4\xe2\x83\x92",    6},    /* "≤⃒" */
    {"nvlt",                             4, "\x3c\xe2\x83\x92",            4},    /* "<⃒" */
    {"nvltrie",                          7, "\xe2\x8a\xb4\xe2\x83\x92",    6},    /* "⊴⃒" */
    {"nvrArr",                           6, "\xe2\xa4\x83",                3},    /* "⤃" */
    {"nvrtrie",                          7, "\xe2\x8a\xb5\xe2\x83\x92",    6},    /* "⊵⃒" */
    {"nvsim",                            5, "\xe2\x88\xbc\xe2\x83\x92",    6},    /* "∼⃒" */
    {"nwArr",                            5, "\xe2\x87\x96",                3},    /* "⇖" */
    {"nwarhk",                           6, "\xe2\xa4\xa3",                3},    /* "⤣" */
    {"nwarr",                            5, "\xe2\x86\x96",                3},    /* "↖" */
    {"nwarrow",                          7, "\xe2\x86\x96",                3},    /* "↖" */
    {"nwnear",                           6, "\xe2\xa4\xa7",                3},    /* "⤧" */
    {"oS",                               2, "\xe2\x93\x88",                3},    /* "Ⓢ" */
    {"oacute",                           6, "\xc3\xb3",                    2},    /* "ó" */
    {"oast",                             4, "\xe2\x8a\x9b",                3},    /* "⊛" */
    {"ocir",                             4, "\xe2\x8a\x9a",                3},    /* "⊚" */
    {"ocirc",                            5, "\xc3\xb4",                    2},    /* "ô" */
    {"ocy",                              3, "\xd0\xbe",                    2},    /* "о" */
    {"odash",                            5, "\xe2\x8a\x9d",                3},    /* "⊝" */
    {"odblac",                           6, "\xc5\x91",                    2},    /* "ő" */
    {"odiv",                             4, "\xe2\xa8\xb8",                3},    /* "⨸" */
    {"odot",                             4, "\xe2\x8a\x99",                3},    /* "⊙" */
    {"odsold",                           6, "\xe2\xa6\xbc",                3},    /* "⦼" */
    {"oelig",                            5, "\xc5\x93",                    2},    /* "œ" */
    {"ofcir",                            5, "\xe2\xa6\xbf",                3},    /* "⦿" */
    {"ofr",                              3, "\xf0\x9d\x94\xac",            4},    /* "𝔬" */
    {"ogon",                             4, "\xcb\x9b",                    2},    /* "˛" */
    {"ograve",                           6, "\xc3\xb2",                    2},    /* "ò" */
    {"ogt",                              3, "\xe2\xa7\x81",                3},    /* "⧁" */
    {"ohbar",                            5, "\xe2\xa6\xb5",                3},    /* "⦵" */
    {"ohm",                              3, "\xce\xa9",                    2},    /* "Ω" */
    {"oint",                             4, "\xe2\x88\xae",                3},    /* "∮" */
    {"olarr",                            5, "\xe2\x86\xba",                3},    /* "↺" */
    {"olcir",                            5, "\xe2\xa6\xbe",                3},    /* "⦾" */
    {"olcross",                          7, "\xe2\xa6\xbb",                3},    /* "⦻" */
    {"oline",                            5, "\xe2\x80\xbe",                3},    /* "‾" */
    {"olt",                              3, "\xe2\xa7\x80",                3},    /* "⧀" */
    {"omacr",                            5, "\xc5\x8d",                    2},    /* "ō" */
    {"omega",                            5, "\xcf\x89",                    2},    /* "ω" */
    {"omicron",                          7, "\xce\xbf",                    2},    /* "ο" */
    {"omid",                             4, "\xe2\xa6\xb6",                3},    /* "⦶" */
    {"ominus",                           6, "\xe2\x8a\x96",                3},    /* "⊖" */
    {"oopf",                             4, "\xf0\x9d\x95\xa0",            4},    /* "𝕠" */
    {"opar",                             4, "\xe2\xa6\xb7",                3},    /* "⦷" */
    {"operp",                            5, "\xe2\xa6\xb9",                3},    /* "⦹" */
    {"oplus",                            5, "\xe2\x8a\x95",                3},    /* "⊕" */
    {"or",                               2, "\xe2\x88\xa8",                3},    /* "∨" */
    {"orarr",                            5, "\xe2\x86\xbb",                3},    /* "↻" */
    {"ord",                              3, "\xe2\xa9\x9d",                3},    /* "⩝" */
    {"order",                            5, "\xe2\x84\xb4",                3},    /* "ℴ" */
    {"orderof",                          7, "\xe2\x84\xb4",                3},    /* "ℴ" */
    {"ordf",                             4, "\xc2\xaa",                    2},    /* "ª" */
    {"ordm",                             4, "\xc2\xba",                    2},    /* "º" */
    {"origof",                           6, "\xe2\x8a\xb6",                3},    /* "⊶" */
    {"oror",                             4, "\xe2\xa9\x96",                3},    /* "⩖" */
    {"orslope",                          7, "\xe2\xa9\x97",                3},    /* "⩗" */
    {"orv",                              3, "\xe2\xa9\x9b",                3},    /* "⩛" */
    {"oscr",                             4, "\xe2\x84\xb4",                3},    /* "ℴ" */
    {"oslash",                           6, "\xc3\xb8",                    2},    /* "ø" */
    {"osol",                             4, "\xe2\x8a\x98",                3},    /* "⊘" */
    {"otilde",                           6, "\xc3\xb5",                    2},    /* "õ" */
    {"otimes",                           6, "\xe2\x8a\x97",                3},    /* "⊗" */
    {"otimesas",                         8, "\xe2\xa8\xb6",                3},    /* "⨶" */
    {"ouml",                             4, "\xc3\xb6",                    2},    /* "ö" */
    {"ovbar",                            5, "\xe2\x8c\xbd",                3},    /* "⌽" */
    {"par",                              3, "\xe2\x88\xa5",                3},    /* "∥" */
    {"para",                             4, "\xc2\xb6",                    2},    /* "¶" */
    {"parallel",                         8, "\xe2\x88\xa5",                3},    /* "∥" */
    {"parsim",                           6, "\xe2\xab\xb3",                3},    /* "⫳" */
    {"parsl",                            5, "\xe2\xab\xbd",                3},    /* "⫽" */
    {"part",                             4, "\xe2\x88\x82",                3},    /* "∂" */
    {"pcy",                              3, "\xd0\xbf",                    2},    /* "п" */
    {"percnt",                           6, "\x25",                        1},    /* "%" */
    {"period",                           6, "\x2e",                        1},    /* "." */
    {"permil",                           6, "\xe2\x80\xb0",                3},    /* "‰" */
    {"perp",                             4, "\xe2\x8a\xa5",                3},    /* "⊥" */
    {"pertenk",                          7, "\xe2\x80\xb1",                3},    /* "‱" */
    {"pfr",                              3, "\xf0\x9d\x94\xad",            4},    /* "𝔭" */
    {"phi",                              3, "\xcf\x86",                    2},    /* "φ" */
    {"phiv",                             4, "\xcf\x95",                    2},    /* "ϕ" */
    {"phmmat",                           6, "\xe2\x84\xb3",                3},    /* "ℳ" */
    {"phone",                            5, "\xe2\x98\x8e",                3},    /* "☎" */
    {"pi",                               2, "\xcf\x80",                    2},    /* "π" */
    {"pitchfork",                        9, "\xe2\x8b\x94",                3},    /* "⋔" */
    {"piv",                              3, "\xcf\x96",                    2},    /* "ϖ" */
    {"planck",                           6, "\xe2\x84\x8f",                3},    /* "ℏ" */
    {"planckh",                          7, "\xe2\x84\x8e",                3},    /* "ℎ" */
    {"plankv",                           6, "\xe2\x84\x8f",                3},    /* "ℏ" */
    {"plus",                             4, "\x2b",                        1},    /* "+" */
    {"plusacir",                         8, "\xe2\xa8\xa3",                3},    /* "⨣" */
    {"plusb",                            5, "\xe2\x8a\x9e",                3},    /* "⊞" */
    {"pluscir",                          7, "\xe2\xa8\xa2",                3},    /* "⨢" */
    {"plusdo",                           6, "\xe2\x88\x94",                3},    /* "∔" */
    {"plusdu",                           6, "\xe2\xa8\xa5",                3},    /* "⨥" */
    {"pluse",                            5, "\xe2\xa9\xb2",                3},    /* "⩲" */
    {"plusmn",                           6, "\xc2\xb1",                    2},    /* "±" */
    {"plussim",                          7, "\xe2\xa8\xa6",                3},    /* "⨦" */
    {"plustwo",                          7, "\xe2\xa8\xa7",                3},    /* "⨧" */
    {"pm",                               2, "\xc2\xb1",                    2},    /* "±" */
    {"pointint",                         8, "\xe2\xa8\x95",                3},    /* "⨕" */
    {"popf",                             4, "\xf0\x9d\x95\xa1",            4},    /* "𝕡" */
    {"pound",                            5, "\xc2\xa3",                    2},    /* "£" */
    {"pr",                               2, "\xe2\x89\xba",                3},    /* "≺" */
    {"prE",                              3, "\xe2\xaa\xb3",                3},    /* "⪳" */
    {"prap",                             4, "\xe2\xaa\xb7",                3},    /* "⪷" */
    {"prcue",                            5, "\xe2\x89\xbc",                3},    /* "≼" */
    {"pre",                              3, "\xe2\xaa\xaf",                3},    /* "⪯" */
    {"prec",                             4, "\xe2\x89\xba",                3},    /* "≺" */
    {"precapprox",                      10, "\xe2\xaa\xb7",                3},    /* "⪷" */
    {"preccurlyeq",                     11, "\xe2\x89\xbc",                3},    /* "≼" */
    {"preceq",                           6, "\xe2\xaa\xaf",                3},    /* "⪯" */
    {"precnapprox",                     11, "\xe2\xaa\xb9",                3},    /* "⪹" */
    {"precneqq",                         8, "\xe2\xaa\xb5",                3},    /* "⪵" */
    {"precnsim",                         8, "\xe2\x8b\xa8",                3},    /* "⋨" */
    {"precsim",                          7, "\xe2\x89\xbe",                3},    /* "≾" */
    {"prime",                            5, "\xe2\x80\xb2",                3},    /* "′" */
    {"primes",                           6, "\xe2\x84\x99",                3},    /* "ℙ" */
    {"prnE",                             4, "\xe2\xaa\xb5",                3},    /* "⪵" */
    {"prnap",                            5, "\xe2\xaa\xb9",                3},    /* "⪹" */
    {"prnsim",                           6, "\xe2\x8b\xa8",                3},    /* "⋨" */
    {"prod",                             4, "\xe2\x88\x8f",                3},    /* "∏" */
    {"profalar",                         8, "\xe2\x8c\xae",                3},    /* "⌮" */
    {"profline",                         8, "\xe2\x8c\x92",                3},    /* "⌒" */
    {"profsurf",                         8, "\xe2\x8c\x93",                3},    /* "⌓" */
    {"prop",                             4, "\xe2\x88\x9d",                3},    /* "∝" */
    {"propto",                           6, "\xe2\x88\x9d",                3},    /* "∝" */
    {"prsim",                            5, "\xe2\x89\xbe",                3},    /* "≾" */
    {"prurel",                           6, "\xe2\x8a\xb0",                3},    /* "⊰" */
    {"pscr",                             4, "\xf0\x9d\x93\x85",            4},    /* "𝓅" */
    {"psi",                              3, "\xcf\x88",                    2},    /* "ψ" */
    {"puncsp",                           6, "\xe2\x80\x88",                3},    /* " " */
    {"qfr",                              3, "\xf0\x9d\x94\xae",            4},    /* "𝔮" */
    {"qint",                             4, "\xe2\xa8\x8c",                3},    /* "⨌" */
    {"qopf",                             4, "\xf0\x9d\x95\xa2",            4},    /* "𝕢" */
    {"qprime",                           6, "\xe2\x81\x97",                3},    /* "⁗" */
    {"qscr",                             4, "\xf0\x9d\x93\x86",            4},    /* "𝓆" */
    {"quaternions",                     11, "\xe2\x84\x8d",                3},    /* "ℍ" */
    {"quatint",                          7, "\xe2\xa8\x96",                3},    /* "⨖" */
    {"quest",                            5, "\x3f",                        1},    /* "?" */
    {"questeq",                          7, "\xe2\x89\x9f",                3},    /* "≟" */
    {"quot",                             4, "\x22",                        1},    /* """ */
    {"rAarr",                            5, "\xe2\x87\x9b",                3},    /* "⇛" */
    {"rArr",                             4, "\xe2\x87\x92",                3},    /* "⇒" */
    {"rAtail",                           6, "\xe2\xa4\x9c",                3},    /* "⤜" */
    {"rBarr",                            5, "\xe2\xa4\x8f",                3},    /* "⤏" */
    {"rHar",                             4, "\xe2\xa5\xa4",                3},    /* "⥤" */
    {"race",                             4, "\xe2\x88\xbd\xcc\xb1",        5},    /* "∽̱" */
    {"racute",                           6, "\xc5\x95",                    2},    /* "ŕ" */
    {"radic",                            5, "\xe2\x88\x9a",                3},    /* "√" */
    {"raemptyv",                         8, "\xe2\xa6\xb3",                3},    /* "⦳" */
    {"rang",                             4, "\xe2\x9f\xa9",                3},    /* "⟩" */
    {"rangd",                            5, "\xe2\xa6\x92",                3},    /* "⦒" */
    {"range",                            5, "\xe2\xa6\xa5",                3},    /* "⦥" */
    {"rangle",                           6, "\xe2\x9f\xa9",                3},    /* "⟩" */
    {"raquo",                            5, "\xc2\xbb",                    2},    /* "»" */
    {"rarr",                             4, "\xe2\x86\x92",                3},    /* "→" */
    {"rarrap",                           6, "\xe2\xa5\xb5",                3},    /* "⥵" */
    {"rarrb",                            5, "\xe2\x87\xa5",                3},    /* "⇥" */
    {"rarrbfs",                          7, "\xe2\xa4\xa0",                3},    /* "⤠" */
    {"rarrc",                            5, "\xe2\xa4\xb3",                3},    /* "⤳" */
    {"rarrfs",                           6, "\xe2\xa4\x9e",                3},    /* "⤞" */
    {"rarrhk",                           6, "\xe2\x86\xaa",                3},    /* "↪" */
    {"rarrlp",                           6, "\xe2\x86\xac",                3},    /* "↬" */
    {"rarrpl",                           6, "\xe2\xa5\x85",                3},    /* "⥅" */
    {"rarrsim",                          7, "\xe2\xa5\xb4",                3},    /* "⥴" */
    {"rarrtl",                           6, "\xe2\x86\xa3",                3},    /* "↣" */
    {"rarrw",                            5, "\xe2\x86\x9d",                3},    /* "↝" */
    {"ratail",                           6, "\xe2\xa4\x9a",                3},    /* "⤚" */
    {"ratio",                            5, "\xe2\x88\xb6",                3},    /* "∶" */
    {"rationals",                        9, "\xe2\x84\x9a",                3},    /* "ℚ" */
    {"rbarr",                            5, "\xe2\xa4\x8d",                3},    /* "⤍" */
    {"rbbrk",                            5, "\xe2\x9d\xb3",                3},    /* "❳" */
    {"rbrace",                           6, "\x7d",                        1},    /* "}" */
    {"rbrack",                           6, "\x5d",                        1},    /* "]" */
    {"rbrke",                            5, "\xe2\xa6\x8c",                3},    /* "⦌" */
    {"rbrksld",                          7, "\xe2\xa6\x8e",                3},    /* "⦎" */
    {"rbrkslu",                          7, "\xe2\xa6\x90",                3},    /* "⦐" */
    {"rcaron",                           6, "\xc5\x99",                    2},    /* "ř" */
    {"rcedil",                           6, "\xc5\x97",                    2},    /* "ŗ" */
    {"rceil",                            5, "\xe2\x8c\x89",                3},    /* "⌉" */
    {"rcub",                             4, "\x7d",                        1},    /* "}" */
    {"rcy",                              3, "\xd1\x80",                    2},    /* "р" */
    {"rdca",                             4, "\xe2\xa4\xb7",                3},    /* "⤷" */
    {"rdldhar",                          7, "\xe2\xa5\xa9",                3},    /* "⥩" */
    {"rdquo",                            5, "\xe2\x80\x9d",                3},    /* "”" */
    {"rdquor",                           6, "\xe2\x80\x9d",                3},    /* "”" */
    {"rdsh",                             4, "\xe2\x86\xb3",                3},    /* "↳" */
    {"real",                             4, "\xe2\x84\x9c",                3},    /* "ℜ" */
    {"realine",                          7, "\xe2\x84\x9b",                3},    /* "ℛ" */
    {"realpart",                         8, "\xe2\x84\x9c",                3},    /* "ℜ" */
    {"reals",                            5, "\xe2\x84\x9d",                3},    /* "ℝ" */
    {"rect",                             4, "\xe2\x96\xad",                3},    /* "▭" */
    {"reg",                              3, "\xc2\xae",                    2},    /* "®" */
    {"rfisht",                           6, "\xe2\xa5\xbd",                3},    /* "⥽" */
    {"rfloor",                           6, "\xe2\x8c\x8b",                3},    /* "⌋" */
    {"rfr",                              3, "\xf0\x9d\x94\xaf",            4},    /* "𝔯" */
    {"rhard",                            5, "\xe2\x87\x81",                3},    /* "⇁" */
    {"rharu",                            5, "\xe2\x87\x80",                3},    /* "⇀" */
    {"rharul",                           6, "\xe2\xa5\xac",                3},    /* "⥬" */
    {"rho",                              3, "\xcf\x81",                    2},    /* "ρ" */
    {"rhov",                             4, "\xcf\xb1",                    2},    /* "ϱ" */
    {"rightarrow",                      10, "\xe2\x86\x92",                3},    /* "→" */
    {"rightarrowtail",                  14, "\xe2\x86\xa3",                3},    /* "↣" */
    {"rightharpoondown",                16, "\xe2\x87\x81",                3},    /* "⇁" */
    {"rightharpoonup",                  14, "\xe2\x87\x80",                3},    /* "⇀" */
    {"rightleftarrows",                 15, "\xe2\x87\x84",                3},    /* "⇄" */
    {"rightleftharpoons",               17, "\xe2\x87\x8c",                3},    /* "⇌" */
    {"rightrightarrows",                16, "\xe2\x87\x89",                3},    /* "⇉" */
    {"rightsquigarrow",                 15, "\xe2\x86\x9d",                3},    /* "↝" */
    {"rightthreetimes",                 15, "\xe2\x8b\x8c",                3},    /* "⋌" */
    {"ring",                             4, "\xcb\x9a",                    2},    /* "˚" */
    {"risingdotseq",                    12, "\xe2\x89\x93",                3},    /* "≓" */
    {"rlarr",                            5, "\xe2\x87\x84",                3},    /* "⇄" */
    {"rlhar",                            5, "\xe2\x87\x8c",                3},    /* "⇌" */
    {"rlm",                              3, "\xe2\x80\x8f",                3},    /* rlm */
    {"rmoust",                           6, "\xe2\x8e\xb1",                3},    /* "⎱" */
    {"rmoustache",                      10, "\xe2\x8e\xb1",                3},    /* "⎱" */
    {"rnmid",                            5, "\xe2\xab\xae",                3},    /* "⫮" */
    {"roang",                            5, "\xe2\x9f\xad",                3},    /* "⟭" */
    {"roarr",                            5, "\xe2\x87\xbe",                3},    /* "⇾" */
    {"robrk",                            5, "\xe2\x9f\xa7",                3},    /* "⟧" */
    {"ropar",                            5, "\xe2\xa6\x86",                3},    /* "⦆" */
    {"ropf",                             4, "\xf0\x9d\x95\xa3",            4},    /* "𝕣" */
    {"roplus",                           6, "\xe2\xa8\xae",                3},    /* "⨮" */
    {"rotimes",                          7, "\xe2\xa8\xb5",                3},    /* "⨵" */
    {"rpar",                             4, "\x29",                        1},    /* ")" */
    {"rpargt",                           6, "\xe2\xa6\x94",                3},    /* "⦔" */
    {"rppolint",                         8, "\xe2\xa8\x92",                3},    /* "⨒" */
    {"rrarr",                            5, "\xe2\x87\x89",                3},    /* "⇉" */
    {"rsaquo",                           6, "\xe2\x80\xba",                3},    /* "›" */
    {"rscr",                             4, "\xf0\x9d\x93\x87",            4},    /* "𝓇" */
    {"rsh",                              3, "\xe2\x86\xb1",                3},    /* "↱" */
    {"rsqb",                             4, "\x5d",                        1},    /* "]" */
    {"rsquo",                            5, "\xe2\x80\x99",                3},    /* "’" */
    {"rsquor",                           6, "\xe2\x80\x99",                3},    /* "’" */
    {"rthree",                           6, "\xe2\x8b\x8c",                3},    /* "⋌" */
    {"rtimes",                           6, "\xe2\x8b\x8a",                3},    /* "⋊" */
    {"rtri",                             4, "\xe2\x96\xb9",                3},    /* "▹" */
    {"rtrie",                            5, "\xe2\x8a\xb5",                3},    /* "⊵" */
    {"rtrif",                            5, "\xe2\x96\xb8",                3},    /* "▸" */
    {"rtriltri",                         8, "\xe2\xa7\x8e",                3},    /* "⧎" */
    {"ruluhar",                          7, "\xe2\xa5\xa8",                3},    /* "⥨" */
    {"rx",                               2, "\xe2\x84\x9e",                3},    /* "℞" */
    {"sacute",                           6, "\xc5\x9b",                    2},    /* "ś" */
    {"sbquo",                            5, "\xe2\x80\x9a",                3},    /* "‚" */
    {"sc",                               2, "\xe2\x89\xbb",                3},    /* "≻" */
    {"scE",                              3, "\xe2\xaa\xb4",                3},    /* "⪴" */
    {"scap",                             4, "\xe2\xaa\xb8",                3},    /* "⪸" */
    {"scaron",                           6, "\xc5\xa1",                    2},    /* "š" */
    {"sccue",                            5, "\xe2\x89\xbd",                3},    /* "≽" */
    {"sce",                              3, "\xe2\xaa\xb0",                3},    /* "⪰" */
    {"scedil",                           6, "\xc5\x9f",                    2},    /* "ş" */
    {"scirc",                            5, "\xc5\x9d",                    2},    /* "ŝ" */
    {"scnE",                             4, "\xe2\xaa\xb6",                3},    /* "⪶" */
    {"scnap",                            5, "\xe2\xaa\xba",                3},    /* "⪺" */
    {"scnsim",                           6, "\xe2\x8b\xa9",                3},    /* "⋩" */
    {"scpolint",                         8, "\xe2\xa8\x93",                3},    /* "⨓" */
    {"scsim",                            5, "\xe2\x89\xbf",                3},    /* "≿" */
    {"scy",                              3, "\xd1\x81",                    2},    /* "с" */
    {"sdot",                             4, "\xe2\x8b\x85",                3},    /* "⋅" */
    {"sdotb",                            5, "\xe2\x8a\xa1",                3},    /* "⊡" */
    {"sdote",                            5, "\xe2\xa9\xa6",                3},    /* "⩦" */
    {"seArr",                            5, "\xe2\x87\x98",                3},    /* "⇘" */
    {"searhk",                           6, "\xe2\xa4\xa5",                3},    /* "⤥" */
    {"searr",                            5, "\xe2\x86\x98",                3},    /* "↘" */
    {"searrow",                          7, "\xe2\x86\x98",                3},    /* "↘" */
    {"sect",                             4, "\xc2\xa7",                    2},    /* "§" */
    {"semi",                             4, "\x3b",                        1},    /* ";" */
    {"seswar",                           6, "\xe2\xa4\xa9",                3},    /* "⤩" */
    {"setminus",                         8, "\xe2\x88\x96",                3},    /* "∖" */
    {"setmn",                            5, "\xe2\x88\x96",                3},    /* "∖" */
    {"sext",                             4, "\xe2\x9c\xb6",                3},    /* "✶" */
    {"sfr",                              3, "\xf0\x9d\x94\xb0",            4},    /* "𝔰" */
    {"sfrown",                           6, "\xe2\x8c\xa2",                3},    /* "⌢" */
    {"sharp",                            5, "\xe2\x99\xaf",                3},    /* "♯" */
    {"shchcy",                           6, "\xd1\x89",                    2},    /* "щ" */
    {"shcy",                             4, "\xd1\x88",                    2},    /* "ш" */
    {"shortmid",                         8, "\xe2\x88\xa3",                3},    /* "∣" */
    {"shortparallel",                   13, "\xe2\x88\xa5",                3},    /* "∥" */
    {"shy",                              3, "\xc2\xad",                    2},    /* shy */
    {"sigma",                            5, "\xcf\x83",                    2},    /* "σ" */
    {"sigmaf",                           6, "\xcf\x82",                    2},    /* "ς" */
    {"sigmav",                           6, "\xcf\x82",                    2},    /* "ς" */
    {"sim",                              3, "\xe2\x88\xbc",                3},    /* "∼" */
    {"simdot",                           6, "\xe2\xa9\xaa",                3},    /* "⩪" */
    {"sime",                             4, "\xe2\x89\x83",                3},    /* "≃" */
    {"simeq",                            5, "\xe2\x89\x83",                3},    /* "≃" */
    {"simg",                             4, "\xe2\xaa\x9e",                3},    /* "⪞" */
    {"simgE",                            5, "\xe2\xaa\xa0",                3},    /* "⪠" */
    {"siml",                             4, "\xe2\xaa\x9d",                3},    /* "⪝" */
    {"simlE",                            5, "\xe2\xaa\x9f",                3},    /* "⪟" */
    {"simne",                            5, "\xe2\x89\x86",                3},    /* "≆" */
    {"simplus",                          7, "\xe2\xa8\xa4",                3},    /* "⨤" */
    {"simrarr",                          7, "\xe2\xa5\xb2",                3},    /* "⥲" */
    {"slarr",                            5, "\xe2\x86\x90",                3},    /* "←" */
    {"smallsetminus",                   13, "\xe2\x88\x96",                3},    /* "∖" */
    {"smashp",                           6, "\xe2\xa8\xb3",                3},    /* "⨳" */
    {"smeparsl",                         8, "\xe2\xa7\xa4",                3},    /* "⧤" */
    {"smid",                             4, "\xe2\x88\xa3",                3},    /* "∣" */
    {"smile",                            5, "\xe2\x8c\xa3",                3},    /* "⌣" */
    {"smt",                              3, "\xe2\xaa\xaa",                3},    /* "⪪" */
    {"smte",                             4, "\xe2\xaa\xac",                3},    /* "⪬" */
    {"smtes",                            5, "\xe2\xaa\xac\xef\xb8\x80",    6},    /* "⪬︀" */
    {"softcy",                           6, "\xd1\x8c",                    2},    /* "ь" */
    {"sol",                              3, "\x2f",                        1},    /* "/" */
    {"solb",                             4, "\xe2\xa7\x84",                3},    /* "⧄" */
    {"solbar",                           6, "\xe2\x8c\xbf",                3},    /* "⌿" */
    {"sopf",                             4, "\xf0\x9d\x95\xa4",            4},    /* "𝕤" */
    {"spades",                           6, "\xe2\x99\xa0",                3},    /* "♠" */
    {"spadesuit",                        9, "\xe2\x99\xa0",                3},    /* "♠" */
    {"spar",                             4, "\xe2\x88\xa5",                3},    /* "∥" */
    {"sqcap",                            5, "\xe2\x8a\x93",                3},    /* "⊓" */
    {"sqcaps",                           6, "\xe2\x8a\x93\xef\xb8\x80",    6},    /* "⊓︀" */
    {"sqcup",                            5, "\xe2\x8a\x94",                3},    /* "⊔" */
    {"sqcups",                           6, "\xe2\x8a\x94\xef\xb8\x80",    6},    /* "⊔︀" */
    {"sqsub",                            5, "\xe2\x8a\x8f",                3},    /* "⊏" */
    {"sqsube",                           6, "\xe2\x8a\x91",                3},    /* "⊑" */
    {"sqsubset",                         8, "\xe2\x8a\x8f",                3},    /* "⊏" */
    {"sqsubseteq",                      10, "\xe2\x8a\x91",                3},    /* "⊑" */
    {"sqsup",                            5, "\xe2\x8a\x90",                3},    /* "⊐" */
    {"sqsupe",                           6, "\xe2\x8a\x92",                3},    /* "⊒" */
    {"sqsupset",                         8, "\xe2\x8a\x90",                3},    /* "⊐" */
    {"sqsupseteq",                      10, "\xe2\x8a\x92",                3},    /* "⊒" */
    {"squ",                              3, "\xe2\x96\xa1",                3},    /* "□" */
    {"square",                           6, "\xe2\x96\xa1",                3},    /* "□" */
    {"squarf",                           6, "\xe2\x96\xaa",                3},    /* "▪" */
    {"squf",                             4, "\xe2\x96\xaa",                3},    /* "▪" */
    {"srarr",                            5, "\xe2\x86\x92",                3},    /* "→" */
    {"sscr",                             4, "\xf0\x9d\x93\x88",            4},    /* "𝓈" */
    {"ssetmn",                           6, "\xe2\x88\x96",                3},    /* "∖" */
    {"ssmile",                           6, "\xe2\x8c\xa3",                3},    /* "⌣" */
    {"sstarf",                           6, "\xe2\x8b\x86",                3},    /* "⋆" */
    {"star",                             4, "\xe2\x98\x86",                3},    /* "☆" */
    {"starf",                            5, "\xe2\x98\x85",                3},    /* "★" */
    {"straightepsilon",                 15, "\xcf\xb5",                    2},    /* "ϵ" */
    {"straightphi",                     11, "\xcf\x95",                    2},    /* "ϕ" */
    {"strns",                            5, "\xc2\xaf",                    2},    /* "¯" */
    {"sub",                              3, "\xe2\x8a\x82",                3},    /* "⊂" */
    {"subE",                             4, "\xe2\xab\x85",                3},    /* "⫅" */
    {"subdot",                           6, "\xe2\xaa\xbd",                3},    /* "⪽" */
    {"sube",                             4, "\xe2\x8a\x86",                3},    /* "⊆" */
    {"subedot",                          7, "\xe2\xab\x83",                3},    /* "⫃" */
    {"submult",                          7, "\xe2\xab\x81",                3},    /* "⫁" */
    {"subnE",                            5, "\xe2\xab\x8b",                3},    /* "⫋" */
    {"subne",                            5, "\xe2\x8a\x8a",                3},    /* "⊊" */
    {"subplus",                          7, "\xe2\xaa\xbf",                3},    /* "⪿" */
    {"subrarr",                          7, "\xe2\xa5\xb9",                3},    /* "⥹" */
    {"subset",                           6, "\xe2\x8a\x82",                3},    /* "⊂" */
    {"subseteq",                         8, "\xe2\x8a\x86",                3},    /* "⊆" */
    {"subseteqq",                        9, "\xe2\xab\x85",                3},    /* "⫅" */
    {"subsetneq",                        9, "\xe2\x8a\x8a",                3},    /* "⊊" */
    {"subsetneqq",                      10, "\xe2\xab\x8b",                3},    /* "⫋" */
    {"subsim",                           6, "\xe2\xab\x87",                3},    /* "⫇" */
    {"subsub",                           6, "\xe2\xab\x95",                3},    /* "⫕" */
    {"subsup",                           6, "\xe2\xab\x93",                3},    /* "⫓" */
    {"succ",                             4, "\xe2\x89\xbb",                3},    /* "≻" */
    {"succapprox",                      10, "\xe2\xaa\xb8",                3},    /* "⪸" */
    {"succcurlyeq",                     11, "\xe2\x89\xbd",                3},    /* "≽" */
    {"succeq",                           6, "\xe2\xaa\xb0",                3},    /* "⪰" */
    {"succnapprox",                     11, "\xe2\xaa\xba",                3},    /* "⪺" */
    {"succneqq",                         8, "\xe2\xaa\xb6",                3},    /* "⪶" */
    {"succnsim",                         8, "\xe2\x8b\xa9",                3},    /* "⋩" */
    {"succsim",                          7, "\xe2\x89\xbf",                3},    /* "≿" */
    {"sum",                              3, "\xe2\x88\x91",                3},    /* "∑" */
    {"sung",                             4, "\xe2\x99\xaa",                3},    /* "♪" */
    {"sup",                              3, "\xe2\x8a\x83",                3},    /* "⊃" */
    {"sup1",                             4, "\xc2\xb9",                    2},    /* "¹" */
    {"sup2",                             4, "\xc2\xb2",                    2},    /* "²" */
    {"sup3",                             4, "\xc2\xb3",                    2},    /* "³" */
    {"supE",                             4, "\xe2\xab\x86",                3},    /* "⫆" */
    {"supdot",                           6, "\xe2\xaa\xbe",                3},    /* "⪾" */
    {"supdsub",                          7, "\xe2\xab\x98",                3},    /* "⫘" */
    {"supe",                             4, "\xe2\x8a\x87",                3},    /* "⊇" */
    {"supedot",                          7, "\xe2\xab\x84",                3},    /* "⫄" */
    {"suphsol",                          7, "\xe2\x9f\x89",                3},    /* "⟉" */
    {"suphsub",                          7, "\xe2\xab\x97",                3},    /* "⫗" */
    {"suplarr",                          7, "\xe2\xa5\xbb",                3},    /* "⥻" */
    {"supmult",                          7, "\xe2\xab\x82",                3},    /* "⫂" */
    {"supnE",                            5, "\xe2\xab\x8c",                3},    /* "⫌" */
    {"supne",                            5, "\xe2\x8a\x8b",                3},    /* "⊋" */
    {"supplus",                          7, "\xe2\xab\x80",                3},    /* "⫀" */
    {"supset",                           6, "\xe2\x8a\x83",                3},    /* "⊃" */
    {"supseteq",                         8, "\xe2\x8a\x87",                3},    /* "⊇" */
    {"supseteqq",                        9, "\xe2\xab\x86",                3},    /* "⫆" */
    {"supsetneq",                        9, "\xe2\x8a\x8b",                3},    /* "⊋" */
    {"supsetneqq",                      10, "\xe2\xab\x8c",                3},    /* "⫌" */
    {"supsim",                           6, "\xe2\xab\x88",                3},    /* "⫈" */
    {"supsub",                           6, "\xe2\xab\x94",                3},    /* "⫔" */
    {"supsup",                           6, "\xe2\xab\x96",                3},    /* "⫖" */
    {"swArr",                            5, "\xe2\x87\x99",                3},    /* "⇙" */
    {"swarhk",                           6, "\xe2\xa4\xa6",                3},    /* "⤦" */
    {"swarr",                            5, "\xe2\x86\x99",                3},    /* "↙" */
    {"swarrow",                          7, "\xe2\x86\x99",                3},    /* "↙" */
    {"swnwar",                           6, "\xe2\xa4\xaa",                3},    /* "⤪" */
    {"szlig",                            5, "\xc3\x9f",                    2},    /* "ß" */
    {"target",                           6, "\xe2\x8c\x96",                3},    /* "⌖" */
    {"tau",                              3, "\xcf\x84",                    2},    /* "τ" */
    {"tbrk",                             4, "\xe2\x8e\xb4",                3},    /* "⎴" */
    {"tcaron",                           6, "\xc5\xa5",                    2},    /* "ť" */
    {"tcedil",                           6, "\xc5\xa3",                    2},    /* "ţ" */
    {"tcy",                              3, "\xd1\x82",                    2},    /* "т" */
    {"tdot",                             4, "\xe2\x83\x9b",                3},    /* "⃛" */
    {"telrec",                           6, "\xe2\x8c\x95",                3},    /* "⌕" */
    {"tfr",                              3, "\xf0\x9d\x94\xb1",            4},    /* "𝔱" */
    {"there4",                           6, "\xe2\x88\xb4",                3},    /* "∴" */
    {"therefore",                        9, "\xe2\x88\xb4",                3},    /* "∴" */
    {"theta",                            5, "\xce\xb8",                    2},    /* "θ" */
    {"thetasym",                         8, "\xcf\x91",                    2},    /* "ϑ" */
    {"thetav",                           6, "\xcf\x91",                    2},    /* "ϑ" */
    {"thickapprox",                     11, "\xe2\x89\x88",                3},    /* "≈" */
    {"thicksim",                         8, "\xe2\x88\xbc",                3},    /* "∼" */
    {"thinsp",                           6, "\xe2\x80\x89",                3},    /* " " */
    {"thkap",                            5, "\xe2\x89\x88",                3},    /* "≈" */
    {"thksim",                           6, "\xe2\x88\xbc",                3},    /* "∼" */
    {"thorn",                            5, "\xc3\xbe",                    2},    /* "þ" */
    {"tilde",                            5, "\xcb\x9c",                    2},    /* "˜" */
    {"times",                            5, "\xc3\x97",                    2},    /* "×" */
    {"timesb",                           6, "\xe2\x8a\xa0",                3},    /* "⊠" */
    {"timesbar",                         8, "\xe2\xa8\xb1",                3},    /* "⨱" */
    {"timesd",                           6, "\xe2\xa8\xb0",                3},    /* "⨰" */
    {"tint",                             4, "\xe2\x88\xad",                3},    /* "∭" */
    {"toea",                             4, "\xe2\xa4\xa8",                3},    /* "⤨" */
    {"top",                              3, "\xe2\x8a\xa4",                3},    /* "⊤" */
    {"topbot",                           6, "\xe2\x8c\xb6",                3},    /* "⌶" */
    {"topcir",                           6, "\xe2\xab\xb1",                3},    /* "⫱" */
    {"topf",                             4, "\xf0\x9d\x95\xa5",            4},    /* "𝕥" */
    {"topfork",                          7, "\xe2\xab\x9a",                3},    /* "⫚" */
    {"tosa",                             4, "\xe2\xa4\xa9",                3},    /* "⤩" */
    {"tprime",                           6, "\xe2\x80\xb4",                3},    /* "‴" */
    {"trade",                            5, "\xe2\x84\xa2",                3},    /* "™" */
    {"triangle",                         8, "\xe2\x96\xb5",                3},    /* "▵" */
    {"triangledown",                    12, "\xe2\x96\xbf",                3},    /* "▿" */
    {"triangleleft",                    12, "\xe2\x97\x83",                3},    /* "◃" */
    {"trianglelefteq",                  14, "\xe2\x8a\xb4",                3},    /* "⊴" */
    {"triangleq",                        9, "\xe2\x89\x9c",                3},    /* "≜" */
    {"triangleright",                   13, "\xe2\x96\xb9",                3},    /* "▹" */
    {"trianglerighteq",                 15, "\xe2\x8a\xb5",                3},    /* "⊵" */
    {"tridot",                           6, "\xe2\x97\xac",                3},    /* "◬" */
    {"trie",                             4, "\xe2\x89\x9c",                3},    /* "≜" */
    {"triminus",                         8, "\xe2\xa8\xba",                3},    /* "⨺" */
    {"triplus",                          7, "\xe2\xa8\xb9",                3},    /* "⨹" */
    {"trisb",                            5, "\xe2\xa7\x8d",                3},    /* "⧍" */
    {"tritime",                          7, "\xe2\xa8\xbb",                3},    /* "⨻" */
    {"trpezium",                         8, "\xe2\x8f\xa2",                3},    /* "⏢" */
    {"tscr",                             4, "\xf0\x9d\x93\x89",            4},    /* "𝓉" */
    {"tscy",                             4, "\xd1\x86",                    2},    /* "ц" */
    {"tshcy",                            5, "\xd1\x9b",                    2},    /* "ћ" */
    {"tstrok",                           6, "\xc5\xa7",                    2},    /* "ŧ" */
    {"twixt",                            5, "\xe2\x89\xac",                3},    /* "≬" */
    {"twoheadleftarrow",                16, "\xe2\x86\x9e",                3},    /* "↞" */
    {"twoheadrightarrow",               17, "\xe2\x86\xa0",                3},    /* "↠" */
    {"uArr",                             4, "\xe2\x87\x91",                3},    /* "⇑" */
    {"uHar",                             4, "\xe2\xa5\xa3",                3},    /* "⥣" */
    {"uacute",                           6, "\xc3\xba",                    2},    /* "ú" */
    {"uarr",                             4, "\xe2\x86\x91",                3},    /* "↑" */
    {"ubrcy",                            5, "\xd1\x9e",                    2},    /* "ў" */
    {"ubreve",                           6, "\xc5\xad",                    2},    /* "ŭ" */
    {"ucirc",                            5, "\xc3\xbb",                    2},    /* "û" */
    {"ucy",                              3, "\xd1\x83",                    2},    /* "у" */
    {"udarr",                            5, "\xe2\x87\x85",                3},    /* "⇅" */
    {"udblac",                           6, "\xc5\xb1",                    2},    /* "ű" */
    {"udhar",                            5, "\xe2\xa5\xae",                3},    /* "⥮" */
    {"ufisht",                           6, "\xe2\xa5\xbe",                3},    /* "⥾" */
    {"ufr",                              3, "\xf0\x9d\x94\xb2",            4},    /* "𝔲" */
    {"ugrave",                           6, "\xc3\xb9",                    2},    /* "ù" */
    {"uharl",                            5, "\xe2\x86\xbf",                3},    /* "↿" */
    {"uharr",                            5, "\xe2\x86\xbe",                3},    /* "↾" */
    {"uhblk",                            5, "\xe2\x96\x80",                3},    /* "▀" */
    {"ulcorn",                           6, "\xe2\x8c\x9c",                3},    /* "⌜" */
    {"ulcorner",                         8, "\xe2\x8c\x9c",                3},    /* "⌜" */
    {"ulcrop",                           6, "\xe2\x8c\x8f",                3},    /* "⌏" */
    {"ultri",                            5, "\xe2\x97\xb8",                3},    /* "◸" */
    {"umacr",                            5, "\xc5\xab",                    2},    /* "ū" */
    {"uml",                              3, "\xc2\xa8",                    2},    /* "¨" */
    {"uogon",                            5, "\xc5\xb3",                    2},    /* "ų" */
    {"uopf",                             4, "\xf0\x9d\x95\xa6",            4},    /* "𝕦" */
    {"uparrow",                          7, "\xe2\x86\x91",                3},    /* "↑" */
    {"updownarrow",                     11, "\xe2\x86\x95",                3},    /* "↕" */
    {"upharpoonleft",                   13, "\xe2\x86\xbf",                3},    /* "↿" */
    {"upharpoonright",                  14, "\xe2\x86\xbe",                3},    /* "↾" */
    {"uplus",                            5, "\xe2\x8a\x8e",                3},    /* "⊎" */
    {"upsi",                             4, "\xcf\x85",                    2},    /* "υ" */
    {"upsih",                            5, "\xcf\x92",                    2},    /* "ϒ" */
    {"upsilon",                          7, "\xcf\x85",                    2},    /* "υ" */
    {"upuparrows",                      10, "\xe2\x87\x88",                3},    /* "⇈" */
    {"urcorn",                           6, "\xe2\x8c\x9d",                3},    /* "⌝" */
    {"urcorner",                         8, "\xe2\x8c\x9d",                3},    /* "⌝" */
    {"urcrop",                           6, "\xe2\x8c\x8e",                3},    /* "⌎" */
    {"uring",                            5, "\xc5\xaf",                    2},    /* "ů" */
    {"urtri",                            5, "\xe2\x97\xb9",                3},    /* "◹" */
    {"uscr",                             4, "\xf0\x9d\x93\x8a",            4},    /* "𝓊" */
    {"utdot",                            5, "\xe2\x8b\xb0",                3},    /* "⋰" */
    {"utilde",                           6, "\xc5\xa9",                    2},    /* "ũ" */
    {"utri",                             4, "\xe2\x96\xb5",                3},    /* "▵" */
    {"utrif",                            5, "\xe2\x96\xb4",                3},    /* "▴" */
    {"uuarr",                            5, "\xe2\x87\x88",                3},    /* "⇈" */
    {"uuml",                             4, "\xc3\xbc",                    2},    /* "ü" */
    {"uwangle",                          7, "\xe2\xa6\xa7",                3},    /* "⦧" */
    {"vArr",                             4, "\xe2\x87\x95",                3},    /* "⇕" */
    {"vBar",                             4, "\xe2\xab\xa8",                3},    /* "⫨" */
    {"vBarv",                            5, "\xe2\xab\xa9",                3},    /* "⫩" */
    {"vDash",                            5, "\xe2\x8a\xa8",                3},    /* "⊨" */
    {"vangrt",                           6, "\xe2\xa6\x9c",                3},    /* "⦜" */
    {"varepsilon",                      10, "\xcf\xb5",                    2},    /* "ϵ" */
    {"varkappa",                         8, "\xcf\xb0",                    2},    /* "ϰ" */
    {"varnothing",                      10, "\xe2\x88\x85",                3},    /* "∅" */
    {"varphi",                           6, "\xcf\x95",                    2},    /* "ϕ" */
    {"varpi",                            5, "\xcf\x96",                    2},    /* "ϖ" */
    {"varpropto",                        9, "\xe2\x88\x9d",                3},    /* "∝" */
    {"varr",                             4, "\xe2\x86\x95",                3},    /* "↕" */
    {"varrho",                           6, "\xcf\xb1",                    2},    /* "ϱ" */
    {"varsigma",                         8, "\xcf\x82",                    2},    /* "ς" */
    {"varsubsetneq",                    12, "\xe2\x8a\x8a\xef\xb8\x80",    6},    /* "⊊︀" */
    {"varsubsetneqq",                   13, "\xe2\xab\x8b\xef\xb8\x80",    6},    /* "⫋︀" */
    {"varsupsetneq",                    12, "\xe2\x8a\x8b\xef\xb8\x80",    6},    /* "⊋︀" */
    {"varsupsetneqq",                   13, "\xe2\xab\x8c\xef\xb8\x80",    6},    /* "⫌︀" */
    {"vartheta",                         8, "\xcf\x91",                    2},    /* "ϑ" */
    {"vartriangleleft",                 15, "\xe2\x8a\xb2",                3},    /* "⊲" */
    {"vartriangleright",                16, "\xe2\x8a\xb3",                3},    /* "⊳" */
    {"vcy",                              3, "\xd0\xb2",                    2},    /* "в" */
    {"vdash",                            5, "\xe2\x8a\xa2",                3},    /* "⊢" */
    {"vee",                              3, "\xe2\x88\xa8",                3},    /* "∨" */
    {"veebar",                           6, "\xe2\x8a\xbb",                3},    /* "⊻" */
    {"veeeq",                            5, "\xe2\x89\x9a",                3},    /* "≚" */
    {"vellip",                           6, "\xe2\x8b\xae",                3},    /* "⋮" */
    {"verbar",                           6, "\x7c",                        1},    /* "|" */
    {"vert",                             4, "\x7c",                        1},    /* "|" */
    {"vfr",                              3, "\xf0\x9d\x94\xb3",            4},    /* "𝔳" */
    {"vltri",                            5, "\xe2\x8a\xb2",                3},    /* "⊲" */
    {"vnsub",                            5, "\xe2\x8a\x82\xe2\x83\x92",    6},    /* "⊂⃒" */
    {"vnsup",                            5, "\xe2\x8a\x83\xe2\x83\x92",    6},    /* "⊃⃒" */
    {"vopf",                             4, "\xf0\x9d\x95\xa7",            4},    /* "𝕧" */
    {"vprop",                            5, "\xe2\x88\x9d",                3},    /* "∝" */
    {"vrtri",                            5, "\xe2\x8a\xb3",                3},    /* "⊳" */
    {"vscr",                             4, "\xf0\x9d\x93\x8b",            4},    /* "𝓋" */
    {"vsubnE",                           6, "\xe2\xab\x8b\xef\xb8\x80",    6},    /* "⫋︀" */
    {"vsubne",                           6, "\xe2\x8a\x8a\xef\xb8\x80",    6},    /* "⊊︀" */
    {"vsupnE",                           6, "\xe2\xab\x8c\xef\xb8\x80",    6},    /* "⫌︀" */
    {"vsupne",                           6, "\xe2\x8a\x8b\xef\xb8\x80",    6},    /* "⊋︀" */
    {"vzigzag",                          7, "\xe2\xa6\x9a",                3},    /* "⦚" */
    {"wcirc",                            5, "\xc5\xb5",                    2},    /* "ŵ" */
    {"wedbar",                           6, "\xe2\xa9\x9f",                3},    /* "⩟" */
    {"wedge",                            5, "\xe2\x88\xa7",                3},    /* "∧" */
    {"wedgeq",                           6, "\xe2\x89\x99",                3},    /* "≙" */
    {"weierp",                           6, "\xe2\x84\x98",                3},    /* "℘" */
    {"wfr",                              3, "\xf0\x9d\x94\xb4",            4},    /* "𝔴" */
    {"wopf",                             4, "\xf0\x9d\x95\xa8",            4},    /* "𝕨" */
    {"wp",                               2, "\xe2\x84\x98",                3},    /* "℘" */
    {"wr",                               2, "\xe2\x89\x80",                3},    /* "≀" */
    {"wreath",                           6, "\xe2\x89\x80",                3},    /* "≀" */
    {"wscr",                             4, "\xf0\x9d\x93\x8c",            4},    /* "𝓌" */
    {"xcap",                             4, "\xe2\x8b\x82",                3},    /* "⋂" */
    {"xcirc",                            5, "\xe2\x97\xaf",                3},    /* "◯" */
    {"xcup",                             4, "\xe2\x8b\x83",                3},    /* "⋃" */
    {"xdtri",                            5, "\xe2\x96\xbd",                3},    /* "▽" */
    {"xfr",                              3, "\xf0\x9d\x94\xb5",            4},    /* "𝔵" */
    {"xhArr",                            5, "\xe2\x9f\xba",                3},    /* "⟺" */
    {"xharr",                            5, "\xe2\x9f\xb7",                3},    /* "⟷" */
    {"xi",                               2, "\xce\xbe",                    2},    /* "ξ" */
    {"xlArr",                            5, "\xe2\x9f\xb8",                3},    /* "⟸" */
    {"xlarr",                            5, "\xe2\x9f\xb5",                3},    /* "⟵" */
    {"xmap",                             4, "\xe2\x9f\xbc",                3},    /* "⟼" */
    {"xnis",                             4, "\xe2\x8b\xbb",                3},    /* "⋻" */
    {"xodot",                            5, "\xe2\xa8\x80",                3},    /* "⨀" */
    {"xopf",                             4, "\xf0\x9d\x95\xa9",            4},    /* "𝕩" */
    {"xoplus",                           6, "\xe2\xa8\x81",                3},    /* "⨁" */
    {"xotime",                           6, "\xe2\xa8\x82",                3},    /* "⨂" */
    {"xrArr",                            5, "\xe2\x9f\xb9",                3},    /* "⟹" */
    {"xrarr",                            5, "\xe2\x9f\xb6",                3},    /* "⟶" */
    {"xscr",                             4, "\xf0\x9d\x93\x8d",            4},    /* "𝓍" */
    {"xsqcup",                           6, "\xe2\xa8\x86",                3},    /* "⨆" */
    {"xuplus",                           6, "\xe2\xa8\x84",                3},    /* "⨄" */
    {"xutri",                            5, "\xe2\x96\xb3",                3},    /* "△" */
    {"xvee",                             4, "\xe2\x8b\x81",                3},    /* "⋁" */
    {"xwedge",                           6, "\xe2\x8b\x80",                3},    /* "⋀" */
    {"yacute",                           6, "\xc3\xbd",                    2},    /* "ý" */
    {"yacy",                             4, "\xd1\x8f",                    2},    /* "я" */
    {"ycirc",                            5, "\xc5\xb7",                    2},    /* "ŷ" */
    {"ycy",                              3, "\xd1\x8b",                    2},    /* "ы" */
    {"yen",                              3, "\xc2\xa5",                    2},    /* "¥" */
    {"yfr",                              3, "\xf0\x9d\x94\xb6",            4},    /* "𝔶" */
    {"yicy",                             4, "\xd1\x97",                    2},    /* "ї" */
    {"yopf",                             4, "\xf0\x9d\x95\xaa",            4},    /* "𝕪" */
    {"yscr",                             4, "\xf0\x9d\x93\x8e",            4},    /* "𝓎" */
    {"yucy",                             4, "\xd1\x8e",                    2},    /* "ю" */
    {"yuml",                             4, "\xc3\xbf",                    2},    /* "ÿ" */
    {"zacute",                           6, "\xc5\xba",                    2},    /* "ź" */
    {"zcaron",                           6, "\xc5\xbe",                    2},    /* "ž" */
    {"zcy",                              3, "\xd0\xb7",                    2},    /* "з" */
    {"zdot",                             4, "\xc5\xbc",                    2},    /* "ż" */
    {"zeetrf",                           6, "\xe2\x84\xa8",                3},    /* "ℨ" */
    {"zeta",                             4, "\xce\xb6",                    2},    /* "ζ" */
    {"zfr",                              3, "\xf0\x9d\x94\xb7",            4},    /* "𝔷" */
    {"zhcy",                             4, "\xd0\xb6",                    2},    /* "ж" */
    {"zigrarr",                          7, "\xe2\x87\x9d",                3},    /* "⇝" */
    {"zopf",                             4, "\xf0\x9d\x95\xab",            4},    /* "𝕫" */
    {"zscr",                             4, "\xf0\x9d\x93\x8f",            4},    /* "𝓏" */
    {"zwj",                              3, "\xe2\x80\x8d",                3},    /* zwj */
    {"zwnj",                             4, "\xe2\x80\x8c",                3},    /* zwnj */
    {NULL,                               0, "",                            0}
};


static size_t
EntityDecode(const char *entity, size_t length, bool *needEncodePtr, char *outPtr)
{
    size_t decoded = 0u;

    NS_NONNULL_ASSERT(entity != NULL);
    NS_NONNULL_ASSERT(outPtr != NULL);
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
NsTclStripHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
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
                                           inString, (TCL_SIZE_T)strlen(inString), &ds);
            Tcl_DStringResult(interp, &ds);
        } else {
            Tcl_SetObjResult(interp, Tcl_NewStringObj(inString, TCL_INDEX_NONE));
        }
        ns_free(inString);
    }
    return result;
}



/*
 *----------------------------------------------------------------------
 *
 * HtmlParseTagAtts --
 *
 *      Helper function of NsTclParseHtmlObjCmd() to parse contents of a tag
 *      (name and attributes).
 *
 * Results:
 *      List containing name and parsed attributes in form of a dict Tcl_Obj.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static Tcl_Obj *
HtmlParseTagAtts(const char *string, ptrdiff_t length)
{
    ptrdiff_t   i = 0;
    Tcl_Obj    *resultObj, *nameObj;

    Ns_Log(Debug, "HtmlParseTagAtts string '%s' length %ld", string, length);


    /*
     * Accept every non-space character as tagname - the first character is
     * checked already.
     */
    if (i<length && CHARTYPE(space, string[i]) == 0) {
        i++;
    }
    /*
     * Accept every non-space character after first char, a few are disallowed)
     */
    while (i<length
           && CHARTYPE(space, string[i]) == 0
           && string[i] != '/'
           ) {
        if (string[i] == '\''
            || string[i] == '"'
            || string[i] == '&') {
            return NULL;
        }
        i++;
    }
    resultObj = Tcl_NewListObj(0, NULL);
    nameObj = Tcl_NewStringObj(string, (TCL_SIZE_T)i);
    Tcl_ListObjAppendElement(NULL, resultObj, nameObj);
    Ns_Log(Debug, "... tagname '%s'", Tcl_GetString(nameObj));

    while (i<length && CHARTYPE(space, string[i]) != 0) {
        Ns_Log(Debug, "... after tagname skip space '%c'", string[i]);
        i++;
    }

    /*
     * When the tag name starts with a slash, it is the endtag without
     * attributes.x
     */
    if (string[0] != '/') {
        Tcl_Obj *attributesObj = Tcl_NewDictObj(), *valueObj;
        bool     incorrectSyntax = NS_FALSE;

        while (i<length) {
            /*
             * We have attributes
             */
            ptrdiff_t attributeStart = i, attributeNameEnd;

            Ns_Log(Debug, "to parse attribute name '%s' i %ld length %ld", &string[i], i, length);

            if (CHARTYPE(space, string[i]) != 0) {
                Ns_Log(Warning, "HtmlParseTagAtts: attribute name MUST NOT START WITH SPACE '%s'",
                       &string[i]);
            }

            while (i<length
                   && CHARTYPE(space, string[i]) == 0
                   && string[i] != '"'
                   && string[i] != '\''
                   && string[i] != '='
                   && string[i] != '/'
               ) {
                i++;
            }
            attributeNameEnd = i;

            while (i<length && CHARTYPE(space, string[i]) != 0) {
                //Ns_Log(Debug, "... after att skip space %ld %c", i, string[i]);
                i++;
            }
            /*
             * After the attribute name, we expect an "=" or whitespace/end
             * for empty values.
             */
            if (string[i] == '=') {
                ptrdiff_t valueStart, valueEnd;
                char delimiter = '\0';

                i++;
                while (i<length && CHARTYPE(space, string[i]) != 0) {
                    //Ns_Log(Debug, "... after equals skip space %ld %c", i, string[i]);
                    i++;
                }
                if (string[i] == '\'' || string[i] == '"') {
                    delimiter = string[i];
                    i++;
                }
                Ns_Log(Debug, "... got equals at pos %ld delimiter %c", i, delimiter);

                valueStart = i;
                valueEnd = valueStart;
                if (i<length) {
                    Ns_Log(Debug, "to parse attribute value '%s' i %ld length %ld delimiter %c",
                           &string[i], i, length, delimiter);
                    if (delimiter == '\0') {
                        /*
                         * No delimiter, collect non-space chars as value.
                         */
                        while (i<length && CHARTYPE(space, string[i]) == 0) {
                            i++;
                        }
                        valueEnd = i;
                    } else {
                        while (i<length && string[i] != delimiter) {
                            i++;
                        }
                        if (string[i] != delimiter) {
                            Ns_Log(Warning, "HtmlParseTagAtts: missing closing delimiter (%c) in (%s)",
                                   delimiter, string);
                            incorrectSyntax = NS_TRUE;
                        }
                        valueEnd = i;
                    }
                    i++;
                } else {
                    /*
                     * Equal sign is at the end, value start is value end,
                     * assume an empty value.
                     */
                }
                if (!incorrectSyntax) {
                    nameObj = Tcl_NewStringObj(&string[attributeStart],
                                               (TCL_SIZE_T)(attributeNameEnd - attributeStart));
                    valueObj = Tcl_NewStringObj(&string[valueStart],
                                                (TCL_SIZE_T)(valueEnd - valueStart));
                    Tcl_DictObjPut(NULL, attributesObj, nameObj, valueObj);
                    Ns_Log(Debug, "... att '%s' got value '%s'",
                           Tcl_GetString(nameObj), Tcl_GetString(valueObj));
                }
            } else if (string[i] != '/') {
                if (!incorrectSyntax) {
                    /*
                     * No equals after attribute name: The value is implicitly the empty string.
                     * https://www.w3.org/TR/2011/WD-html5-20110525/syntax.html#syntax-tag-name
                     */
                    nameObj = Tcl_NewStringObj(&string[attributeStart],
                                               (TCL_SIZE_T)(attributeNameEnd - attributeStart));

                    valueObj = Tcl_NewStringObj("", 0);
                    Tcl_DictObjPut(NULL, attributesObj, nameObj, valueObj);
                    Ns_Log(Debug, "... no equals %c i %ld length %ld att '%s' value '%s'", string[i], i, length,
                           Tcl_GetString(nameObj), Tcl_GetString(valueObj));
                }
                /*
                 * Since we have skipped space already, we might be at the
                 * first character of the next attribute already. In case this
                 * attribute was the last, we point to the closing ">",
                 * decrementing in fine as well.
                 */
            } else {
                /*
                 * The next character is '/' (terminating slash, as used for
                 * empty tag notation such as "<br/>". Skip it.
                 */
                i++;
            }

            /*
             * We are after the attribute value, skip potential white space.
             */
            while (i<length && CHARTYPE(space, string[i]) != 0) {
                // Ns_Log(Debug, "... end of loop skip space pos %ld '%c'", i, string[i]);
                i++;
            }
            if (i == attributeStart) {
                /*
                 * Safety belt: we are still at the begin of the attribute,
                 * nothing was consumed. To avoid infinite loops, advance here and complain.
                 */
                Ns_Log(Warning, "HtmlParseTagAtts: safety belt, nothing consumed, we are pos %ld '%c' in string '%s'",
                       i, string[i], string);
                i++;
            }
        }

        if (incorrectSyntax) {
            Tcl_DecrRefCount(resultObj);
            resultObj = NULL;
        } else {
            Tcl_ListObjAppendElement(NULL, resultObj, attributesObj);
        }
    }

    return resultObj;
}

/*
 *----------------------------------------------------------------------
 *
 * HtmlFinishElement --
 *
 *       Helper function of NsTclParseHtmlObjCmd() to return a list element of
 *       the result list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
static void
HtmlFinishElement(Tcl_Obj *listObj, const char *what, const char *lastStart,
                  const char *currentPtr, bool noAngle, bool onlyTags, Tcl_Obj *contentObj)
{
    if (onlyTags) {
        if (contentObj != NULL) {
            Tcl_ListObjAppendElement(NULL, listObj, contentObj);
        }
    } else {
        ptrdiff_t length = currentPtr - lastStart;
        Tcl_Obj  *elementObj = Tcl_NewListObj(0, NULL);

        Tcl_ListObjAppendElement(NULL, elementObj, Tcl_NewStringObj(what, TCL_INDEX_NONE));
        if (noAngle) {
            lastStart --;
            length += 2;
        }
        Tcl_ListObjAppendElement(NULL, elementObj,
                                 Tcl_NewStringObj(lastStart, (TCL_SIZE_T)length));
        if (contentObj != NULL) {
            Tcl_ListObjAppendElement(NULL, elementObj, contentObj);
        }
        Tcl_ListObjAppendElement(NULL, listObj, elementObj);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclParseHtmlObjCmd --
 *
 *      Implements "ns_parsehtml".
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
NsTclParseHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_OBJC_T objc, Tcl_Obj *const* objv)
{
    int          result = TCL_OK, withNoAngleOption = (int)NS_FALSE, onlyTagsOption = (int)NS_FALSE;
    char        *htmlString = (char *)NS_EMPTY_STRING;
    Ns_ObjvSpec opts[] = {
        {"-noangle",  Ns_ObjvBool, &withNoAngleOption, INT2PTR(NS_TRUE)},
        {"-onlytags", Ns_ObjvBool, &onlyTagsOption, INT2PTR(NS_TRUE)},
        {"--",        Ns_ObjvBreak, NULL,    NULL},
        {NULL, NULL, NULL, NULL}
    };
    Ns_ObjvSpec  args[] = {
        {"html", Ns_ObjvString,  &htmlString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (withNoAngleOption == NS_TRUE && onlyTagsOption == NS_TRUE) {
        Ns_TclPrintfResult(interp, "the options '-noangle' and '-onlytags' are mutually exclusive");
        result = TCL_ERROR;

    } else {
        bool        inTag;     /* flag to see if are we inside a tag */
        bool        inComment; /* flag to see if we are inside a comment */
        bool        inPi;      /* flag to see if we are inside a processing instruction */
        const char *ptr;       /* moving pointer to input string */
        const char *lastStart;
        Tcl_Obj    *listObj;
        bool        noAngle = withNoAngleOption ? NS_FALSE : NS_TRUE;
        bool        onlyTags = (bool)onlyTagsOption;

        lastStart  = htmlString;
        ptr        = htmlString;
        inTag      = NS_FALSE;
        inComment  = NS_FALSE;
        inPi       = NS_FALSE;

        listObj = Tcl_NewListObj(0, NULL);

        while (*ptr != '\0') {

            Ns_Log(Debug, "inptr %c inTag %d inComment %d string <%s>",
                   *ptr, inTag, inComment, ptr);

            if (inComment) {
                if ((*(ptr) == '-')
                    && (*(ptr + 1) == '-')
                    && (*(ptr + 2) == '>')) {
                    inComment  = NS_FALSE;
                    ptr += 2;
                    HtmlFinishElement(listObj, "comment", lastStart, ptr, noAngle, onlyTags, NULL);
                    lastStart = ptr + 1;
                }
            } else if (inPi) {
                if ((*(ptr) == '?')
                    && *(ptr + 1) == '>') {
                    inPi  = NS_FALSE;
                    ptr += 1;
                    HtmlFinishElement(listObj, "pi", lastStart, ptr, noAngle, onlyTags, NULL);
                    lastStart = ptr + 1;
                }
            } else if (inTag) {
                if (*ptr == '>') {
                    Tcl_Obj *contentObj;

                    contentObj = HtmlParseTagAtts(lastStart, ptr - lastStart);
                    /*
                     * Closing a tag.
                     */
                    inTag = NS_FALSE;
                    if (contentObj == NULL) {
                        /*
                         * Parsing of the tag content was syntactically not
                         * possible, therefore, fallback to treat the content
                         * as text, including the surrounding <> characters.
                         */
                        HtmlFinishElement(listObj, "text",
                                          lastStart-1, ptr+1, NS_FALSE, onlyTags, NULL);
                    } else {
                        HtmlFinishElement(listObj, "tag",
                                          lastStart, ptr, noAngle, onlyTags, contentObj);
                    }
                    lastStart = ptr + 1;
                }
            } else if (*ptr == '<'
                       && CHARTYPE(space,*(ptr + 1)) == 0
                       && strchr(ptr, '>') != NULL) {
                char nextChar = *(ptr + 1);

                if (ptr != lastStart) {
                    HtmlFinishElement(listObj, "text", lastStart, ptr, NS_FALSE, onlyTags, NULL);
                }
                lastStart = ptr + 1;
                /*
                 * We have either a tag (with potential arguments) or a comment.
                 */
                if ((nextChar == '!')
                    && (*(ptr + 2) == '-')
                    && (*(ptr + 3) == '-')) {
                    inTag = NS_FALSE;
                    inComment = NS_TRUE;
                } else if (nextChar == '?') {
                    inTag = NS_FALSE;
                    inPi = NS_TRUE;
                } else if (nextChar == '/'
                           || (nextChar >= 'a' && nextChar <= 'z')
                           || (nextChar >= 'A' && nextChar <= 'Z') ){
                    inTag = NS_TRUE;
                } else {
                    Ns_Log(Debug, "first character of tag '%c' is unknown, must be text: %s",
                           nextChar, htmlString);
                    lastStart = ptr;
                    ptr--;
                }
                ptr++;
            }
            ptr++;
        }
        if (ptr != lastStart) {
            HtmlFinishElement(listObj, "text", lastStart, ptr, NS_FALSE, onlyTags, NULL);
        }

        Tcl_SetObjResult(interp, listObj);
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
