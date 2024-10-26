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

static const char *htmlQuoteChars = "<>&'\"";

/*
 * Static functions defined in this file.
 */
static void QuoteHtml(Ns_DString *dsPtr, const char *breakChar, const char *htmlString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static bool WordEndsInSemi(const char *word, size_t *lengthPtr)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);

static int ToUTF8(long value, char *outPtr)
    NS_GNUC_NONNULL(2);

static size_t EntityDecode(const char *entity, ssize_t length, bool *needEncodePtr, char *outPtr, const char **toParse)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4) NS_GNUC_NONNULL(5);

static void
HtmlFinishElement(Tcl_Obj *listObj, const char* what, const char *lastStart,
                  const char *currentPtr, bool noAngle,  bool onlyTags, Tcl_Obj *contentObj)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3) NS_GNUC_NONNULL(4);
static Tcl_Obj *
HtmlParseTagAtts(const char *string, ptrdiff_t length)
    NS_GNUC_NONNULL(1);

static bool InitOnce(void);

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
        breakChar = strpbrk(toProcess, htmlQuoteChars);

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
     * If the first character is a NUL character, there is nothing to do.
     */
    if (*htmlString != '\0') {
        const char *breakChar = strpbrk(htmlString, htmlQuoteChars);

        if (breakChar != NULL) {
            QuoteHtml(dsPtr, strpbrk(htmlString, htmlQuoteChars), htmlString);
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
NsTclQuoteHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
            const char *breakChar = strpbrk(htmlString, htmlQuoteChars);

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
NsTclUnquoteHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
        TCL_SIZE_T  htmlLength;
        const char *htmlString = Tcl_GetStringFromObj(htmlObj, &htmlLength);
        const char *endOfString = htmlString + htmlLength;
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
                    size_t     entityLength = 0u, decoded = 0u;
                    TCL_SIZE_T prefixLength = (TCL_SIZE_T)(possibleEntity - htmlString);
                    TCL_SIZE_T oldLength;

                    /*
                     * Add the string leading to the ampersand to the output
                     * and proceed in the string by this amount of bytes.
                     */
                    if (possibleEntity != htmlString) {
                        Ns_DStringNAppend(dsPtr, htmlString, prefixLength);
                        htmlString += prefixLength;
                    }
                    oldLength = dsPtr->length;

                    /*
                     * The appended characters are max 8 bytes; make sure, we
                     * have this space in the Tcl_DString.
                     */
                    Tcl_DStringSetLength(dsPtr, oldLength + 8);

                    if (likely(WordEndsInSemi(possibleEntity, &entityLength))) {
                        decoded = EntityDecode(possibleEntity + 1u, (ssize_t)entityLength, &needEncode,
                                               dsPtr->string + oldLength, &htmlString);
                    }
                    if (unlikely(decoded == 0)) {
                        decoded = EntityDecode(possibleEntity + 1u, - (endOfString - (possibleEntity + 1)), &needEncode,
                                               dsPtr->string + oldLength, &htmlString);
                    }
                    Tcl_DStringSetLength(dsPtr, oldLength + (TCL_SIZE_T)decoded);

                    if (likely(decoded > 0)) {
                        htmlString++;
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
static size_t entityIndexTable[256] = {0};
static size_t legacyEntityIndexTable[256] = {0};

typedef struct namedEntity_t {
    const char *name;
    size_t length;
    const char *value;
    size_t outputLength;
} namedEntity_t;

static const namedEntity_t namedEntities[] = {
    {"AElig",                            5, "\xc3\x86",                    2},    /* "Æ" U+000C6 */
    {"AMP",                              3, "\x26",                        1},    /* "&" U+00026 */
    {"Aacute",                           6, "\xc3\x81",                    2},    /* "Á" U+000C1 */
    {"Abreve",                           6, "\xc4\x82",                    2},    /* "Ă" U+00102 */
    {"Acirc",                            5, "\xc3\x82",                    2},    /* "Â" U+000C2 */
    {"Acy",                              3, "\xd0\x90",                    2},    /* "А" U+00410 */
    {"Afr",                              3, "\xf0\x9d\x94\x84",            4},    /* "𝔄" U+1D504 */
    {"Agrave",                           6, "\xc3\x80",                    2},    /* "À" U+000C0 */
    {"Alpha",                            5, "\xce\x91",                    2},    /* "Α" U+00391 */
    {"Amacr",                            5, "\xc4\x80",                    2},    /* "Ā" U+00100 */
    {"And",                              3, "\xe2\xa9\x93",                3},    /* "⩓" U+02A53 */
    {"Aogon",                            5, "\xc4\x84",                    2},    /* "Ą" U+00104 */
    {"Aopf",                             4, "\xf0\x9d\x94\xb8",            4},    /* "𝔸" U+1D538 */
    {"ApplyFunction",                   13, "\xe2\x81\xa1",                3},    /* "⁡" U+02061 */
    {"Aring",                            5, "\xc3\x85",                    2},    /* "Å" U+000C5 */
    {"Ascr",                             4, "\xf0\x9d\x92\x9c",            4},    /* "𝒜" U+1D49C */
    {"Assign",                           6, "\xe2\x89\x94",                3},    /* "≔" U+02254 */
    {"Atilde",                           6, "\xc3\x83",                    2},    /* "Ã" U+000C3 */
    {"Auml",                             4, "\xc3\x84",                    2},    /* "Ä" U+000C4 */
    {"Backslash",                        9, "\xe2\x88\x96",                3},    /* "∖" U+02216 */
    {"Barv",                             4, "\xe2\xab\xa7",                3},    /* "⫧" U+02AE7 */
    {"Barwed",                           6, "\xe2\x8c\x86",                3},    /* "⌆" U+02306 */
    {"Bcy",                              3, "\xd0\x91",                    2},    /* "Б" U+00411 */
    {"Because",                          7, "\xe2\x88\xb5",                3},    /* "∵" U+02235 */
    {"Bernoullis",                      10, "\xe2\x84\xac",                3},    /* "ℬ" U+0212C */
    {"Beta",                             4, "\xce\x92",                    2},    /* "Β" U+00392 */
    {"Bfr",                              3, "\xf0\x9d\x94\x85",            4},    /* "𝔅" U+1D505 */
    {"Bopf",                             4, "\xf0\x9d\x94\xb9",            4},    /* "𝔹" U+1D539 */
    {"Breve",                            5, "\xcb\x98",                    2},    /* "˘" U+002D8 */
    {"Bscr",                             4, "\xe2\x84\xac",                3},    /* "ℬ" U+0212C */
    {"Bumpeq",                           6, "\xe2\x89\x8e",                3},    /* "≎" U+0224E */
    {"CHcy",                             4, "\xd0\xa7",                    2},    /* "Ч" U+00427 */
    {"COPY",                             4, "\xc2\xa9",                    2},    /* "©" U+000A9 */
    {"Cacute",                           6, "\xc4\x86",                    2},    /* "Ć" U+00106 */
    {"Cap",                              3, "\xe2\x8b\x92",                3},    /* "⋒" U+022D2 */
    {"CapitalDifferentialD",            20, "\xe2\x85\x85",                3},    /* "ⅅ" U+02145 */
    {"Cayleys",                          7, "\xe2\x84\xad",                3},    /* "ℭ" U+0212D */
    {"Ccaron",                           6, "\xc4\x8c",                    2},    /* "Č" U+0010C */
    {"Ccedil",                           6, "\xc3\x87",                    2},    /* "Ç" U+000C7 */
    {"Ccirc",                            5, "\xc4\x88",                    2},    /* "Ĉ" U+00108 */
    {"Cconint",                          7, "\xe2\x88\xb0",                3},    /* "∰" U+02230 */
    {"Cdot",                             4, "\xc4\x8a",                    2},    /* "Ċ" U+0010A */
    {"Cedilla",                          7, "\xc2\xb8",                    2},    /* "¸" U+000B8 */
    {"CenterDot",                        9, "\xc2\xb7",                    2},    /* "·" U+000B7 */
    {"Cfr",                              3, "\xe2\x84\xad",                3},    /* "ℭ" U+0212D */
    {"Chi",                              3, "\xce\xa7",                    2},    /* "Χ" U+003A7 */
    {"CircleDot",                        9, "\xe2\x8a\x99",                3},    /* "⊙" U+02299 */
    {"CircleMinus",                     11, "\xe2\x8a\x96",                3},    /* "⊖" U+02296 */
    {"CirclePlus",                      10, "\xe2\x8a\x95",                3},    /* "⊕" U+02295 */
    {"CircleTimes",                     11, "\xe2\x8a\x97",                3},    /* "⊗" U+02297 */
    {"ClockwiseContourIntegral",        24, "\xe2\x88\xb2",                3},    /* "∲" U+02232 */
    {"CloseCurlyDoubleQuote",           21, "\xe2\x80\x9d",                3},    /* "”" U+0201D */
    {"CloseCurlyQuote",                 15, "\xe2\x80\x99",                3},    /* "’" U+02019 */
    {"Colon",                            5, "\xe2\x88\xb7",                3},    /* "∷" U+02237 */
    {"Colone",                           6, "\xe2\xa9\xb4",                3},    /* "⩴" U+02A74 */
    {"Congruent",                        9, "\xe2\x89\xa1",                3},    /* "≡" U+02261 */
    {"Conint",                           6, "\xe2\x88\xaf",                3},    /* "∯" U+0222F */
    {"ContourIntegral",                 15, "\xe2\x88\xae",                3},    /* "∮" U+0222E */
    {"Copf",                             4, "\xe2\x84\x82",                3},    /* "ℂ" U+02102 */
    {"Coproduct",                        9, "\xe2\x88\x90",                3},    /* "∐" U+02210 */
    {"CounterClockwiseContourIntegral", 31, "\xe2\x88\xb3",                3},    /* "∳" U+02233 */
    {"Cross",                            5, "\xe2\xa8\xaf",                3},    /* "⨯" U+02A2F */
    {"Cscr",                             4, "\xf0\x9d\x92\x9e",            4},    /* "𝒞" U+1D49E */
    {"Cup",                              3, "\xe2\x8b\x93",                3},    /* "⋓" U+022D3 */
    {"CupCap",                           6, "\xe2\x89\x8d",                3},    /* "≍" U+0224D */
    {"DD",                               2, "\xe2\x85\x85",                3},    /* "ⅅ" U+02145 */
    {"DDotrahd",                         8, "\xe2\xa4\x91",                3},    /* "⤑" U+02911 */
    {"DJcy",                             4, "\xd0\x82",                    2},    /* "Ђ" U+00402 */
    {"DScy",                             4, "\xd0\x85",                    2},    /* "Ѕ" U+00405 */
    {"DZcy",                             4, "\xd0\x8f",                    2},    /* "Џ" U+0040F */
    {"Dagger",                           6, "\xe2\x80\xa1",                3},    /* "‡" U+02021 */
    {"Darr",                             4, "\xe2\x86\xa1",                3},    /* "↡" U+021A1 */
    {"Dashv",                            5, "\xe2\xab\xa4",                3},    /* "⫤" U+02AE4 */
    {"Dcaron",                           6, "\xc4\x8e",                    2},    /* "Ď" U+0010E */
    {"Dcy",                              3, "\xd0\x94",                    2},    /* "Д" U+00414 */
    {"Del",                              3, "\xe2\x88\x87",                3},    /* "∇" U+02207 */
    {"Delta",                            5, "\xce\x94",                    2},    /* "Δ" U+00394 */
    {"Dfr",                              3, "\xf0\x9d\x94\x87",            4},    /* "𝔇" U+1D507 */
    {"DiacriticalAcute",                16, "\xc2\xb4",                    2},    /* "´" U+000B4 */
    {"DiacriticalDot",                  14, "\xcb\x99",                    2},    /* "˙" U+002D9 */
    {"DiacriticalDoubleAcute",          22, "\xcb\x9d",                    2},    /* "˝" U+002DD */
    {"DiacriticalGrave",                16, "\x60",                        1},    /* "`" U+00060 */
    {"DiacriticalTilde",                16, "\xcb\x9c",                    2},    /* "˜" U+002DC */
    {"Diamond",                          7, "\xe2\x8b\x84",                3},    /* "⋄" U+022C4 */
    {"DifferentialD",                   13, "\xe2\x85\x86",                3},    /* "ⅆ" U+02146 */
    {"Dopf",                             4, "\xf0\x9d\x94\xbb",            4},    /* "𝔻" U+1D53B */
    {"Dot",                              3, "\xc2\xa8",                    2},    /* "¨" U+000A8 */
    {"DotDot",                           6, "\xe2\x83\x9c",                3},    /* "⃜" U+020DC */
    {"DotEqual",                         8, "\xe2\x89\x90",                3},    /* "≐" U+02250 */
    {"DoubleContourIntegral",           21, "\xe2\x88\xaf",                3},    /* "∯" U+0222F */
    {"DoubleDot",                        9, "\xc2\xa8",                    2},    /* "¨" U+000A8 */
    {"DoubleDownArrow",                 15, "\xe2\x87\x93",                3},    /* "⇓" U+021D3 */
    {"DoubleLeftArrow",                 15, "\xe2\x87\x90",                3},    /* "⇐" U+021D0 */
    {"DoubleLeftRightArrow",            20, "\xe2\x87\x94",                3},    /* "⇔" U+021D4 */
    {"DoubleLeftTee",                   13, "\xe2\xab\xa4",                3},    /* "⫤" U+02AE4 */
    {"DoubleLongLeftArrow",             19, "\xe2\x9f\xb8",                3},    /* "⟸" U+027F8 */
    {"DoubleLongLeftRightArrow",        24, "\xe2\x9f\xba",                3},    /* "⟺" U+027FA */
    {"DoubleLongRightArrow",            20, "\xe2\x9f\xb9",                3},    /* "⟹" U+027F9 */
    {"DoubleRightArrow",                16, "\xe2\x87\x92",                3},    /* "⇒" U+021D2 */
    {"DoubleRightTee",                  14, "\xe2\x8a\xa8",                3},    /* "⊨" U+022A8 */
    {"DoubleUpArrow",                   13, "\xe2\x87\x91",                3},    /* "⇑" U+021D1 */
    {"DoubleUpDownArrow",               17, "\xe2\x87\x95",                3},    /* "⇕" U+021D5 */
    {"DoubleVerticalBar",               17, "\xe2\x88\xa5",                3},    /* "∥" U+02225 */
    {"DownArrow",                        9, "\xe2\x86\x93",                3},    /* "↓" U+02193 */
    {"DownArrowBar",                    12, "\xe2\xa4\x93",                3},    /* "⤓" U+02913 */
    {"DownArrowUpArrow",                16, "\xe2\x87\xb5",                3},    /* "⇵" U+021F5 */
    {"DownBreve",                        9, "\xcc\x91",                    2},    /* "̑" U+00311 */
    {"DownLeftRightVector",             19, "\xe2\xa5\x90",                3},    /* "⥐" U+02950 */
    {"DownLeftTeeVector",               17, "\xe2\xa5\x9e",                3},    /* "⥞" U+0295E */
    {"DownLeftVector",                  14, "\xe2\x86\xbd",                3},    /* "↽" U+021BD */
    {"DownLeftVectorBar",               17, "\xe2\xa5\x96",                3},    /* "⥖" U+02956 */
    {"DownRightTeeVector",              18, "\xe2\xa5\x9f",                3},    /* "⥟" U+0295F */
    {"DownRightVector",                 15, "\xe2\x87\x81",                3},    /* "⇁" U+021C1 */
    {"DownRightVectorBar",              18, "\xe2\xa5\x97",                3},    /* "⥗" U+02957 */
    {"DownTee",                          7, "\xe2\x8a\xa4",                3},    /* "⊤" U+022A4 */
    {"DownTeeArrow",                    12, "\xe2\x86\xa7",                3},    /* "↧" U+021A7 */
    {"Downarrow",                        9, "\xe2\x87\x93",                3},    /* "⇓" U+021D3 */
    {"Dscr",                             4, "\xf0\x9d\x92\x9f",            4},    /* "𝒟" U+1D49F */
    {"Dstrok",                           6, "\xc4\x90",                    2},    /* "Đ" U+00110 */
    {"ENG",                              3, "\xc5\x8a",                    2},    /* "Ŋ" U+0014A */
    {"ETH",                              3, "\xc3\x90",                    2},    /* "Ð" U+000D0 */
    {"Eacute",                           6, "\xc3\x89",                    2},    /* "É" U+000C9 */
    {"Ecaron",                           6, "\xc4\x9a",                    2},    /* "Ě" U+0011A */
    {"Ecirc",                            5, "\xc3\x8a",                    2},    /* "Ê" U+000CA */
    {"Ecy",                              3, "\xd0\xad",                    2},    /* "Э" U+0042D */
    {"Edot",                             4, "\xc4\x96",                    2},    /* "Ė" U+00116 */
    {"Efr",                              3, "\xf0\x9d\x94\x88",            4},    /* "𝔈" U+1D508 */
    {"Egrave",                           6, "\xc3\x88",                    2},    /* "È" U+000C8 */
    {"Element",                          7, "\xe2\x88\x88",                3},    /* "∈" U+02208 */
    {"Emacr",                            5, "\xc4\x92",                    2},    /* "Ē" U+00112 */
    {"EmptySmallSquare",                16, "\xe2\x97\xbb",                3},    /* "◻" U+025FB */
    {"EmptyVerySmallSquare",            20, "\xe2\x96\xab",                3},    /* "▫" U+025AB */
    {"Eogon",                            5, "\xc4\x98",                    2},    /* "Ę" U+00118 */
    {"Eopf",                             4, "\xf0\x9d\x94\xbc",            4},    /* "𝔼" U+1D53C */
    {"Epsilon",                          7, "\xce\x95",                    2},    /* "Ε" U+00395 */
    {"Equal",                            5, "\xe2\xa9\xb5",                3},    /* "⩵" U+02A75 */
    {"EqualTilde",                      10, "\xe2\x89\x82",                3},    /* "≂" U+02242 */
    {"Equilibrium",                     11, "\xe2\x87\x8c",                3},    /* "⇌" U+021CC */
    {"Escr",                             4, "\xe2\x84\xb0",                3},    /* "ℰ" U+02130 */
    {"Esim",                             4, "\xe2\xa9\xb3",                3},    /* "⩳" U+02A73 */
    {"Eta",                              3, "\xce\x97",                    2},    /* "Η" U+00397 */
    {"Euml",                             4, "\xc3\x8b",                    2},    /* "Ë" U+000CB */
    {"Exists",                           6, "\xe2\x88\x83",                3},    /* "∃" U+02203 */
    {"ExponentialE",                    12, "\xe2\x85\x87",                3},    /* "ⅇ" U+02147 */
    {"Fcy",                              3, "\xd0\xa4",                    2},    /* "Ф" U+00424 */
    {"Ffr",                              3, "\xf0\x9d\x94\x89",            4},    /* "𝔉" U+1D509 */
    {"FilledSmallSquare",               17, "\xe2\x97\xbc",                3},    /* "◼" U+025FC */
    {"FilledVerySmallSquare",           21, "\xe2\x96\xaa",                3},    /* "▪" U+025AA */
    {"Fopf",                             4, "\xf0\x9d\x94\xbd",            4},    /* "𝔽" U+1D53D */
    {"ForAll",                           6, "\xe2\x88\x80",                3},    /* "∀" U+02200 */
    {"Fouriertrf",                      10, "\xe2\x84\xb1",                3},    /* "ℱ" U+02131 */
    {"Fscr",                             4, "\xe2\x84\xb1",                3},    /* "ℱ" U+02131 */
    {"GJcy",                             4, "\xd0\x83",                    2},    /* "Ѓ" U+00403 */
    {"GT",                               2, "\x3e",                        1},    /* ">" U+0003E */
    {"Gamma",                            5, "\xce\x93",                    2},    /* "Γ" U+00393 */
    {"Gammad",                           6, "\xcf\x9c",                    2},    /* "Ϝ" U+003DC */
    {"Gbreve",                           6, "\xc4\x9e",                    2},    /* "Ğ" U+0011E */
    {"Gcedil",                           6, "\xc4\xa2",                    2},    /* "Ģ" U+00122 */
    {"Gcirc",                            5, "\xc4\x9c",                    2},    /* "Ĝ" U+0011C */
    {"Gcy",                              3, "\xd0\x93",                    2},    /* "Г" U+00413 */
    {"Gdot",                             4, "\xc4\xa0",                    2},    /* "Ġ" U+00120 */
    {"Gfr",                              3, "\xf0\x9d\x94\x8a",            4},    /* "𝔊" U+1D50A */
    {"Gg",                               2, "\xe2\x8b\x99",                3},    /* "⋙" U+022D9 */
    {"Gopf",                             4, "\xf0\x9d\x94\xbe",            4},    /* "𝔾" U+1D53E */
    {"GreaterEqual",                    12, "\xe2\x89\xa5",                3},    /* "≥" U+02265 */
    {"GreaterEqualLess",                16, "\xe2\x8b\x9b",                3},    /* "⋛" U+022DB */
    {"GreaterFullEqual",                16, "\xe2\x89\xa7",                3},    /* "≧" U+02267 */
    {"GreaterGreater",                  14, "\xe2\xaa\xa2",                3},    /* "⪢" U+02AA2 */
    {"GreaterLess",                     11, "\xe2\x89\xb7",                3},    /* "≷" U+02277 */
    {"GreaterSlantEqual",               17, "\xe2\xa9\xbe",                3},    /* "⩾" U+02A7E */
    {"GreaterTilde",                    12, "\xe2\x89\xb3",                3},    /* "≳" U+02273 */
    {"Gscr",                             4, "\xf0\x9d\x92\xa2",            4},    /* "𝒢" U+1D4A2 */
    {"Gt",                               2, "\xe2\x89\xab",                3},    /* "≫" U+0226B */
    {"HARDcy",                           6, "\xd0\xaa",                    2},    /* "Ъ" U+0042A */
    {"Hacek",                            5, "\xcb\x87",                    2},    /* "ˇ" U+002C7 */
    {"Hat",                              3, "\x5e",                        1},    /* "^" U+0005E */
    {"Hcirc",                            5, "\xc4\xa4",                    2},    /* "Ĥ" U+00124 */
    {"Hfr",                              3, "\xe2\x84\x8c",                3},    /* "ℌ" U+0210C */
    {"HilbertSpace",                    12, "\xe2\x84\x8b",                3},    /* "ℋ" U+0210B */
    {"Hopf",                             4, "\xe2\x84\x8d",                3},    /* "ℍ" U+0210D */
    {"HorizontalLine",                  14, "\xe2\x94\x80",                3},    /* "─" U+02500 */
    {"Hscr",                             4, "\xe2\x84\x8b",                3},    /* "ℋ" U+0210B */
    {"Hstrok",                           6, "\xc4\xa6",                    2},    /* "Ħ" U+00126 */
    {"HumpDownHump",                    12, "\xe2\x89\x8e",                3},    /* "≎" U+0224E */
    {"HumpEqual",                        9, "\xe2\x89\x8f",                3},    /* "≏" U+0224F */
    {"IEcy",                             4, "\xd0\x95",                    2},    /* "Е" U+00415 */
    {"IJlig",                            5, "\xc4\xb2",                    2},    /* "Ĳ" U+00132 */
    {"IOcy",                             4, "\xd0\x81",                    2},    /* "Ё" U+00401 */
    {"Iacute",                           6, "\xc3\x8d",                    2},    /* "Í" U+000CD */
    {"Icirc",                            5, "\xc3\x8e",                    2},    /* "Î" U+000CE */
    {"Icy",                              3, "\xd0\x98",                    2},    /* "И" U+00418 */
    {"Idot",                             4, "\xc4\xb0",                    2},    /* "İ" U+00130 */
    {"Ifr",                              3, "\xe2\x84\x91",                3},    /* "ℑ" U+02111 */
    {"Igrave",                           6, "\xc3\x8c",                    2},    /* "Ì" U+000CC */
    {"Im",                               2, "\xe2\x84\x91",                3},    /* "ℑ" U+02111 */
    {"Imacr",                            5, "\xc4\xaa",                    2},    /* "Ī" U+0012A */
    {"ImaginaryI",                      10, "\xe2\x85\x88",                3},    /* "ⅈ" U+02148 */
    {"Implies",                          7, "\xe2\x87\x92",                3},    /* "⇒" U+021D2 */
    {"Int",                              3, "\xe2\x88\xac",                3},    /* "∬" U+0222C */
    {"Integral",                         8, "\xe2\x88\xab",                3},    /* "∫" U+0222B */
    {"Intersection",                    12, "\xe2\x8b\x82",                3},    /* "⋂" U+022C2 */
    {"InvisibleComma",                  14, "\xe2\x81\xa3",                3},    /* "⁣" U+02063 */
    {"InvisibleTimes",                  14, "\xe2\x81\xa2",                3},    /* "⁢" U+02062 */
    {"Iogon",                            5, "\xc4\xae",                    2},    /* "Į" U+0012E */
    {"Iopf",                             4, "\xf0\x9d\x95\x80",            4},    /* "𝕀" U+1D540 */
    {"Iota",                             4, "\xce\x99",                    2},    /* "Ι" U+00399 */
    {"Iscr",                             4, "\xe2\x84\x90",                3},    /* "ℐ" U+02110 */
    {"Itilde",                           6, "\xc4\xa8",                    2},    /* "Ĩ" U+00128 */
    {"Iukcy",                            5, "\xd0\x86",                    2},    /* "І" U+00406 */
    {"Iuml",                             4, "\xc3\x8f",                    2},    /* "Ï" U+000CF */
    {"Jcirc",                            5, "\xc4\xb4",                    2},    /* "Ĵ" U+00134 */
    {"Jcy",                              3, "\xd0\x99",                    2},    /* "Й" U+00419 */
    {"Jfr",                              3, "\xf0\x9d\x94\x8d",            4},    /* "𝔍" U+1D50D */
    {"Jopf",                             4, "\xf0\x9d\x95\x81",            4},    /* "𝕁" U+1D541 */
    {"Jscr",                             4, "\xf0\x9d\x92\xa5",            4},    /* "𝒥" U+1D4A5 */
    {"Jsercy",                           6, "\xd0\x88",                    2},    /* "Ј" U+00408 */
    {"Jukcy",                            5, "\xd0\x84",                    2},    /* "Є" U+00404 */
    {"KHcy",                             4, "\xd0\xa5",                    2},    /* "Х" U+00425 */
    {"KJcy",                             4, "\xd0\x8c",                    2},    /* "Ќ" U+0040C */
    {"Kappa",                            5, "\xce\x9a",                    2},    /* "Κ" U+0039A */
    {"Kcedil",                           6, "\xc4\xb6",                    2},    /* "Ķ" U+00136 */
    {"Kcy",                              3, "\xd0\x9a",                    2},    /* "К" U+0041A */
    {"Kfr",                              3, "\xf0\x9d\x94\x8e",            4},    /* "𝔎" U+1D50E */
    {"Kopf",                             4, "\xf0\x9d\x95\x82",            4},    /* "𝕂" U+1D542 */
    {"Kscr",                             4, "\xf0\x9d\x92\xa6",            4},    /* "𝒦" U+1D4A6 */
    {"LJcy",                             4, "\xd0\x89",                    2},    /* "Љ" U+00409 */
    {"LT",                               2, "\x3c",                        1},    /* "<" U+0003C */
    {"Lacute",                           6, "\xc4\xb9",                    2},    /* "Ĺ" U+00139 */
    {"Lambda",                           6, "\xce\x9b",                    2},    /* "Λ" U+0039B */
    {"Lang",                             4, "\xe2\x9f\xaa",                3},    /* "⟪" U+027EA */
    {"Laplacetrf",                      10, "\xe2\x84\x92",                3},    /* "ℒ" U+02112 */
    {"Larr",                             4, "\xe2\x86\x9e",                3},    /* "↞" U+0219E */
    {"Lcaron",                           6, "\xc4\xbd",                    2},    /* "Ľ" U+0013D */
    {"Lcedil",                           6, "\xc4\xbb",                    2},    /* "Ļ" U+0013B */
    {"Lcy",                              3, "\xd0\x9b",                    2},    /* "Л" U+0041B */
    {"LeftAngleBracket",                16, "\xe2\x9f\xa8",                3},    /* "⟨" U+027E8 */
    {"LeftArrow",                        9, "\xe2\x86\x90",                3},    /* "←" U+02190 */
    {"LeftArrowBar",                    12, "\xe2\x87\xa4",                3},    /* "⇤" U+021E4 */
    {"LeftArrowRightArrow",             19, "\xe2\x87\x86",                3},    /* "⇆" U+021C6 */
    {"LeftCeiling",                     11, "\xe2\x8c\x88",                3},    /* "⌈" U+02308 */
    {"LeftDoubleBracket",               17, "\xe2\x9f\xa6",                3},    /* "⟦" U+027E6 */
    {"LeftDownTeeVector",               17, "\xe2\xa5\xa1",                3},    /* "⥡" U+02961 */
    {"LeftDownVector",                  14, "\xe2\x87\x83",                3},    /* "⇃" U+021C3 */
    {"LeftDownVectorBar",               17, "\xe2\xa5\x99",                3},    /* "⥙" U+02959 */
    {"LeftFloor",                        9, "\xe2\x8c\x8a",                3},    /* "⌊" U+0230A */
    {"LeftRightArrow",                  14, "\xe2\x86\x94",                3},    /* "↔" U+02194 */
    {"LeftRightVector",                 15, "\xe2\xa5\x8e",                3},    /* "⥎" U+0294E */
    {"LeftTee",                          7, "\xe2\x8a\xa3",                3},    /* "⊣" U+022A3 */
    {"LeftTeeArrow",                    12, "\xe2\x86\xa4",                3},    /* "↤" U+021A4 */
    {"LeftTeeVector",                   13, "\xe2\xa5\x9a",                3},    /* "⥚" U+0295A */
    {"LeftTriangle",                    12, "\xe2\x8a\xb2",                3},    /* "⊲" U+022B2 */
    {"LeftTriangleBar",                 15, "\xe2\xa7\x8f",                3},    /* "⧏" U+029CF */
    {"LeftTriangleEqual",               17, "\xe2\x8a\xb4",                3},    /* "⊴" U+022B4 */
    {"LeftUpDownVector",                16, "\xe2\xa5\x91",                3},    /* "⥑" U+02951 */
    {"LeftUpTeeVector",                 15, "\xe2\xa5\xa0",                3},    /* "⥠" U+02960 */
    {"LeftUpVector",                    12, "\xe2\x86\xbf",                3},    /* "↿" U+021BF */
    {"LeftUpVectorBar",                 15, "\xe2\xa5\x98",                3},    /* "⥘" U+02958 */
    {"LeftVector",                      10, "\xe2\x86\xbc",                3},    /* "↼" U+021BC */
    {"LeftVectorBar",                   13, "\xe2\xa5\x92",                3},    /* "⥒" U+02952 */
    {"Leftarrow",                        9, "\xe2\x87\x90",                3},    /* "⇐" U+021D0 */
    {"Leftrightarrow",                  14, "\xe2\x87\x94",                3},    /* "⇔" U+021D4 */
    {"LessEqualGreater",                16, "\xe2\x8b\x9a",                3},    /* "⋚" U+022DA */
    {"LessFullEqual",                   13, "\xe2\x89\xa6",                3},    /* "≦" U+02266 */
    {"LessGreater",                     11, "\xe2\x89\xb6",                3},    /* "≶" U+02276 */
    {"LessLess",                         8, "\xe2\xaa\xa1",                3},    /* "⪡" U+02AA1 */
    {"LessSlantEqual",                  14, "\xe2\xa9\xbd",                3},    /* "⩽" U+02A7D */
    {"LessTilde",                        9, "\xe2\x89\xb2",                3},    /* "≲" U+02272 */
    {"Lfr",                              3, "\xf0\x9d\x94\x8f",            4},    /* "𝔏" U+1D50F */
    {"Ll",                               2, "\xe2\x8b\x98",                3},    /* "⋘" U+022D8 */
    {"Lleftarrow",                      10, "\xe2\x87\x9a",                3},    /* "⇚" U+021DA */
    {"Lmidot",                           6, "\xc4\xbf",                    2},    /* "Ŀ" U+0013F */
    {"LongLeftArrow",                   13, "\xe2\x9f\xb5",                3},    /* "⟵" U+027F5 */
    {"LongLeftRightArrow",              18, "\xe2\x9f\xb7",                3},    /* "⟷" U+027F7 */
    {"LongRightArrow",                  14, "\xe2\x9f\xb6",                3},    /* "⟶" U+027F6 */
    {"Longleftarrow",                   13, "\xe2\x9f\xb8",                3},    /* "⟸" U+027F8 */
    {"Longleftrightarrow",              18, "\xe2\x9f\xba",                3},    /* "⟺" U+027FA */
    {"Longrightarrow",                  14, "\xe2\x9f\xb9",                3},    /* "⟹" U+027F9 */
    {"Lopf",                             4, "\xf0\x9d\x95\x83",            4},    /* "𝕃" U+1D543 */
    {"LowerLeftArrow",                  14, "\xe2\x86\x99",                3},    /* "↙" U+02199 */
    {"LowerRightArrow",                 15, "\xe2\x86\x98",                3},    /* "↘" U+02198 */
    {"Lscr",                             4, "\xe2\x84\x92",                3},    /* "ℒ" U+02112 */
    {"Lsh",                              3, "\xe2\x86\xb0",                3},    /* "↰" U+021B0 */
    {"Lstrok",                           6, "\xc5\x81",                    2},    /* "Ł" U+00141 */
    {"Lt",                               2, "\xe2\x89\xaa",                3},    /* "≪" U+0226A */
    {"Map",                              3, "\xe2\xa4\x85",                3},    /* "⤅" U+02905 */
    {"Mcy",                              3, "\xd0\x9c",                    2},    /* "М" U+0041C */
    {"MediumSpace",                     11, "\xe2\x81\x9f",                3},    /* " " U+0205F */
    {"Mellintrf",                        9, "\xe2\x84\xb3",                3},    /* "ℳ" U+02133 */
    {"Mfr",                              3, "\xf0\x9d\x94\x90",            4},    /* "𝔐" U+1D510 */
    {"MinusPlus",                        9, "\xe2\x88\x93",                3},    /* "∓" U+02213 */
    {"Mopf",                             4, "\xf0\x9d\x95\x84",            4},    /* "𝕄" U+1D544 */
    {"Mscr",                             4, "\xe2\x84\xb3",                3},    /* "ℳ" U+02133 */
    {"Mu",                               2, "\xce\x9c",                    2},    /* "Μ" U+0039C */
    {"NJcy",                             4, "\xd0\x8a",                    2},    /* "Њ" U+0040A */
    {"Nacute",                           6, "\xc5\x83",                    2},    /* "Ń" U+00143 */
    {"Ncaron",                           6, "\xc5\x87",                    2},    /* "Ň" U+00147 */
    {"Ncedil",                           6, "\xc5\x85",                    2},    /* "Ņ" U+00145 */
    {"Ncy",                              3, "\xd0\x9d",                    2},    /* "Н" U+0041D */
    {"NegativeMediumSpace",             19, "\xe2\x80\x8b",                3},    /* "​" U+0200B */
    {"NegativeThickSpace",              18, "\xe2\x80\x8b",                3},    /* "​" U+0200B */
    {"NegativeThinSpace",               17, "\xe2\x80\x8b",                3},    /* "​" U+0200B */
    {"NegativeVeryThinSpace",           21, "\xe2\x80\x8b",                3},    /* "​" U+0200B */
    {"NestedGreaterGreater",            20, "\xe2\x89\xab",                3},    /* "≫" U+0226B */
    {"NestedLessLess",                  14, "\xe2\x89\xaa",                3},    /* "≪" U+0226A */
    {"NewLine",                          7, "\x0a",                        1},    /* NewLine U+0000A */
    {"Nfr",                              3, "\xf0\x9d\x94\x91",            4},    /* "𝔑" U+1D511 */
    {"NoBreak",                          7, "\xe2\x81\xa0",                3},    /* "⁠" U+02060 */
    {"NonBreakingSpace",                16, "\xc2\xa0",                    2},    /* " " U+000A0 */
    {"Nopf",                             4, "\xe2\x84\x95",                3},    /* "ℕ" U+02115 */
    {"Not",                              3, "\xe2\xab\xac",                3},    /* "⫬" U+02AEC */
    {"NotCongruent",                    12, "\xe2\x89\xa2",                3},    /* "≢" U+02262 */
    {"NotCupCap",                        9, "\xe2\x89\xad",                3},    /* "≭" U+0226D */
    {"NotDoubleVerticalBar",            20, "\xe2\x88\xa6",                3},    /* "∦" U+02226 */
    {"NotElement",                      10, "\xe2\x88\x89",                3},    /* "∉" U+02209 */
    {"NotEqual",                         8, "\xe2\x89\xa0",                3},    /* "≠" U+02260 */
    {"NotEqualTilde",                   13, "\xe2\x89\x82\xcc\xb8",        5},    /* "≂̸" U+02242 U+00338 */
    {"NotExists",                        9, "\xe2\x88\x84",                3},    /* "∄" U+02204 */
    {"NotGreater",                      10, "\xe2\x89\xaf",                3},    /* "≯" U+0226F */
    {"NotGreaterEqual",                 15, "\xe2\x89\xb1",                3},    /* "≱" U+02271 */
    {"NotGreaterFullEqual",             19, "\xe2\x89\xa7\xcc\xb8",        5},    /* "≧̸" U+02267 U+00338 */
    {"NotGreaterGreater",               17, "\xe2\x89\xab\xcc\xb8",        5},    /* "≫̸" U+0226B U+00338 */
    {"NotGreaterLess",                  14, "\xe2\x89\xb9",                3},    /* "≹" U+02279 */
    {"NotGreaterSlantEqual",            20, "\xe2\xa9\xbe\xcc\xb8",        5},    /* "⩾̸" U+02A7E U+00338 */
    {"NotGreaterTilde",                 15, "\xe2\x89\xb5",                3},    /* "≵" U+02275 */
    {"NotHumpDownHump",                 15, "\xe2\x89\x8e\xcc\xb8",        5},    /* "≎̸" U+0224E U+00338 */
    {"NotHumpEqual",                    12, "\xe2\x89\x8f\xcc\xb8",        5},    /* "≏̸" U+0224F U+00338 */
    {"NotLeftTriangle",                 15, "\xe2\x8b\xaa",                3},    /* "⋪" U+022EA */
    {"NotLeftTriangleBar",              18, "\xe2\xa7\x8f\xcc\xb8",        5},    /* "⧏̸" U+029CF U+00338 */
    {"NotLeftTriangleEqual",            20, "\xe2\x8b\xac",                3},    /* "⋬" U+022EC */
    {"NotLess",                          7, "\xe2\x89\xae",                3},    /* "≮" U+0226E */
    {"NotLessEqual",                    12, "\xe2\x89\xb0",                3},    /* "≰" U+02270 */
    {"NotLessGreater",                  14, "\xe2\x89\xb8",                3},    /* "≸" U+02278 */
    {"NotLessLess",                     11, "\xe2\x89\xaa\xcc\xb8",        5},    /* "≪̸" U+0226A U+00338 */
    {"NotLessSlantEqual",               17, "\xe2\xa9\xbd\xcc\xb8",        5},    /* "⩽̸" U+02A7D U+00338 */
    {"NotLessTilde",                    12, "\xe2\x89\xb4",                3},    /* "≴" U+02274 */
    {"NotNestedGreaterGreater",         23, "\xe2\xaa\xa2\xcc\xb8",        5},    /* "⪢̸" U+02AA2 U+00338 */
    {"NotNestedLessLess",               17, "\xe2\xaa\xa1\xcc\xb8",        5},    /* "⪡̸" U+02AA1 U+00338 */
    {"NotPrecedes",                     11, "\xe2\x8a\x80",                3},    /* "⊀" U+02280 */
    {"NotPrecedesEqual",                16, "\xe2\xaa\xaf\xcc\xb8",        5},    /* "⪯̸" U+02AAF U+00338 */
    {"NotPrecedesSlantEqual",           21, "\xe2\x8b\xa0",                3},    /* "⋠" U+022E0 */
    {"NotReverseElement",               17, "\xe2\x88\x8c",                3},    /* "∌" U+0220C */
    {"NotRightTriangle",                16, "\xe2\x8b\xab",                3},    /* "⋫" U+022EB */
    {"NotRightTriangleBar",             19, "\xe2\xa7\x90\xcc\xb8",        5},    /* "⧐̸" U+029D0 U+00338 */
    {"NotRightTriangleEqual",           21, "\xe2\x8b\xad",                3},    /* "⋭" U+022ED */
    {"NotSquareSubset",                 15, "\xe2\x8a\x8f\xcc\xb8",        5},    /* "⊏̸" U+0228F U+00338 */
    {"NotSquareSubsetEqual",            20, "\xe2\x8b\xa2",                3},    /* "⋢" U+022E2 */
    {"NotSquareSuperset",               17, "\xe2\x8a\x90\xcc\xb8",        5},    /* "⊐̸" U+02290 U+00338 */
    {"NotSquareSupersetEqual",          22, "\xe2\x8b\xa3",                3},    /* "⋣" U+022E3 */
    {"NotSubset",                        9, "\xe2\x8a\x82\xe2\x83\x92",    6},    /* "⊂⃒" U+02282 U+020D2 */
    {"NotSubsetEqual",                  14, "\xe2\x8a\x88",                3},    /* "⊈" U+02288 */
    {"NotSucceeds",                     11, "\xe2\x8a\x81",                3},    /* "⊁" U+02281 */
    {"NotSucceedsEqual",                16, "\xe2\xaa\xb0\xcc\xb8",        5},    /* "⪰̸" U+02AB0 U+00338 */
    {"NotSucceedsSlantEqual",           21, "\xe2\x8b\xa1",                3},    /* "⋡" U+022E1 */
    {"NotSucceedsTilde",                16, "\xe2\x89\xbf\xcc\xb8",        5},    /* "≿̸" U+0227F U+00338 */
    {"NotSuperset",                     11, "\xe2\x8a\x83\xe2\x83\x92",    6},    /* "⊃⃒" U+02283 U+020D2 */
    {"NotSupersetEqual",                16, "\xe2\x8a\x89",                3},    /* "⊉" U+02289 */
    {"NotTilde",                         8, "\xe2\x89\x81",                3},    /* "≁" U+02241 */
    {"NotTildeEqual",                   13, "\xe2\x89\x84",                3},    /* "≄" U+02244 */
    {"NotTildeFullEqual",               17, "\xe2\x89\x87",                3},    /* "≇" U+02247 */
    {"NotTildeTilde",                   13, "\xe2\x89\x89",                3},    /* "≉" U+02249 */
    {"NotVerticalBar",                  14, "\xe2\x88\xa4",                3},    /* "∤" U+02224 */
    {"Nscr",                             4, "\xf0\x9d\x92\xa9",            4},    /* "𝒩" U+1D4A9 */
    {"Ntilde",                           6, "\xc3\x91",                    2},    /* "Ñ" U+000D1 */
    {"Nu",                               2, "\xce\x9d",                    2},    /* "Ν" U+0039D */
    {"OElig",                            5, "\xc5\x92",                    2},    /* "Œ" U+00152 */
    {"Oacute",                           6, "\xc3\x93",                    2},    /* "Ó" U+000D3 */
    {"Ocirc",                            5, "\xc3\x94",                    2},    /* "Ô" U+000D4 */
    {"Ocy",                              3, "\xd0\x9e",                    2},    /* "О" U+0041E */
    {"Odblac",                           6, "\xc5\x90",                    2},    /* "Ő" U+00150 */
    {"Ofr",                              3, "\xf0\x9d\x94\x92",            4},    /* "𝔒" U+1D512 */
    {"Ograve",                           6, "\xc3\x92",                    2},    /* "Ò" U+000D2 */
    {"Omacr",                            5, "\xc5\x8c",                    2},    /* "Ō" U+0014C */
    {"Omega",                            5, "\xce\xa9",                    2},    /* "Ω" U+003A9 */
    {"Omicron",                          7, "\xce\x9f",                    2},    /* "Ο" U+0039F */
    {"Oopf",                             4, "\xf0\x9d\x95\x86",            4},    /* "𝕆" U+1D546 */
    {"OpenCurlyDoubleQuote",            20, "\xe2\x80\x9c",                3},    /* "“" U+0201C */
    {"OpenCurlyQuote",                  14, "\xe2\x80\x98",                3},    /* "‘" U+02018 */
    {"Or",                               2, "\xe2\xa9\x94",                3},    /* "⩔" U+02A54 */
    {"Oscr",                             4, "\xf0\x9d\x92\xaa",            4},    /* "𝒪" U+1D4AA */
    {"Oslash",                           6, "\xc3\x98",                    2},    /* "Ø" U+000D8 */
    {"Otilde",                           6, "\xc3\x95",                    2},    /* "Õ" U+000D5 */
    {"Otimes",                           6, "\xe2\xa8\xb7",                3},    /* "⨷" U+02A37 */
    {"Ouml",                             4, "\xc3\x96",                    2},    /* "Ö" U+000D6 */
    {"OverBar",                          7, "\xe2\x80\xbe",                3},    /* "‾" U+0203E */
    {"OverBrace",                        9, "\xe2\x8f\x9e",                3},    /* "⏞" U+023DE */
    {"OverBracket",                     11, "\xe2\x8e\xb4",                3},    /* "⎴" U+023B4 */
    {"OverParenthesis",                 15, "\xe2\x8f\x9c",                3},    /* "⏜" U+023DC */
    {"PartialD",                         8, "\xe2\x88\x82",                3},    /* "∂" U+02202 */
    {"Pcy",                              3, "\xd0\x9f",                    2},    /* "П" U+0041F */
    {"Pfr",                              3, "\xf0\x9d\x94\x93",            4},    /* "𝔓" U+1D513 */
    {"Phi",                              3, "\xce\xa6",                    2},    /* "Φ" U+003A6 */
    {"Pi",                               2, "\xce\xa0",                    2},    /* "Π" U+003A0 */
    {"PlusMinus",                        9, "\xc2\xb1",                    2},    /* "±" U+000B1 */
    {"Poincareplane",                   13, "\xe2\x84\x8c",                3},    /* "ℌ" U+0210C */
    {"Popf",                             4, "\xe2\x84\x99",                3},    /* "ℙ" U+02119 */
    {"Pr",                               2, "\xe2\xaa\xbb",                3},    /* "⪻" U+02ABB */
    {"Precedes",                         8, "\xe2\x89\xba",                3},    /* "≺" U+0227A */
    {"PrecedesEqual",                   13, "\xe2\xaa\xaf",                3},    /* "⪯" U+02AAF */
    {"PrecedesSlantEqual",              18, "\xe2\x89\xbc",                3},    /* "≼" U+0227C */
    {"PrecedesTilde",                   13, "\xe2\x89\xbe",                3},    /* "≾" U+0227E */
    {"Prime",                            5, "\xe2\x80\xb3",                3},    /* "″" U+02033 */
    {"Product",                          7, "\xe2\x88\x8f",                3},    /* "∏" U+0220F */
    {"Proportion",                      10, "\xe2\x88\xb7",                3},    /* "∷" U+02237 */
    {"Proportional",                    12, "\xe2\x88\x9d",                3},    /* "∝" U+0221D */
    {"Pscr",                             4, "\xf0\x9d\x92\xab",            4},    /* "𝒫" U+1D4AB */
    {"Psi",                              3, "\xce\xa8",                    2},    /* "Ψ" U+003A8 */
    {"QUOT",                             4, "\x22",                        1},    /* """ U+00022 */
    {"Qfr",                              3, "\xf0\x9d\x94\x94",            4},    /* "𝔔" U+1D514 */
    {"Qopf",                             4, "\xe2\x84\x9a",                3},    /* "ℚ" U+0211A */
    {"Qscr",                             4, "\xf0\x9d\x92\xac",            4},    /* "𝒬" U+1D4AC */
    {"RBarr",                            5, "\xe2\xa4\x90",                3},    /* "⤐" U+02910 */
    {"REG",                              3, "\xc2\xae",                    2},    /* "®" U+000AE */
    {"Racute",                           6, "\xc5\x94",                    2},    /* "Ŕ" U+00154 */
    {"Rang",                             4, "\xe2\x9f\xab",                3},    /* "⟫" U+027EB */
    {"Rarr",                             4, "\xe2\x86\xa0",                3},    /* "↠" U+021A0 */
    {"Rarrtl",                           6, "\xe2\xa4\x96",                3},    /* "⤖" U+02916 */
    {"Rcaron",                           6, "\xc5\x98",                    2},    /* "Ř" U+00158 */
    {"Rcedil",                           6, "\xc5\x96",                    2},    /* "Ŗ" U+00156 */
    {"Rcy",                              3, "\xd0\xa0",                    2},    /* "Р" U+00420 */
    {"Re",                               2, "\xe2\x84\x9c",                3},    /* "ℜ" U+0211C */
    {"ReverseElement",                  14, "\xe2\x88\x8b",                3},    /* "∋" U+0220B */
    {"ReverseEquilibrium",              18, "\xe2\x87\x8b",                3},    /* "⇋" U+021CB */
    {"ReverseUpEquilibrium",            20, "\xe2\xa5\xaf",                3},    /* "⥯" U+0296F */
    {"Rfr",                              3, "\xe2\x84\x9c",                3},    /* "ℜ" U+0211C */
    {"Rho",                              3, "\xce\xa1",                    2},    /* "Ρ" U+003A1 */
    {"RightAngleBracket",               17, "\xe2\x9f\xa9",                3},    /* "⟩" U+027E9 */
    {"RightArrow",                      10, "\xe2\x86\x92",                3},    /* "→" U+02192 */
    {"RightArrowBar",                   13, "\xe2\x87\xa5",                3},    /* "⇥" U+021E5 */
    {"RightArrowLeftArrow",             19, "\xe2\x87\x84",                3},    /* "⇄" U+021C4 */
    {"RightCeiling",                    12, "\xe2\x8c\x89",                3},    /* "⌉" U+02309 */
    {"RightDoubleBracket",              18, "\xe2\x9f\xa7",                3},    /* "⟧" U+027E7 */
    {"RightDownTeeVector",              18, "\xe2\xa5\x9d",                3},    /* "⥝" U+0295D */
    {"RightDownVector",                 15, "\xe2\x87\x82",                3},    /* "⇂" U+021C2 */
    {"RightDownVectorBar",              18, "\xe2\xa5\x95",                3},    /* "⥕" U+02955 */
    {"RightFloor",                      10, "\xe2\x8c\x8b",                3},    /* "⌋" U+0230B */
    {"RightTee",                         8, "\xe2\x8a\xa2",                3},    /* "⊢" U+022A2 */
    {"RightTeeArrow",                   13, "\xe2\x86\xa6",                3},    /* "↦" U+021A6 */
    {"RightTeeVector",                  14, "\xe2\xa5\x9b",                3},    /* "⥛" U+0295B */
    {"RightTriangle",                   13, "\xe2\x8a\xb3",                3},    /* "⊳" U+022B3 */
    {"RightTriangleBar",                16, "\xe2\xa7\x90",                3},    /* "⧐" U+029D0 */
    {"RightTriangleEqual",              18, "\xe2\x8a\xb5",                3},    /* "⊵" U+022B5 */
    {"RightUpDownVector",               17, "\xe2\xa5\x8f",                3},    /* "⥏" U+0294F */
    {"RightUpTeeVector",                16, "\xe2\xa5\x9c",                3},    /* "⥜" U+0295C */
    {"RightUpVector",                   13, "\xe2\x86\xbe",                3},    /* "↾" U+021BE */
    {"RightUpVectorBar",                16, "\xe2\xa5\x94",                3},    /* "⥔" U+02954 */
    {"RightVector",                     11, "\xe2\x87\x80",                3},    /* "⇀" U+021C0 */
    {"RightVectorBar",                  14, "\xe2\xa5\x93",                3},    /* "⥓" U+02953 */
    {"Rightarrow",                      10, "\xe2\x87\x92",                3},    /* "⇒" U+021D2 */
    {"Ropf",                             4, "\xe2\x84\x9d",                3},    /* "ℝ" U+0211D */
    {"RoundImplies",                    12, "\xe2\xa5\xb0",                3},    /* "⥰" U+02970 */
    {"Rrightarrow",                     11, "\xe2\x87\x9b",                3},    /* "⇛" U+021DB */
    {"Rscr",                             4, "\xe2\x84\x9b",                3},    /* "ℛ" U+0211B */
    {"Rsh",                              3, "\xe2\x86\xb1",                3},    /* "↱" U+021B1 */
    {"RuleDelayed",                     11, "\xe2\xa7\xb4",                3},    /* "⧴" U+029F4 */
    {"SHCHcy",                           6, "\xd0\xa9",                    2},    /* "Щ" U+00429 */
    {"SHcy",                             4, "\xd0\xa8",                    2},    /* "Ш" U+00428 */
    {"SOFTcy",                           6, "\xd0\xac",                    2},    /* "Ь" U+0042C */
    {"Sacute",                           6, "\xc5\x9a",                    2},    /* "Ś" U+0015A */
    {"Sc",                               2, "\xe2\xaa\xbc",                3},    /* "⪼" U+02ABC */
    {"Scaron",                           6, "\xc5\xa0",                    2},    /* "Š" U+00160 */
    {"Scedil",                           6, "\xc5\x9e",                    2},    /* "Ş" U+0015E */
    {"Scirc",                            5, "\xc5\x9c",                    2},    /* "Ŝ" U+0015C */
    {"Scy",                              3, "\xd0\xa1",                    2},    /* "С" U+00421 */
    {"Sfr",                              3, "\xf0\x9d\x94\x96",            4},    /* "𝔖" U+1D516 */
    {"ShortDownArrow",                  14, "\xe2\x86\x93",                3},    /* "↓" U+02193 */
    {"ShortLeftArrow",                  14, "\xe2\x86\x90",                3},    /* "←" U+02190 */
    {"ShortRightArrow",                 15, "\xe2\x86\x92",                3},    /* "→" U+02192 */
    {"ShortUpArrow",                    12, "\xe2\x86\x91",                3},    /* "↑" U+02191 */
    {"Sigma",                            5, "\xce\xa3",                    2},    /* "Σ" U+003A3 */
    {"SmallCircle",                     11, "\xe2\x88\x98",                3},    /* "∘" U+02218 */
    {"Sopf",                             4, "\xf0\x9d\x95\x8a",            4},    /* "𝕊" U+1D54A */
    {"Sqrt",                             4, "\xe2\x88\x9a",                3},    /* "√" U+0221A */
    {"Square",                           6, "\xe2\x96\xa1",                3},    /* "□" U+025A1 */
    {"SquareIntersection",              18, "\xe2\x8a\x93",                3},    /* "⊓" U+02293 */
    {"SquareSubset",                    12, "\xe2\x8a\x8f",                3},    /* "⊏" U+0228F */
    {"SquareSubsetEqual",               17, "\xe2\x8a\x91",                3},    /* "⊑" U+02291 */
    {"SquareSuperset",                  14, "\xe2\x8a\x90",                3},    /* "⊐" U+02290 */
    {"SquareSupersetEqual",             19, "\xe2\x8a\x92",                3},    /* "⊒" U+02292 */
    {"SquareUnion",                     11, "\xe2\x8a\x94",                3},    /* "⊔" U+02294 */
    {"Sscr",                             4, "\xf0\x9d\x92\xae",            4},    /* "𝒮" U+1D4AE */
    {"Star",                             4, "\xe2\x8b\x86",                3},    /* "⋆" U+022C6 */
    {"Sub",                              3, "\xe2\x8b\x90",                3},    /* "⋐" U+022D0 */
    {"Subset",                           6, "\xe2\x8b\x90",                3},    /* "⋐" U+022D0 */
    {"SubsetEqual",                     11, "\xe2\x8a\x86",                3},    /* "⊆" U+02286 */
    {"Succeeds",                         8, "\xe2\x89\xbb",                3},    /* "≻" U+0227B */
    {"SucceedsEqual",                   13, "\xe2\xaa\xb0",                3},    /* "⪰" U+02AB0 */
    {"SucceedsSlantEqual",              18, "\xe2\x89\xbd",                3},    /* "≽" U+0227D */
    {"SucceedsTilde",                   13, "\xe2\x89\xbf",                3},    /* "≿" U+0227F */
    {"SuchThat",                         8, "\xe2\x88\x8b",                3},    /* "∋" U+0220B */
    {"Sum",                              3, "\xe2\x88\x91",                3},    /* "∑" U+02211 */
    {"Sup",                              3, "\xe2\x8b\x91",                3},    /* "⋑" U+022D1 */
    {"Superset",                         8, "\xe2\x8a\x83",                3},    /* "⊃" U+02283 */
    {"SupersetEqual",                   13, "\xe2\x8a\x87",                3},    /* "⊇" U+02287 */
    {"Supset",                           6, "\xe2\x8b\x91",                3},    /* "⋑" U+022D1 */
    {"THORN",                            5, "\xc3\x9e",                    2},    /* "Þ" U+000DE */
    {"TRADE",                            5, "\xe2\x84\xa2",                3},    /* "™" U+02122 */
    {"TSHcy",                            5, "\xd0\x8b",                    2},    /* "Ћ" U+0040B */
    {"TScy",                             4, "\xd0\xa6",                    2},    /* "Ц" U+00426 */
    {"Tab",                              3, "\x09",                        1},    /* Tab U+00009 */
    {"Tau",                              3, "\xce\xa4",                    2},    /* "Τ" U+003A4 */
    {"Tcaron",                           6, "\xc5\xa4",                    2},    /* "Ť" U+00164 */
    {"Tcedil",                           6, "\xc5\xa2",                    2},    /* "Ţ" U+00162 */
    {"Tcy",                              3, "\xd0\xa2",                    2},    /* "Т" U+00422 */
    {"Tfr",                              3, "\xf0\x9d\x94\x97",            4},    /* "𝔗" U+1D517 */
    {"Therefore",                        9, "\xe2\x88\xb4",                3},    /* "∴" U+02234 */
    {"Theta",                            5, "\xce\x98",                    2},    /* "Θ" U+00398 */
    {"ThickSpace",                      10, "\xe2\x81\x9f\xe2\x80\x8a",    6},    /* "  " U+0205F U+0200A */
    {"ThinSpace",                        9, "\xe2\x80\x89",                3},    /* " " U+02009 */
    {"Tilde",                            5, "\xe2\x88\xbc",                3},    /* "∼" U+0223C */
    {"TildeEqual",                      10, "\xe2\x89\x83",                3},    /* "≃" U+02243 */
    {"TildeFullEqual",                  14, "\xe2\x89\x85",                3},    /* "≅" U+02245 */
    {"TildeTilde",                      10, "\xe2\x89\x88",                3},    /* "≈" U+02248 */
    {"Topf",                             4, "\xf0\x9d\x95\x8b",            4},    /* "𝕋" U+1D54B */
    {"TripleDot",                        9, "\xe2\x83\x9b",                3},    /* "⃛" U+020DB */
    {"Tscr",                             4, "\xf0\x9d\x92\xaf",            4},    /* "𝒯" U+1D4AF */
    {"Tstrok",                           6, "\xc5\xa6",                    2},    /* "Ŧ" U+00166 */
    {"Uacute",                           6, "\xc3\x9a",                    2},    /* "Ú" U+000DA */
    {"Uarr",                             4, "\xe2\x86\x9f",                3},    /* "↟" U+0219F */
    {"Uarrocir",                         8, "\xe2\xa5\x89",                3},    /* "⥉" U+02949 */
    {"Ubrcy",                            5, "\xd0\x8e",                    2},    /* "Ў" U+0040E */
    {"Ubreve",                           6, "\xc5\xac",                    2},    /* "Ŭ" U+0016C */
    {"Ucirc",                            5, "\xc3\x9b",                    2},    /* "Û" U+000DB */
    {"Ucy",                              3, "\xd0\xa3",                    2},    /* "У" U+00423 */
    {"Udblac",                           6, "\xc5\xb0",                    2},    /* "Ű" U+00170 */
    {"Ufr",                              3, "\xf0\x9d\x94\x98",            4},    /* "𝔘" U+1D518 */
    {"Ugrave",                           6, "\xc3\x99",                    2},    /* "Ù" U+000D9 */
    {"Umacr",                            5, "\xc5\xaa",                    2},    /* "Ū" U+0016A */
    {"UnderBar",                         8, "\x5f",                        1},    /* "_" U+0005F */
    {"UnderBrace",                      10, "\xe2\x8f\x9f",                3},    /* "⏟" U+023DF */
    {"UnderBracket",                    12, "\xe2\x8e\xb5",                3},    /* "⎵" U+023B5 */
    {"UnderParenthesis",                16, "\xe2\x8f\x9d",                3},    /* "⏝" U+023DD */
    {"Union",                            5, "\xe2\x8b\x83",                3},    /* "⋃" U+022C3 */
    {"UnionPlus",                        9, "\xe2\x8a\x8e",                3},    /* "⊎" U+0228E */
    {"Uogon",                            5, "\xc5\xb2",                    2},    /* "Ų" U+00172 */
    {"Uopf",                             4, "\xf0\x9d\x95\x8c",            4},    /* "𝕌" U+1D54C */
    {"UpArrow",                          7, "\xe2\x86\x91",                3},    /* "↑" U+02191 */
    {"UpArrowBar",                      10, "\xe2\xa4\x92",                3},    /* "⤒" U+02912 */
    {"UpArrowDownArrow",                16, "\xe2\x87\x85",                3},    /* "⇅" U+021C5 */
    {"UpDownArrow",                     11, "\xe2\x86\x95",                3},    /* "↕" U+02195 */
    {"UpEquilibrium",                   13, "\xe2\xa5\xae",                3},    /* "⥮" U+0296E */
    {"UpTee",                            5, "\xe2\x8a\xa5",                3},    /* "⊥" U+022A5 */
    {"UpTeeArrow",                      10, "\xe2\x86\xa5",                3},    /* "↥" U+021A5 */
    {"Uparrow",                          7, "\xe2\x87\x91",                3},    /* "⇑" U+021D1 */
    {"Updownarrow",                     11, "\xe2\x87\x95",                3},    /* "⇕" U+021D5 */
    {"UpperLeftArrow",                  14, "\xe2\x86\x96",                3},    /* "↖" U+02196 */
    {"UpperRightArrow",                 15, "\xe2\x86\x97",                3},    /* "↗" U+02197 */
    {"Upsi",                             4, "\xcf\x92",                    2},    /* "ϒ" U+003D2 */
    {"Upsilon",                          7, "\xce\xa5",                    2},    /* "Υ" U+003A5 */
    {"Uring",                            5, "\xc5\xae",                    2},    /* "Ů" U+0016E */
    {"Uscr",                             4, "\xf0\x9d\x92\xb0",            4},    /* "𝒰" U+1D4B0 */
    {"Utilde",                           6, "\xc5\xa8",                    2},    /* "Ũ" U+00168 */
    {"Uuml",                             4, "\xc3\x9c",                    2},    /* "Ü" U+000DC */
    {"VDash",                            5, "\xe2\x8a\xab",                3},    /* "⊫" U+022AB */
    {"Vbar",                             4, "\xe2\xab\xab",                3},    /* "⫫" U+02AEB */
    {"Vcy",                              3, "\xd0\x92",                    2},    /* "В" U+00412 */
    {"Vdash",                            5, "\xe2\x8a\xa9",                3},    /* "⊩" U+022A9 */
    {"Vdashl",                           6, "\xe2\xab\xa6",                3},    /* "⫦" U+02AE6 */
    {"Vee",                              3, "\xe2\x8b\x81",                3},    /* "⋁" U+022C1 */
    {"Verbar",                           6, "\xe2\x80\x96",                3},    /* "‖" U+02016 */
    {"Vert",                             4, "\xe2\x80\x96",                3},    /* "‖" U+02016 */
    {"VerticalBar",                     11, "\xe2\x88\xa3",                3},    /* "∣" U+02223 */
    {"VerticalLine",                    12, "\x7c",                        1},    /* "|" U+0007C */
    {"VerticalSeparator",               17, "\xe2\x9d\x98",                3},    /* "❘" U+02758 */
    {"VerticalTilde",                   13, "\xe2\x89\x80",                3},    /* "≀" U+02240 */
    {"VeryThinSpace",                   13, "\xe2\x80\x8a",                3},    /* " " U+0200A */
    {"Vfr",                              3, "\xf0\x9d\x94\x99",            4},    /* "𝔙" U+1D519 */
    {"Vopf",                             4, "\xf0\x9d\x95\x8d",            4},    /* "𝕍" U+1D54D */
    {"Vscr",                             4, "\xf0\x9d\x92\xb1",            4},    /* "𝒱" U+1D4B1 */
    {"Vvdash",                           6, "\xe2\x8a\xaa",                3},    /* "⊪" U+022AA */
    {"Wcirc",                            5, "\xc5\xb4",                    2},    /* "Ŵ" U+00174 */
    {"Wedge",                            5, "\xe2\x8b\x80",                3},    /* "⋀" U+022C0 */
    {"Wfr",                              3, "\xf0\x9d\x94\x9a",            4},    /* "𝔚" U+1D51A */
    {"Wopf",                             4, "\xf0\x9d\x95\x8e",            4},    /* "𝕎" U+1D54E */
    {"Wscr",                             4, "\xf0\x9d\x92\xb2",            4},    /* "𝒲" U+1D4B2 */
    {"Xfr",                              3, "\xf0\x9d\x94\x9b",            4},    /* "𝔛" U+1D51B */
    {"Xi",                               2, "\xce\x9e",                    2},    /* "Ξ" U+0039E */
    {"Xopf",                             4, "\xf0\x9d\x95\x8f",            4},    /* "𝕏" U+1D54F */
    {"Xscr",                             4, "\xf0\x9d\x92\xb3",            4},    /* "𝒳" U+1D4B3 */
    {"YAcy",                             4, "\xd0\xaf",                    2},    /* "Я" U+0042F */
    {"YIcy",                             4, "\xd0\x87",                    2},    /* "Ї" U+00407 */
    {"YUcy",                             4, "\xd0\xae",                    2},    /* "Ю" U+0042E */
    {"Yacute",                           6, "\xc3\x9d",                    2},    /* "Ý" U+000DD */
    {"Ycirc",                            5, "\xc5\xb6",                    2},    /* "Ŷ" U+00176 */
    {"Ycy",                              3, "\xd0\xab",                    2},    /* "Ы" U+0042B */
    {"Yfr",                              3, "\xf0\x9d\x94\x9c",            4},    /* "𝔜" U+1D51C */
    {"Yopf",                             4, "\xf0\x9d\x95\x90",            4},    /* "𝕐" U+1D550 */
    {"Yscr",                             4, "\xf0\x9d\x92\xb4",            4},    /* "𝒴" U+1D4B4 */
    {"Yuml",                             4, "\xc5\xb8",                    2},    /* "Ÿ" U+00178 */
    {"ZHcy",                             4, "\xd0\x96",                    2},    /* "Ж" U+00416 */
    {"Zacute",                           6, "\xc5\xb9",                    2},    /* "Ź" U+00179 */
    {"Zcaron",                           6, "\xc5\xbd",                    2},    /* "Ž" U+0017D */
    {"Zcy",                              3, "\xd0\x97",                    2},    /* "З" U+00417 */
    {"Zdot",                             4, "\xc5\xbb",                    2},    /* "Ż" U+0017B */
    {"ZeroWidthSpace",                  14, "\xe2\x80\x8b",                3},    /* "​" U+0200B */
    {"Zeta",                             4, "\xce\x96",                    2},    /* "Ζ" U+00396 */
    {"Zfr",                              3, "\xe2\x84\xa8",                3},    /* "ℨ" U+02128 */
    {"Zopf",                             4, "\xe2\x84\xa4",                3},    /* "ℤ" U+02124 */
    {"Zscr",                             4, "\xf0\x9d\x92\xb5",            4},    /* "𝒵" U+1D4B5 */
    {"aacute",                           6, "\xc3\xa1",                    2},    /* "á" U+000E1 */
    {"abreve",                           6, "\xc4\x83",                    2},    /* "ă" U+00103 */
    {"ac",                               2, "\xe2\x88\xbe",                3},    /* "∾" U+0223E */
    {"acE",                              3, "\xe2\x88\xbe\xcc\xb3",        5},    /* "∾̳" U+0223E U+00333 */
    {"acd",                              3, "\xe2\x88\xbf",                3},    /* "∿" U+0223F */
    {"acirc",                            5, "\xc3\xa2",                    2},    /* "â" U+000E2 */
    {"acute",                            5, "\xc2\xb4",                    2},    /* "´" U+000B4 */
    {"acy",                              3, "\xd0\xb0",                    2},    /* "а" U+00430 */
    {"aelig",                            5, "\xc3\xa6",                    2},    /* "æ" U+000E6 */
    {"af",                               2, "\xe2\x81\xa1",                3},    /* "⁡" U+02061 */
    {"afr",                              3, "\xf0\x9d\x94\x9e",            4},    /* "𝔞" U+1D51E */
    {"agrave",                           6, "\xc3\xa0",                    2},    /* "à" U+000E0 */
    {"alefsym",                          7, "\xe2\x84\xb5",                3},    /* "ℵ" U+02135 */
    {"aleph",                            5, "\xe2\x84\xb5",                3},    /* "ℵ" U+02135 */
    {"alpha",                            5, "\xce\xb1",                    2},    /* "α" U+003B1 */
    {"amacr",                            5, "\xc4\x81",                    2},    /* "ā" U+00101 */
    {"amalg",                            5, "\xe2\xa8\xbf",                3},    /* "⨿" U+02A3F */
    {"amp",                              3, "\x26",                        1},    /* "&" U+00026 */
    {"and",                              3, "\xe2\x88\xa7",                3},    /* "∧" U+02227 */
    {"andand",                           6, "\xe2\xa9\x95",                3},    /* "⩕" U+02A55 */
    {"andd",                             4, "\xe2\xa9\x9c",                3},    /* "⩜" U+02A5C */
    {"andslope",                         8, "\xe2\xa9\x98",                3},    /* "⩘" U+02A58 */
    {"andv",                             4, "\xe2\xa9\x9a",                3},    /* "⩚" U+02A5A */
    {"ang",                              3, "\xe2\x88\xa0",                3},    /* "∠" U+02220 */
    {"ange",                             4, "\xe2\xa6\xa4",                3},    /* "⦤" U+029A4 */
    {"angle",                            5, "\xe2\x88\xa0",                3},    /* "∠" U+02220 */
    {"angmsd",                           6, "\xe2\x88\xa1",                3},    /* "∡" U+02221 */
    {"angmsdaa",                         8, "\xe2\xa6\xa8",                3},    /* "⦨" U+029A8 */
    {"angmsdab",                         8, "\xe2\xa6\xa9",                3},    /* "⦩" U+029A9 */
    {"angmsdac",                         8, "\xe2\xa6\xaa",                3},    /* "⦪" U+029AA */
    {"angmsdad",                         8, "\xe2\xa6\xab",                3},    /* "⦫" U+029AB */
    {"angmsdae",                         8, "\xe2\xa6\xac",                3},    /* "⦬" U+029AC */
    {"angmsdaf",                         8, "\xe2\xa6\xad",                3},    /* "⦭" U+029AD */
    {"angmsdag",                         8, "\xe2\xa6\xae",                3},    /* "⦮" U+029AE */
    {"angmsdah",                         8, "\xe2\xa6\xaf",                3},    /* "⦯" U+029AF */
    {"angrt",                            5, "\xe2\x88\x9f",                3},    /* "∟" U+0221F */
    {"angrtvb",                          7, "\xe2\x8a\xbe",                3},    /* "⊾" U+022BE */
    {"angrtvbd",                         8, "\xe2\xa6\x9d",                3},    /* "⦝" U+0299D */
    {"angsph",                           6, "\xe2\x88\xa2",                3},    /* "∢" U+02222 */
    {"angst",                            5, "\xc3\x85",                    2},    /* "Å" U+000C5 */
    {"angzarr",                          7, "\xe2\x8d\xbc",                3},    /* "⍼" U+0237C */
    {"aogon",                            5, "\xc4\x85",                    2},    /* "ą" U+00105 */
    {"aopf",                             4, "\xf0\x9d\x95\x92",            4},    /* "𝕒" U+1D552 */
    {"ap",                               2, "\xe2\x89\x88",                3},    /* "≈" U+02248 */
    {"apE",                              3, "\xe2\xa9\xb0",                3},    /* "⩰" U+02A70 */
    {"apacir",                           6, "\xe2\xa9\xaf",                3},    /* "⩯" U+02A6F */
    {"ape",                              3, "\xe2\x89\x8a",                3},    /* "≊" U+0224A */
    {"apid",                             4, "\xe2\x89\x8b",                3},    /* "≋" U+0224B */
    {"apos",                             4, "\x27",                        1},    /* "'" U+00027 */
    {"approx",                           6, "\xe2\x89\x88",                3},    /* "≈" U+02248 */
    {"approxeq",                         8, "\xe2\x89\x8a",                3},    /* "≊" U+0224A */
    {"aring",                            5, "\xc3\xa5",                    2},    /* "å" U+000E5 */
    {"ascr",                             4, "\xf0\x9d\x92\xb6",            4},    /* "𝒶" U+1D4B6 */
    {"ast",                              3, "\x2a",                        1},    /* "*" U+0002A */
    {"asymp",                            5, "\xe2\x89\x88",                3},    /* "≈" U+02248 */
    {"asympeq",                          7, "\xe2\x89\x8d",                3},    /* "≍" U+0224D */
    {"atilde",                           6, "\xc3\xa3",                    2},    /* "ã" U+000E3 */
    {"auml",                             4, "\xc3\xa4",                    2},    /* "ä" U+000E4 */
    {"awconint",                         8, "\xe2\x88\xb3",                3},    /* "∳" U+02233 */
    {"awint",                            5, "\xe2\xa8\x91",                3},    /* "⨑" U+02A11 */
    {"bNot",                             4, "\xe2\xab\xad",                3},    /* "⫭" U+02AED */
    {"backcong",                         8, "\xe2\x89\x8c",                3},    /* "≌" U+0224C */
    {"backepsilon",                     11, "\xcf\xb6",                    2},    /* "϶" U+003F6 */
    {"backprime",                        9, "\xe2\x80\xb5",                3},    /* "‵" U+02035 */
    {"backsim",                          7, "\xe2\x88\xbd",                3},    /* "∽" U+0223D */
    {"backsimeq",                        9, "\xe2\x8b\x8d",                3},    /* "⋍" U+022CD */
    {"barvee",                           6, "\xe2\x8a\xbd",                3},    /* "⊽" U+022BD */
    {"barwed",                           6, "\xe2\x8c\x85",                3},    /* "⌅" U+02305 */
    {"barwedge",                         8, "\xe2\x8c\x85",                3},    /* "⌅" U+02305 */
    {"bbrk",                             4, "\xe2\x8e\xb5",                3},    /* "⎵" U+023B5 */
    {"bbrktbrk",                         8, "\xe2\x8e\xb6",                3},    /* "⎶" U+023B6 */
    {"bcong",                            5, "\xe2\x89\x8c",                3},    /* "≌" U+0224C */
    {"bcy",                              3, "\xd0\xb1",                    2},    /* "б" U+00431 */
    {"bdquo",                            5, "\xe2\x80\x9e",                3},    /* "„" U+0201E */
    {"becaus",                           6, "\xe2\x88\xb5",                3},    /* "∵" U+02235 */
    {"because",                          7, "\xe2\x88\xb5",                3},    /* "∵" U+02235 */
    {"bemptyv",                          7, "\xe2\xa6\xb0",                3},    /* "⦰" U+029B0 */
    {"bepsi",                            5, "\xcf\xb6",                    2},    /* "϶" U+003F6 */
    {"bernou",                           6, "\xe2\x84\xac",                3},    /* "ℬ" U+0212C */
    {"beta",                             4, "\xce\xb2",                    2},    /* "β" U+003B2 */
    {"beth",                             4, "\xe2\x84\xb6",                3},    /* "ℶ" U+02136 */
    {"between",                          7, "\xe2\x89\xac",                3},    /* "≬" U+0226C */
    {"bfr",                              3, "\xf0\x9d\x94\x9f",            4},    /* "𝔟" U+1D51F */
    {"bigcap",                           6, "\xe2\x8b\x82",                3},    /* "⋂" U+022C2 */
    {"bigcirc",                          7, "\xe2\x97\xaf",                3},    /* "◯" U+025EF */
    {"bigcup",                           6, "\xe2\x8b\x83",                3},    /* "⋃" U+022C3 */
    {"bigodot",                          7, "\xe2\xa8\x80",                3},    /* "⨀" U+02A00 */
    {"bigoplus",                         8, "\xe2\xa8\x81",                3},    /* "⨁" U+02A01 */
    {"bigotimes",                        9, "\xe2\xa8\x82",                3},    /* "⨂" U+02A02 */
    {"bigsqcup",                         8, "\xe2\xa8\x86",                3},    /* "⨆" U+02A06 */
    {"bigstar",                          7, "\xe2\x98\x85",                3},    /* "★" U+02605 */
    {"bigtriangledown",                 15, "\xe2\x96\xbd",                3},    /* "▽" U+025BD */
    {"bigtriangleup",                   13, "\xe2\x96\xb3",                3},    /* "△" U+025B3 */
    {"biguplus",                         8, "\xe2\xa8\x84",                3},    /* "⨄" U+02A04 */
    {"bigvee",                           6, "\xe2\x8b\x81",                3},    /* "⋁" U+022C1 */
    {"bigwedge",                         8, "\xe2\x8b\x80",                3},    /* "⋀" U+022C0 */
    {"bkarow",                           6, "\xe2\xa4\x8d",                3},    /* "⤍" U+0290D */
    {"blacklozenge",                    12, "\xe2\xa7\xab",                3},    /* "⧫" U+029EB */
    {"blacksquare",                     11, "\xe2\x96\xaa",                3},    /* "▪" U+025AA */
    {"blacktriangle",                   13, "\xe2\x96\xb4",                3},    /* "▴" U+025B4 */
    {"blacktriangledown",               17, "\xe2\x96\xbe",                3},    /* "▾" U+025BE */
    {"blacktriangleleft",               17, "\xe2\x97\x82",                3},    /* "◂" U+025C2 */
    {"blacktriangleright",              18, "\xe2\x96\xb8",                3},    /* "▸" U+025B8 */
    {"blank",                            5, "\xe2\x90\xa3",                3},    /* "␣" U+02423 */
    {"blk12",                            5, "\xe2\x96\x92",                3},    /* "▒" U+02592 */
    {"blk14",                            5, "\xe2\x96\x91",                3},    /* "░" U+02591 */
    {"blk34",                            5, "\xe2\x96\x93",                3},    /* "▓" U+02593 */
    {"block",                            5, "\xe2\x96\x88",                3},    /* "█" U+02588 */
    {"bne",                              3, "\x3d\xe2\x83\xa5",            4},    /* "=⃥" U+0003D U+020E5 */
    {"bnequiv",                          7, "\xe2\x89\xa1\xe2\x83\xa5",    6},    /* "≡⃥" U+02261 U+020E5 */
    {"bnot",                             4, "\xe2\x8c\x90",                3},    /* "⌐" U+02310 */
    {"bopf",                             4, "\xf0\x9d\x95\x93",            4},    /* "𝕓" U+1D553 */
    {"bot",                              3, "\xe2\x8a\xa5",                3},    /* "⊥" U+022A5 */
    {"bottom",                           6, "\xe2\x8a\xa5",                3},    /* "⊥" U+022A5 */
    {"bowtie",                           6, "\xe2\x8b\x88",                3},    /* "⋈" U+022C8 */
    {"boxDL",                            5, "\xe2\x95\x97",                3},    /* "╗" U+02557 */
    {"boxDR",                            5, "\xe2\x95\x94",                3},    /* "╔" U+02554 */
    {"boxDl",                            5, "\xe2\x95\x96",                3},    /* "╖" U+02556 */
    {"boxDr",                            5, "\xe2\x95\x93",                3},    /* "╓" U+02553 */
    {"boxH",                             4, "\xe2\x95\x90",                3},    /* "═" U+02550 */
    {"boxHD",                            5, "\xe2\x95\xa6",                3},    /* "╦" U+02566 */
    {"boxHU",                            5, "\xe2\x95\xa9",                3},    /* "╩" U+02569 */
    {"boxHd",                            5, "\xe2\x95\xa4",                3},    /* "╤" U+02564 */
    {"boxHu",                            5, "\xe2\x95\xa7",                3},    /* "╧" U+02567 */
    {"boxUL",                            5, "\xe2\x95\x9d",                3},    /* "╝" U+0255D */
    {"boxUR",                            5, "\xe2\x95\x9a",                3},    /* "╚" U+0255A */
    {"boxUl",                            5, "\xe2\x95\x9c",                3},    /* "╜" U+0255C */
    {"boxUr",                            5, "\xe2\x95\x99",                3},    /* "╙" U+02559 */
    {"boxV",                             4, "\xe2\x95\x91",                3},    /* "║" U+02551 */
    {"boxVH",                            5, "\xe2\x95\xac",                3},    /* "╬" U+0256C */
    {"boxVL",                            5, "\xe2\x95\xa3",                3},    /* "╣" U+02563 */
    {"boxVR",                            5, "\xe2\x95\xa0",                3},    /* "╠" U+02560 */
    {"boxVh",                            5, "\xe2\x95\xab",                3},    /* "╫" U+0256B */
    {"boxVl",                            5, "\xe2\x95\xa2",                3},    /* "╢" U+02562 */
    {"boxVr",                            5, "\xe2\x95\x9f",                3},    /* "╟" U+0255F */
    {"boxbox",                           6, "\xe2\xa7\x89",                3},    /* "⧉" U+029C9 */
    {"boxdL",                            5, "\xe2\x95\x95",                3},    /* "╕" U+02555 */
    {"boxdR",                            5, "\xe2\x95\x92",                3},    /* "╒" U+02552 */
    {"boxdl",                            5, "\xe2\x94\x90",                3},    /* "┐" U+02510 */
    {"boxdr",                            5, "\xe2\x94\x8c",                3},    /* "┌" U+0250C */
    {"boxh",                             4, "\xe2\x94\x80",                3},    /* "─" U+02500 */
    {"boxhD",                            5, "\xe2\x95\xa5",                3},    /* "╥" U+02565 */
    {"boxhU",                            5, "\xe2\x95\xa8",                3},    /* "╨" U+02568 */
    {"boxhd",                            5, "\xe2\x94\xac",                3},    /* "┬" U+0252C */
    {"boxhu",                            5, "\xe2\x94\xb4",                3},    /* "┴" U+02534 */
    {"boxminus",                         8, "\xe2\x8a\x9f",                3},    /* "⊟" U+0229F */
    {"boxplus",                          7, "\xe2\x8a\x9e",                3},    /* "⊞" U+0229E */
    {"boxtimes",                         8, "\xe2\x8a\xa0",                3},    /* "⊠" U+022A0 */
    {"boxuL",                            5, "\xe2\x95\x9b",                3},    /* "╛" U+0255B */
    {"boxuR",                            5, "\xe2\x95\x98",                3},    /* "╘" U+02558 */
    {"boxul",                            5, "\xe2\x94\x98",                3},    /* "┘" U+02518 */
    {"boxur",                            5, "\xe2\x94\x94",                3},    /* "└" U+02514 */
    {"boxv",                             4, "\xe2\x94\x82",                3},    /* "│" U+02502 */
    {"boxvH",                            5, "\xe2\x95\xaa",                3},    /* "╪" U+0256A */
    {"boxvL",                            5, "\xe2\x95\xa1",                3},    /* "╡" U+02561 */
    {"boxvR",                            5, "\xe2\x95\x9e",                3},    /* "╞" U+0255E */
    {"boxvh",                            5, "\xe2\x94\xbc",                3},    /* "┼" U+0253C */
    {"boxvl",                            5, "\xe2\x94\xa4",                3},    /* "┤" U+02524 */
    {"boxvr",                            5, "\xe2\x94\x9c",                3},    /* "├" U+0251C */
    {"bprime",                           6, "\xe2\x80\xb5",                3},    /* "‵" U+02035 */
    {"breve",                            5, "\xcb\x98",                    2},    /* "˘" U+002D8 */
    {"brvbar",                           6, "\xc2\xa6",                    2},    /* "¦" U+000A6 */
    {"bscr",                             4, "\xf0\x9d\x92\xb7",            4},    /* "𝒷" U+1D4B7 */
    {"bsemi",                            5, "\xe2\x81\x8f",                3},    /* "⁏" U+0204F */
    {"bsim",                             4, "\xe2\x88\xbd",                3},    /* "∽" U+0223D */
    {"bsime",                            5, "\xe2\x8b\x8d",                3},    /* "⋍" U+022CD */
    {"bsol",                             4, "\x5c",                        1},    /* "\" U+0005C */
    {"bsolb",                            5, "\xe2\xa7\x85",                3},    /* "⧅" U+029C5 */
    {"bsolhsub",                         8, "\xe2\x9f\x88",                3},    /* "⟈" U+027C8 */
    {"bull",                             4, "\xe2\x80\xa2",                3},    /* "•" U+02022 */
    {"bullet",                           6, "\xe2\x80\xa2",                3},    /* "•" U+02022 */
    {"bump",                             4, "\xe2\x89\x8e",                3},    /* "≎" U+0224E */
    {"bumpE",                            5, "\xe2\xaa\xae",                3},    /* "⪮" U+02AAE */
    {"bumpe",                            5, "\xe2\x89\x8f",                3},    /* "≏" U+0224F */
    {"bumpeq",                           6, "\xe2\x89\x8f",                3},    /* "≏" U+0224F */
    {"cacute",                           6, "\xc4\x87",                    2},    /* "ć" U+00107 */
    {"cap",                              3, "\xe2\x88\xa9",                3},    /* "∩" U+02229 */
    {"capand",                           6, "\xe2\xa9\x84",                3},    /* "⩄" U+02A44 */
    {"capbrcup",                         8, "\xe2\xa9\x89",                3},    /* "⩉" U+02A49 */
    {"capcap",                           6, "\xe2\xa9\x8b",                3},    /* "⩋" U+02A4B */
    {"capcup",                           6, "\xe2\xa9\x87",                3},    /* "⩇" U+02A47 */
    {"capdot",                           6, "\xe2\xa9\x80",                3},    /* "⩀" U+02A40 */
    {"caps",                             4, "\xe2\x88\xa9\xef\xb8\x80",    6},    /* "∩︀" U+02229 U+0FE00 */
    {"caret",                            5, "\xe2\x81\x81",                3},    /* "⁁" U+02041 */
    {"caron",                            5, "\xcb\x87",                    2},    /* "ˇ" U+002C7 */
    {"ccaps",                            5, "\xe2\xa9\x8d",                3},    /* "⩍" U+02A4D */
    {"ccaron",                           6, "\xc4\x8d",                    2},    /* "č" U+0010D */
    {"ccedil",                           6, "\xc3\xa7",                    2},    /* "ç" U+000E7 */
    {"ccirc",                            5, "\xc4\x89",                    2},    /* "ĉ" U+00109 */
    {"ccups",                            5, "\xe2\xa9\x8c",                3},    /* "⩌" U+02A4C */
    {"ccupssm",                          7, "\xe2\xa9\x90",                3},    /* "⩐" U+02A50 */
    {"cdot",                             4, "\xc4\x8b",                    2},    /* "ċ" U+0010B */
    {"cedil",                            5, "\xc2\xb8",                    2},    /* "¸" U+000B8 */
    {"cemptyv",                          7, "\xe2\xa6\xb2",                3},    /* "⦲" U+029B2 */
    {"cent",                             4, "\xc2\xa2",                    2},    /* "¢" U+000A2 */
    {"centerdot",                        9, "\xc2\xb7",                    2},    /* "·" U+000B7 */
    {"cfr",                              3, "\xf0\x9d\x94\xa0",            4},    /* "𝔠" U+1D520 */
    {"chcy",                             4, "\xd1\x87",                    2},    /* "ч" U+00447 */
    {"check",                            5, "\xe2\x9c\x93",                3},    /* "✓" U+02713 */
    {"checkmark",                        9, "\xe2\x9c\x93",                3},    /* "✓" U+02713 */
    {"chi",                              3, "\xcf\x87",                    2},    /* "χ" U+003C7 */
    {"cir",                              3, "\xe2\x97\x8b",                3},    /* "○" U+025CB */
    {"cirE",                             4, "\xe2\xa7\x83",                3},    /* "⧃" U+029C3 */
    {"circ",                             4, "\xcb\x86",                    2},    /* "ˆ" U+002C6 */
    {"circeq",                           6, "\xe2\x89\x97",                3},    /* "≗" U+02257 */
    {"circlearrowleft",                 15, "\xe2\x86\xba",                3},    /* "↺" U+021BA */
    {"circlearrowright",                16, "\xe2\x86\xbb",                3},    /* "↻" U+021BB */
    {"circledR",                         8, "\xc2\xae",                    2},    /* "®" U+000AE */
    {"circledS",                         8, "\xe2\x93\x88",                3},    /* "Ⓢ" U+024C8 */
    {"circledast",                      10, "\xe2\x8a\x9b",                3},    /* "⊛" U+0229B */
    {"circledcirc",                     11, "\xe2\x8a\x9a",                3},    /* "⊚" U+0229A */
    {"circleddash",                     11, "\xe2\x8a\x9d",                3},    /* "⊝" U+0229D */
    {"cire",                             4, "\xe2\x89\x97",                3},    /* "≗" U+02257 */
    {"cirfnint",                         8, "\xe2\xa8\x90",                3},    /* "⨐" U+02A10 */
    {"cirmid",                           6, "\xe2\xab\xaf",                3},    /* "⫯" U+02AEF */
    {"cirscir",                          7, "\xe2\xa7\x82",                3},    /* "⧂" U+029C2 */
    {"clubs",                            5, "\xe2\x99\xa3",                3},    /* "♣" U+02663 */
    {"clubsuit",                         8, "\xe2\x99\xa3",                3},    /* "♣" U+02663 */
    {"colon",                            5, "\x3a",                        1},    /* ":" U+0003A */
    {"colone",                           6, "\xe2\x89\x94",                3},    /* "≔" U+02254 */
    {"coloneq",                          7, "\xe2\x89\x94",                3},    /* "≔" U+02254 */
    {"comma",                            5, "\x2c",                        1},    /* "," U+0002C */
    {"commat",                           6, "\x40",                        1},    /* "@" U+00040 */
    {"comp",                             4, "\xe2\x88\x81",                3},    /* "∁" U+02201 */
    {"compfn",                           6, "\xe2\x88\x98",                3},    /* "∘" U+02218 */
    {"complement",                      10, "\xe2\x88\x81",                3},    /* "∁" U+02201 */
    {"complexes",                        9, "\xe2\x84\x82",                3},    /* "ℂ" U+02102 */
    {"cong",                             4, "\xe2\x89\x85",                3},    /* "≅" U+02245 */
    {"congdot",                          7, "\xe2\xa9\xad",                3},    /* "⩭" U+02A6D */
    {"conint",                           6, "\xe2\x88\xae",                3},    /* "∮" U+0222E */
    {"copf",                             4, "\xf0\x9d\x95\x94",            4},    /* "𝕔" U+1D554 */
    {"coprod",                           6, "\xe2\x88\x90",                3},    /* "∐" U+02210 */
    {"copy",                             4, "\xc2\xa9",                    2},    /* "©" U+000A9 */
    {"copysr",                           6, "\xe2\x84\x97",                3},    /* "℗" U+02117 */
    {"crarr",                            5, "\xe2\x86\xb5",                3},    /* "↵" U+021B5 */
    {"cross",                            5, "\xe2\x9c\x97",                3},    /* "✗" U+02717 */
    {"cscr",                             4, "\xf0\x9d\x92\xb8",            4},    /* "𝒸" U+1D4B8 */
    {"csub",                             4, "\xe2\xab\x8f",                3},    /* "⫏" U+02ACF */
    {"csube",                            5, "\xe2\xab\x91",                3},    /* "⫑" U+02AD1 */
    {"csup",                             4, "\xe2\xab\x90",                3},    /* "⫐" U+02AD0 */
    {"csupe",                            5, "\xe2\xab\x92",                3},    /* "⫒" U+02AD2 */
    {"ctdot",                            5, "\xe2\x8b\xaf",                3},    /* "⋯" U+022EF */
    {"cudarrl",                          7, "\xe2\xa4\xb8",                3},    /* "⤸" U+02938 */
    {"cudarrr",                          7, "\xe2\xa4\xb5",                3},    /* "⤵" U+02935 */
    {"cuepr",                            5, "\xe2\x8b\x9e",                3},    /* "⋞" U+022DE */
    {"cuesc",                            5, "\xe2\x8b\x9f",                3},    /* "⋟" U+022DF */
    {"cularr",                           6, "\xe2\x86\xb6",                3},    /* "↶" U+021B6 */
    {"cularrp",                          7, "\xe2\xa4\xbd",                3},    /* "⤽" U+0293D */
    {"cup",                              3, "\xe2\x88\xaa",                3},    /* "∪" U+0222A */
    {"cupbrcap",                         8, "\xe2\xa9\x88",                3},    /* "⩈" U+02A48 */
    {"cupcap",                           6, "\xe2\xa9\x86",                3},    /* "⩆" U+02A46 */
    {"cupcup",                           6, "\xe2\xa9\x8a",                3},    /* "⩊" U+02A4A */
    {"cupdot",                           6, "\xe2\x8a\x8d",                3},    /* "⊍" U+0228D */
    {"cupor",                            5, "\xe2\xa9\x85",                3},    /* "⩅" U+02A45 */
    {"cups",                             4, "\xe2\x88\xaa\xef\xb8\x80",    6},    /* "∪︀" U+0222A U+0FE00 */
    {"curarr",                           6, "\xe2\x86\xb7",                3},    /* "↷" U+021B7 */
    {"curarrm",                          7, "\xe2\xa4\xbc",                3},    /* "⤼" U+0293C */
    {"curlyeqprec",                     11, "\xe2\x8b\x9e",                3},    /* "⋞" U+022DE */
    {"curlyeqsucc",                     11, "\xe2\x8b\x9f",                3},    /* "⋟" U+022DF */
    {"curlyvee",                         8, "\xe2\x8b\x8e",                3},    /* "⋎" U+022CE */
    {"curlywedge",                      10, "\xe2\x8b\x8f",                3},    /* "⋏" U+022CF */
    {"curren",                           6, "\xc2\xa4",                    2},    /* "¤" U+000A4 */
    {"curvearrowleft",                  14, "\xe2\x86\xb6",                3},    /* "↶" U+021B6 */
    {"curvearrowright",                 15, "\xe2\x86\xb7",                3},    /* "↷" U+021B7 */
    {"cuvee",                            5, "\xe2\x8b\x8e",                3},    /* "⋎" U+022CE */
    {"cuwed",                            5, "\xe2\x8b\x8f",                3},    /* "⋏" U+022CF */
    {"cwconint",                         8, "\xe2\x88\xb2",                3},    /* "∲" U+02232 */
    {"cwint",                            5, "\xe2\x88\xb1",                3},    /* "∱" U+02231 */
    {"cylcty",                           6, "\xe2\x8c\xad",                3},    /* "⌭" U+0232D */
    {"dArr",                             4, "\xe2\x87\x93",                3},    /* "⇓" U+021D3 */
    {"dHar",                             4, "\xe2\xa5\xa5",                3},    /* "⥥" U+02965 */
    {"dagger",                           6, "\xe2\x80\xa0",                3},    /* "†" U+02020 */
    {"daleth",                           6, "\xe2\x84\xb8",                3},    /* "ℸ" U+02138 */
    {"darr",                             4, "\xe2\x86\x93",                3},    /* "↓" U+02193 */
    {"dash",                             4, "\xe2\x80\x90",                3},    /* "‐" U+02010 */
    {"dashv",                            5, "\xe2\x8a\xa3",                3},    /* "⊣" U+022A3 */
    {"dbkarow",                          7, "\xe2\xa4\x8f",                3},    /* "⤏" U+0290F */
    {"dblac",                            5, "\xcb\x9d",                    2},    /* "˝" U+002DD */
    {"dcaron",                           6, "\xc4\x8f",                    2},    /* "ď" U+0010F */
    {"dcy",                              3, "\xd0\xb4",                    2},    /* "д" U+00434 */
    {"dd",                               2, "\xe2\x85\x86",                3},    /* "ⅆ" U+02146 */
    {"ddagger",                          7, "\xe2\x80\xa1",                3},    /* "‡" U+02021 */
    {"ddarr",                            5, "\xe2\x87\x8a",                3},    /* "⇊" U+021CA */
    {"ddotseq",                          7, "\xe2\xa9\xb7",                3},    /* "⩷" U+02A77 */
    {"deg",                              3, "\xc2\xb0",                    2},    /* "°" U+000B0 */
    {"delta",                            5, "\xce\xb4",                    2},    /* "δ" U+003B4 */
    {"demptyv",                          7, "\xe2\xa6\xb1",                3},    /* "⦱" U+029B1 */
    {"dfisht",                           6, "\xe2\xa5\xbf",                3},    /* "⥿" U+0297F */
    {"dfr",                              3, "\xf0\x9d\x94\xa1",            4},    /* "𝔡" U+1D521 */
    {"dharl",                            5, "\xe2\x87\x83",                3},    /* "⇃" U+021C3 */
    {"dharr",                            5, "\xe2\x87\x82",                3},    /* "⇂" U+021C2 */
    {"diam",                             4, "\xe2\x8b\x84",                3},    /* "⋄" U+022C4 */
    {"diamond",                          7, "\xe2\x8b\x84",                3},    /* "⋄" U+022C4 */
    {"diamondsuit",                     11, "\xe2\x99\xa6",                3},    /* "♦" U+02666 */
    {"diams",                            5, "\xe2\x99\xa6",                3},    /* "♦" U+02666 */
    {"die",                              3, "\xc2\xa8",                    2},    /* "¨" U+000A8 */
    {"digamma",                          7, "\xcf\x9d",                    2},    /* "ϝ" U+003DD */
    {"disin",                            5, "\xe2\x8b\xb2",                3},    /* "⋲" U+022F2 */
    {"div",                              3, "\xc3\xb7",                    2},    /* "÷" U+000F7 */
    {"divide",                           6, "\xc3\xb7",                    2},    /* "÷" U+000F7 */
    {"divideontimes",                   13, "\xe2\x8b\x87",                3},    /* "⋇" U+022C7 */
    {"divonx",                           6, "\xe2\x8b\x87",                3},    /* "⋇" U+022C7 */
    {"djcy",                             4, "\xd1\x92",                    2},    /* "ђ" U+00452 */
    {"dlcorn",                           6, "\xe2\x8c\x9e",                3},    /* "⌞" U+0231E */
    {"dlcrop",                           6, "\xe2\x8c\x8d",                3},    /* "⌍" U+0230D */
    {"dollar",                           6, "\x24",                        1},    /* "$" U+00024 */
    {"dopf",                             4, "\xf0\x9d\x95\x95",            4},    /* "𝕕" U+1D555 */
    {"dot",                              3, "\xcb\x99",                    2},    /* "˙" U+002D9 */
    {"doteq",                            5, "\xe2\x89\x90",                3},    /* "≐" U+02250 */
    {"doteqdot",                         8, "\xe2\x89\x91",                3},    /* "≑" U+02251 */
    {"dotminus",                         8, "\xe2\x88\xb8",                3},    /* "∸" U+02238 */
    {"dotplus",                          7, "\xe2\x88\x94",                3},    /* "∔" U+02214 */
    {"dotsquare",                        9, "\xe2\x8a\xa1",                3},    /* "⊡" U+022A1 */
    {"doublebarwedge",                  14, "\xe2\x8c\x86",                3},    /* "⌆" U+02306 */
    {"downarrow",                        9, "\xe2\x86\x93",                3},    /* "↓" U+02193 */
    {"downdownarrows",                  14, "\xe2\x87\x8a",                3},    /* "⇊" U+021CA */
    {"downharpoonleft",                 15, "\xe2\x87\x83",                3},    /* "⇃" U+021C3 */
    {"downharpoonright",                16, "\xe2\x87\x82",                3},    /* "⇂" U+021C2 */
    {"drbkarow",                         8, "\xe2\xa4\x90",                3},    /* "⤐" U+02910 */
    {"drcorn",                           6, "\xe2\x8c\x9f",                3},    /* "⌟" U+0231F */
    {"drcrop",                           6, "\xe2\x8c\x8c",                3},    /* "⌌" U+0230C */
    {"dscr",                             4, "\xf0\x9d\x92\xb9",            4},    /* "𝒹" U+1D4B9 */
    {"dscy",                             4, "\xd1\x95",                    2},    /* "ѕ" U+00455 */
    {"dsol",                             4, "\xe2\xa7\xb6",                3},    /* "⧶" U+029F6 */
    {"dstrok",                           6, "\xc4\x91",                    2},    /* "đ" U+00111 */
    {"dtdot",                            5, "\xe2\x8b\xb1",                3},    /* "⋱" U+022F1 */
    {"dtri",                             4, "\xe2\x96\xbf",                3},    /* "▿" U+025BF */
    {"dtrif",                            5, "\xe2\x96\xbe",                3},    /* "▾" U+025BE */
    {"duarr",                            5, "\xe2\x87\xb5",                3},    /* "⇵" U+021F5 */
    {"duhar",                            5, "\xe2\xa5\xaf",                3},    /* "⥯" U+0296F */
    {"dwangle",                          7, "\xe2\xa6\xa6",                3},    /* "⦦" U+029A6 */
    {"dzcy",                             4, "\xd1\x9f",                    2},    /* "џ" U+0045F */
    {"dzigrarr",                         8, "\xe2\x9f\xbf",                3},    /* "⟿" U+027FF */
    {"eDDot",                            5, "\xe2\xa9\xb7",                3},    /* "⩷" U+02A77 */
    {"eDot",                             4, "\xe2\x89\x91",                3},    /* "≑" U+02251 */
    {"eacute",                           6, "\xc3\xa9",                    2},    /* "é" U+000E9 */
    {"easter",                           6, "\xe2\xa9\xae",                3},    /* "⩮" U+02A6E */
    {"ecaron",                           6, "\xc4\x9b",                    2},    /* "ě" U+0011B */
    {"ecir",                             4, "\xe2\x89\x96",                3},    /* "≖" U+02256 */
    {"ecirc",                            5, "\xc3\xaa",                    2},    /* "ê" U+000EA */
    {"ecolon",                           6, "\xe2\x89\x95",                3},    /* "≕" U+02255 */
    {"ecy",                              3, "\xd1\x8d",                    2},    /* "э" U+0044D */
    {"edot",                             4, "\xc4\x97",                    2},    /* "ė" U+00117 */
    {"ee",                               2, "\xe2\x85\x87",                3},    /* "ⅇ" U+02147 */
    {"efDot",                            5, "\xe2\x89\x92",                3},    /* "≒" U+02252 */
    {"efr",                              3, "\xf0\x9d\x94\xa2",            4},    /* "𝔢" U+1D522 */
    {"eg",                               2, "\xe2\xaa\x9a",                3},    /* "⪚" U+02A9A */
    {"egrave",                           6, "\xc3\xa8",                    2},    /* "è" U+000E8 */
    {"egs",                              3, "\xe2\xaa\x96",                3},    /* "⪖" U+02A96 */
    {"egsdot",                           6, "\xe2\xaa\x98",                3},    /* "⪘" U+02A98 */
    {"el",                               2, "\xe2\xaa\x99",                3},    /* "⪙" U+02A99 */
    {"elinters",                         8, "\xe2\x8f\xa7",                3},    /* "⏧" U+023E7 */
    {"ell",                              3, "\xe2\x84\x93",                3},    /* "ℓ" U+02113 */
    {"els",                              3, "\xe2\xaa\x95",                3},    /* "⪕" U+02A95 */
    {"elsdot",                           6, "\xe2\xaa\x97",                3},    /* "⪗" U+02A97 */
    {"emacr",                            5, "\xc4\x93",                    2},    /* "ē" U+00113 */
    {"empty",                            5, "\xe2\x88\x85",                3},    /* "∅" U+02205 */
    {"emptyset",                         8, "\xe2\x88\x85",                3},    /* "∅" U+02205 */
    {"emptyv",                           6, "\xe2\x88\x85",                3},    /* "∅" U+02205 */
    {"emsp",                             4, "\xe2\x80\x83",                3},    /* " " U+02003 */
    {"emsp13",                           6, "\xe2\x80\x84",                3},    /* " " U+02004 */
    {"emsp14",                           6, "\xe2\x80\x85",                3},    /* " " U+02005 */
    {"eng",                              3, "\xc5\x8b",                    2},    /* "ŋ" U+0014B */
    {"ensp",                             4, "\xe2\x80\x82",                3},    /* " " U+02002 */
    {"eogon",                            5, "\xc4\x99",                    2},    /* "ę" U+00119 */
    {"eopf",                             4, "\xf0\x9d\x95\x96",            4},    /* "𝕖" U+1D556 */
    {"epar",                             4, "\xe2\x8b\x95",                3},    /* "⋕" U+022D5 */
    {"eparsl",                           6, "\xe2\xa7\xa3",                3},    /* "⧣" U+029E3 */
    {"eplus",                            5, "\xe2\xa9\xb1",                3},    /* "⩱" U+02A71 */
    {"epsi",                             4, "\xce\xb5",                    2},    /* "ε" U+003B5 */
    {"epsilon",                          7, "\xce\xb5",                    2},    /* "ε" U+003B5 */
    {"epsiv",                            5, "\xcf\xb5",                    2},    /* "ϵ" U+003F5 */
    {"eqcirc",                           6, "\xe2\x89\x96",                3},    /* "≖" U+02256 */
    {"eqcolon",                          7, "\xe2\x89\x95",                3},    /* "≕" U+02255 */
    {"eqsim",                            5, "\xe2\x89\x82",                3},    /* "≂" U+02242 */
    {"eqslantgtr",                      10, "\xe2\xaa\x96",                3},    /* "⪖" U+02A96 */
    {"eqslantless",                     11, "\xe2\xaa\x95",                3},    /* "⪕" U+02A95 */
    {"equals",                           6, "\x3d",                        1},    /* "=" U+0003D */
    {"equest",                           6, "\xe2\x89\x9f",                3},    /* "≟" U+0225F */
    {"equiv",                            5, "\xe2\x89\xa1",                3},    /* "≡" U+02261 */
    {"equivDD",                          7, "\xe2\xa9\xb8",                3},    /* "⩸" U+02A78 */
    {"eqvparsl",                         8, "\xe2\xa7\xa5",                3},    /* "⧥" U+029E5 */
    {"erDot",                            5, "\xe2\x89\x93",                3},    /* "≓" U+02253 */
    {"erarr",                            5, "\xe2\xa5\xb1",                3},    /* "⥱" U+02971 */
    {"escr",                             4, "\xe2\x84\xaf",                3},    /* "ℯ" U+0212F */
    {"esdot",                            5, "\xe2\x89\x90",                3},    /* "≐" U+02250 */
    {"esim",                             4, "\xe2\x89\x82",                3},    /* "≂" U+02242 */
    {"eta",                              3, "\xce\xb7",                    2},    /* "η" U+003B7 */
    {"eth",                              3, "\xc3\xb0",                    2},    /* "ð" U+000F0 */
    {"euml",                             4, "\xc3\xab",                    2},    /* "ë" U+000EB */
    {"euro",                             4, "\xe2\x82\xac",                3},    /* "€" U+020AC */
    {"excl",                             4, "\x21",                        1},    /* "!" U+00021 */
    {"exist",                            5, "\xe2\x88\x83",                3},    /* "∃" U+02203 */
    {"expectation",                     11, "\xe2\x84\xb0",                3},    /* "ℰ" U+02130 */
    {"exponentiale",                    12, "\xe2\x85\x87",                3},    /* "ⅇ" U+02147 */
    {"fallingdotseq",                   13, "\xe2\x89\x92",                3},    /* "≒" U+02252 */
    {"fcy",                              3, "\xd1\x84",                    2},    /* "ф" U+00444 */
    {"female",                           6, "\xe2\x99\x80",                3},    /* "♀" U+02640 */
    {"ffilig",                           6, "\xef\xac\x83",                3},    /* "ﬃ" U+0FB03 */
    {"fflig",                            5, "\xef\xac\x80",                3},    /* "ﬀ" U+0FB00 */
    {"ffllig",                           6, "\xef\xac\x84",                3},    /* "ﬄ" U+0FB04 */
    {"ffr",                              3, "\xf0\x9d\x94\xa3",            4},    /* "𝔣" U+1D523 */
    {"filig",                            5, "\xef\xac\x81",                3},    /* "ﬁ" U+0FB01 */
    {"fjlig",                            5, "\x66\x6a",                    2},    /* "fj" U+00066 U+0006A */
    {"flat",                             4, "\xe2\x99\xad",                3},    /* "♭" U+0266D */
    {"fllig",                            5, "\xef\xac\x82",                3},    /* "ﬂ" U+0FB02 */
    {"fltns",                            5, "\xe2\x96\xb1",                3},    /* "▱" U+025B1 */
    {"fnof",                             4, "\xc6\x92",                    2},    /* "ƒ" U+00192 */
    {"fopf",                             4, "\xf0\x9d\x95\x97",            4},    /* "𝕗" U+1D557 */
    {"forall",                           6, "\xe2\x88\x80",                3},    /* "∀" U+02200 */
    {"fork",                             4, "\xe2\x8b\x94",                3},    /* "⋔" U+022D4 */
    {"forkv",                            5, "\xe2\xab\x99",                3},    /* "⫙" U+02AD9 */
    {"fpartint",                         8, "\xe2\xa8\x8d",                3},    /* "⨍" U+02A0D */
    {"frac12",                           6, "\xc2\xbd",                    2},    /* "½" U+000BD */
    {"frac13",                           6, "\xe2\x85\x93",                3},    /* "⅓" U+02153 */
    {"frac14",                           6, "\xc2\xbc",                    2},    /* "¼" U+000BC */
    {"frac15",                           6, "\xe2\x85\x95",                3},    /* "⅕" U+02155 */
    {"frac16",                           6, "\xe2\x85\x99",                3},    /* "⅙" U+02159 */
    {"frac18",                           6, "\xe2\x85\x9b",                3},    /* "⅛" U+0215B */
    {"frac23",                           6, "\xe2\x85\x94",                3},    /* "⅔" U+02154 */
    {"frac25",                           6, "\xe2\x85\x96",                3},    /* "⅖" U+02156 */
    {"frac34",                           6, "\xc2\xbe",                    2},    /* "¾" U+000BE */
    {"frac35",                           6, "\xe2\x85\x97",                3},    /* "⅗" U+02157 */
    {"frac38",                           6, "\xe2\x85\x9c",                3},    /* "⅜" U+0215C */
    {"frac45",                           6, "\xe2\x85\x98",                3},    /* "⅘" U+02158 */
    {"frac56",                           6, "\xe2\x85\x9a",                3},    /* "⅚" U+0215A */
    {"frac58",                           6, "\xe2\x85\x9d",                3},    /* "⅝" U+0215D */
    {"frac78",                           6, "\xe2\x85\x9e",                3},    /* "⅞" U+0215E */
    {"frasl",                            5, "\xe2\x81\x84",                3},    /* "⁄" U+02044 */
    {"frown",                            5, "\xe2\x8c\xa2",                3},    /* "⌢" U+02322 */
    {"fscr",                             4, "\xf0\x9d\x92\xbb",            4},    /* "𝒻" U+1D4BB */
    {"gE",                               2, "\xe2\x89\xa7",                3},    /* "≧" U+02267 */
    {"gEl",                              3, "\xe2\xaa\x8c",                3},    /* "⪌" U+02A8C */
    {"gacute",                           6, "\xc7\xb5",                    2},    /* "ǵ" U+001F5 */
    {"gamma",                            5, "\xce\xb3",                    2},    /* "γ" U+003B3 */
    {"gammad",                           6, "\xcf\x9d",                    2},    /* "ϝ" U+003DD */
    {"gap",                              3, "\xe2\xaa\x86",                3},    /* "⪆" U+02A86 */
    {"gbreve",                           6, "\xc4\x9f",                    2},    /* "ğ" U+0011F */
    {"gcirc",                            5, "\xc4\x9d",                    2},    /* "ĝ" U+0011D */
    {"gcy",                              3, "\xd0\xb3",                    2},    /* "г" U+00433 */
    {"gdot",                             4, "\xc4\xa1",                    2},    /* "ġ" U+00121 */
    {"ge",                               2, "\xe2\x89\xa5",                3},    /* "≥" U+02265 */
    {"gel",                              3, "\xe2\x8b\x9b",                3},    /* "⋛" U+022DB */
    {"geq",                              3, "\xe2\x89\xa5",                3},    /* "≥" U+02265 */
    {"geqq",                             4, "\xe2\x89\xa7",                3},    /* "≧" U+02267 */
    {"geqslant",                         8, "\xe2\xa9\xbe",                3},    /* "⩾" U+02A7E */
    {"ges",                              3, "\xe2\xa9\xbe",                3},    /* "⩾" U+02A7E */
    {"gescc",                            5, "\xe2\xaa\xa9",                3},    /* "⪩" U+02AA9 */
    {"gesdot",                           6, "\xe2\xaa\x80",                3},    /* "⪀" U+02A80 */
    {"gesdoto",                          7, "\xe2\xaa\x82",                3},    /* "⪂" U+02A82 */
    {"gesdotol",                         8, "\xe2\xaa\x84",                3},    /* "⪄" U+02A84 */
    {"gesl",                             4, "\xe2\x8b\x9b\xef\xb8\x80",    6},    /* "⋛︀" U+022DB U+0FE00 */
    {"gesles",                           6, "\xe2\xaa\x94",                3},    /* "⪔" U+02A94 */
    {"gfr",                              3, "\xf0\x9d\x94\xa4",            4},    /* "𝔤" U+1D524 */
    {"gg",                               2, "\xe2\x89\xab",                3},    /* "≫" U+0226B */
    {"ggg",                              3, "\xe2\x8b\x99",                3},    /* "⋙" U+022D9 */
    {"gimel",                            5, "\xe2\x84\xb7",                3},    /* "ℷ" U+02137 */
    {"gjcy",                             4, "\xd1\x93",                    2},    /* "ѓ" U+00453 */
    {"gl",                               2, "\xe2\x89\xb7",                3},    /* "≷" U+02277 */
    {"glE",                              3, "\xe2\xaa\x92",                3},    /* "⪒" U+02A92 */
    {"gla",                              3, "\xe2\xaa\xa5",                3},    /* "⪥" U+02AA5 */
    {"glj",                              3, "\xe2\xaa\xa4",                3},    /* "⪤" U+02AA4 */
    {"gnE",                              3, "\xe2\x89\xa9",                3},    /* "≩" U+02269 */
    {"gnap",                             4, "\xe2\xaa\x8a",                3},    /* "⪊" U+02A8A */
    {"gnapprox",                         8, "\xe2\xaa\x8a",                3},    /* "⪊" U+02A8A */
    {"gne",                              3, "\xe2\xaa\x88",                3},    /* "⪈" U+02A88 */
    {"gneq",                             4, "\xe2\xaa\x88",                3},    /* "⪈" U+02A88 */
    {"gneqq",                            5, "\xe2\x89\xa9",                3},    /* "≩" U+02269 */
    {"gnsim",                            5, "\xe2\x8b\xa7",                3},    /* "⋧" U+022E7 */
    {"gopf",                             4, "\xf0\x9d\x95\x98",            4},    /* "𝕘" U+1D558 */
    {"grave",                            5, "\x60",                        1},    /* "`" U+00060 */
    {"gscr",                             4, "\xe2\x84\x8a",                3},    /* "ℊ" U+0210A */
    {"gsim",                             4, "\xe2\x89\xb3",                3},    /* "≳" U+02273 */
    {"gsime",                            5, "\xe2\xaa\x8e",                3},    /* "⪎" U+02A8E */
    {"gsiml",                            5, "\xe2\xaa\x90",                3},    /* "⪐" U+02A90 */
    {"gt",                               2, "\x3e",                        1},    /* ">" U+0003E */
    {"gtcc",                             4, "\xe2\xaa\xa7",                3},    /* "⪧" U+02AA7 */
    {"gtcir",                            5, "\xe2\xa9\xba",                3},    /* "⩺" U+02A7A */
    {"gtdot",                            5, "\xe2\x8b\x97",                3},    /* "⋗" U+022D7 */
    {"gtlPar",                           6, "\xe2\xa6\x95",                3},    /* "⦕" U+02995 */
    {"gtquest",                          7, "\xe2\xa9\xbc",                3},    /* "⩼" U+02A7C */
    {"gtrapprox",                        9, "\xe2\xaa\x86",                3},    /* "⪆" U+02A86 */
    {"gtrarr",                           6, "\xe2\xa5\xb8",                3},    /* "⥸" U+02978 */
    {"gtrdot",                           6, "\xe2\x8b\x97",                3},    /* "⋗" U+022D7 */
    {"gtreqless",                        9, "\xe2\x8b\x9b",                3},    /* "⋛" U+022DB */
    {"gtreqqless",                      10, "\xe2\xaa\x8c",                3},    /* "⪌" U+02A8C */
    {"gtrless",                          7, "\xe2\x89\xb7",                3},    /* "≷" U+02277 */
    {"gtrsim",                           6, "\xe2\x89\xb3",                3},    /* "≳" U+02273 */
    {"gvertneqq",                        9, "\xe2\x89\xa9\xef\xb8\x80",    6},    /* "≩︀" U+02269 U+0FE00 */
    {"gvnE",                             4, "\xe2\x89\xa9\xef\xb8\x80",    6},    /* "≩︀" U+02269 U+0FE00 */
    {"hArr",                             4, "\xe2\x87\x94",                3},    /* "⇔" U+021D4 */
    {"hairsp",                           6, "\xe2\x80\x8a",                3},    /* " " U+0200A */
    {"half",                             4, "\xc2\xbd",                    2},    /* "½" U+000BD */
    {"hamilt",                           6, "\xe2\x84\x8b",                3},    /* "ℋ" U+0210B */
    {"hardcy",                           6, "\xd1\x8a",                    2},    /* "ъ" U+0044A */
    {"harr",                             4, "\xe2\x86\x94",                3},    /* "↔" U+02194 */
    {"harrcir",                          7, "\xe2\xa5\x88",                3},    /* "⥈" U+02948 */
    {"harrw",                            5, "\xe2\x86\xad",                3},    /* "↭" U+021AD */
    {"hbar",                             4, "\xe2\x84\x8f",                3},    /* "ℏ" U+0210F */
    {"hcirc",                            5, "\xc4\xa5",                    2},    /* "ĥ" U+00125 */
    {"hearts",                           6, "\xe2\x99\xa5",                3},    /* "♥" U+02665 */
    {"heartsuit",                        9, "\xe2\x99\xa5",                3},    /* "♥" U+02665 */
    {"hellip",                           6, "\xe2\x80\xa6",                3},    /* "…" U+02026 */
    {"hercon",                           6, "\xe2\x8a\xb9",                3},    /* "⊹" U+022B9 */
    {"hfr",                              3, "\xf0\x9d\x94\xa5",            4},    /* "𝔥" U+1D525 */
    {"hksearow",                         8, "\xe2\xa4\xa5",                3},    /* "⤥" U+02925 */
    {"hkswarow",                         8, "\xe2\xa4\xa6",                3},    /* "⤦" U+02926 */
    {"hoarr",                            5, "\xe2\x87\xbf",                3},    /* "⇿" U+021FF */
    {"homtht",                           6, "\xe2\x88\xbb",                3},    /* "∻" U+0223B */
    {"hookleftarrow",                   13, "\xe2\x86\xa9",                3},    /* "↩" U+021A9 */
    {"hookrightarrow",                  14, "\xe2\x86\xaa",                3},    /* "↪" U+021AA */
    {"hopf",                             4, "\xf0\x9d\x95\x99",            4},    /* "𝕙" U+1D559 */
    {"horbar",                           6, "\xe2\x80\x95",                3},    /* "―" U+02015 */
    {"hscr",                             4, "\xf0\x9d\x92\xbd",            4},    /* "𝒽" U+1D4BD */
    {"hslash",                           6, "\xe2\x84\x8f",                3},    /* "ℏ" U+0210F */
    {"hstrok",                           6, "\xc4\xa7",                    2},    /* "ħ" U+00127 */
    {"hybull",                           6, "\xe2\x81\x83",                3},    /* "⁃" U+02043 */
    {"hyphen",                           6, "\xe2\x80\x90",                3},    /* "‐" U+02010 */
    {"iacute",                           6, "\xc3\xad",                    2},    /* "í" U+000ED */
    {"ic",                               2, "\xe2\x81\xa3",                3},    /* "⁣" U+02063 */
    {"icirc",                            5, "\xc3\xae",                    2},    /* "î" U+000EE */
    {"icy",                              3, "\xd0\xb8",                    2},    /* "и" U+00438 */
    {"iecy",                             4, "\xd0\xb5",                    2},    /* "е" U+00435 */
    {"iexcl",                            5, "\xc2\xa1",                    2},    /* "¡" U+000A1 */
    {"iff",                              3, "\xe2\x87\x94",                3},    /* "⇔" U+021D4 */
    {"ifr",                              3, "\xf0\x9d\x94\xa6",            4},    /* "𝔦" U+1D526 */
    {"igrave",                           6, "\xc3\xac",                    2},    /* "ì" U+000EC */
    {"ii",                               2, "\xe2\x85\x88",                3},    /* "ⅈ" U+02148 */
    {"iiiint",                           6, "\xe2\xa8\x8c",                3},    /* "⨌" U+02A0C */
    {"iiint",                            5, "\xe2\x88\xad",                3},    /* "∭" U+0222D */
    {"iinfin",                           6, "\xe2\xa7\x9c",                3},    /* "⧜" U+029DC */
    {"iiota",                            5, "\xe2\x84\xa9",                3},    /* "℩" U+02129 */
    {"ijlig",                            5, "\xc4\xb3",                    2},    /* "ĳ" U+00133 */
    {"imacr",                            5, "\xc4\xab",                    2},    /* "ī" U+0012B */
    {"image",                            5, "\xe2\x84\x91",                3},    /* "ℑ" U+02111 */
    {"imagline",                         8, "\xe2\x84\x90",                3},    /* "ℐ" U+02110 */
    {"imagpart",                         8, "\xe2\x84\x91",                3},    /* "ℑ" U+02111 */
    {"imath",                            5, "\xc4\xb1",                    2},    /* "ı" U+00131 */
    {"imof",                             4, "\xe2\x8a\xb7",                3},    /* "⊷" U+022B7 */
    {"imped",                            5, "\xc6\xb5",                    2},    /* "Ƶ" U+001B5 */
    {"in",                               2, "\xe2\x88\x88",                3},    /* "∈" U+02208 */
    {"incare",                           6, "\xe2\x84\x85",                3},    /* "℅" U+02105 */
    {"infin",                            5, "\xe2\x88\x9e",                3},    /* "∞" U+0221E */
    {"infintie",                         8, "\xe2\xa7\x9d",                3},    /* "⧝" U+029DD */
    {"inodot",                           6, "\xc4\xb1",                    2},    /* "ı" U+00131 */
    {"int",                              3, "\xe2\x88\xab",                3},    /* "∫" U+0222B */
    {"intcal",                           6, "\xe2\x8a\xba",                3},    /* "⊺" U+022BA */
    {"integers",                         8, "\xe2\x84\xa4",                3},    /* "ℤ" U+02124 */
    {"intercal",                         8, "\xe2\x8a\xba",                3},    /* "⊺" U+022BA */
    {"intlarhk",                         8, "\xe2\xa8\x97",                3},    /* "⨗" U+02A17 */
    {"intprod",                          7, "\xe2\xa8\xbc",                3},    /* "⨼" U+02A3C */
    {"iocy",                             4, "\xd1\x91",                    2},    /* "ё" U+00451 */
    {"iogon",                            5, "\xc4\xaf",                    2},    /* "į" U+0012F */
    {"iopf",                             4, "\xf0\x9d\x95\x9a",            4},    /* "𝕚" U+1D55A */
    {"iota",                             4, "\xce\xb9",                    2},    /* "ι" U+003B9 */
    {"iprod",                            5, "\xe2\xa8\xbc",                3},    /* "⨼" U+02A3C */
    {"iquest",                           6, "\xc2\xbf",                    2},    /* "¿" U+000BF */
    {"iscr",                             4, "\xf0\x9d\x92\xbe",            4},    /* "𝒾" U+1D4BE */
    {"isin",                             4, "\xe2\x88\x88",                3},    /* "∈" U+02208 */
    {"isinE",                            5, "\xe2\x8b\xb9",                3},    /* "⋹" U+022F9 */
    {"isindot",                          7, "\xe2\x8b\xb5",                3},    /* "⋵" U+022F5 */
    {"isins",                            5, "\xe2\x8b\xb4",                3},    /* "⋴" U+022F4 */
    {"isinsv",                           6, "\xe2\x8b\xb3",                3},    /* "⋳" U+022F3 */
    {"isinv",                            5, "\xe2\x88\x88",                3},    /* "∈" U+02208 */
    {"it",                               2, "\xe2\x81\xa2",                3},    /* "⁢" U+02062 */
    {"itilde",                           6, "\xc4\xa9",                    2},    /* "ĩ" U+00129 */
    {"iukcy",                            5, "\xd1\x96",                    2},    /* "і" U+00456 */
    {"iuml",                             4, "\xc3\xaf",                    2},    /* "ï" U+000EF */
    {"jcirc",                            5, "\xc4\xb5",                    2},    /* "ĵ" U+00135 */
    {"jcy",                              3, "\xd0\xb9",                    2},    /* "й" U+00439 */
    {"jfr",                              3, "\xf0\x9d\x94\xa7",            4},    /* "𝔧" U+1D527 */
    {"jmath",                            5, "\xc8\xb7",                    2},    /* "ȷ" U+00237 */
    {"jopf",                             4, "\xf0\x9d\x95\x9b",            4},    /* "𝕛" U+1D55B */
    {"jscr",                             4, "\xf0\x9d\x92\xbf",            4},    /* "𝒿" U+1D4BF */
    {"jsercy",                           6, "\xd1\x98",                    2},    /* "ј" U+00458 */
    {"jukcy",                            5, "\xd1\x94",                    2},    /* "є" U+00454 */
    {"kappa",                            5, "\xce\xba",                    2},    /* "κ" U+003BA */
    {"kappav",                           6, "\xcf\xb0",                    2},    /* "ϰ" U+003F0 */
    {"kcedil",                           6, "\xc4\xb7",                    2},    /* "ķ" U+00137 */
    {"kcy",                              3, "\xd0\xba",                    2},    /* "к" U+0043A */
    {"kfr",                              3, "\xf0\x9d\x94\xa8",            4},    /* "𝔨" U+1D528 */
    {"kgreen",                           6, "\xc4\xb8",                    2},    /* "ĸ" U+00138 */
    {"khcy",                             4, "\xd1\x85",                    2},    /* "х" U+00445 */
    {"kjcy",                             4, "\xd1\x9c",                    2},    /* "ќ" U+0045C */
    {"kopf",                             4, "\xf0\x9d\x95\x9c",            4},    /* "𝕜" U+1D55C */
    {"kscr",                             4, "\xf0\x9d\x93\x80",            4},    /* "𝓀" U+1D4C0 */
    {"lAarr",                            5, "\xe2\x87\x9a",                3},    /* "⇚" U+021DA */
    {"lArr",                             4, "\xe2\x87\x90",                3},    /* "⇐" U+021D0 */
    {"lAtail",                           6, "\xe2\xa4\x9b",                3},    /* "⤛" U+0291B */
    {"lBarr",                            5, "\xe2\xa4\x8e",                3},    /* "⤎" U+0290E */
    {"lE",                               2, "\xe2\x89\xa6",                3},    /* "≦" U+02266 */
    {"lEg",                              3, "\xe2\xaa\x8b",                3},    /* "⪋" U+02A8B */
    {"lHar",                             4, "\xe2\xa5\xa2",                3},    /* "⥢" U+02962 */
    {"lacute",                           6, "\xc4\xba",                    2},    /* "ĺ" U+0013A */
    {"laemptyv",                         8, "\xe2\xa6\xb4",                3},    /* "⦴" U+029B4 */
    {"lagran",                           6, "\xe2\x84\x92",                3},    /* "ℒ" U+02112 */
    {"lambda",                           6, "\xce\xbb",                    2},    /* "λ" U+003BB */
    {"lang",                             4, "\xe2\x9f\xa8",                3},    /* "⟨" U+027E8 */
    {"langd",                            5, "\xe2\xa6\x91",                3},    /* "⦑" U+02991 */
    {"langle",                           6, "\xe2\x9f\xa8",                3},    /* "⟨" U+027E8 */
    {"lap",                              3, "\xe2\xaa\x85",                3},    /* "⪅" U+02A85 */
    {"laquo",                            5, "\xc2\xab",                    2},    /* "«" U+000AB */
    {"larr",                             4, "\xe2\x86\x90",                3},    /* "←" U+02190 */
    {"larrb",                            5, "\xe2\x87\xa4",                3},    /* "⇤" U+021E4 */
    {"larrbfs",                          7, "\xe2\xa4\x9f",                3},    /* "⤟" U+0291F */
    {"larrfs",                           6, "\xe2\xa4\x9d",                3},    /* "⤝" U+0291D */
    {"larrhk",                           6, "\xe2\x86\xa9",                3},    /* "↩" U+021A9 */
    {"larrlp",                           6, "\xe2\x86\xab",                3},    /* "↫" U+021AB */
    {"larrpl",                           6, "\xe2\xa4\xb9",                3},    /* "⤹" U+02939 */
    {"larrsim",                          7, "\xe2\xa5\xb3",                3},    /* "⥳" U+02973 */
    {"larrtl",                           6, "\xe2\x86\xa2",                3},    /* "↢" U+021A2 */
    {"lat",                              3, "\xe2\xaa\xab",                3},    /* "⪫" U+02AAB */
    {"latail",                           6, "\xe2\xa4\x99",                3},    /* "⤙" U+02919 */
    {"late",                             4, "\xe2\xaa\xad",                3},    /* "⪭" U+02AAD */
    {"lates",                            5, "\xe2\xaa\xad\xef\xb8\x80",    6},    /* "⪭︀" U+02AAD U+0FE00 */
    {"lbarr",                            5, "\xe2\xa4\x8c",                3},    /* "⤌" U+0290C */
    {"lbbrk",                            5, "\xe2\x9d\xb2",                3},    /* "❲" U+02772 */
    {"lbrace",                           6, "\x7b",                        1},    /* "{" U+0007B */
    {"lbrack",                           6, "\x5b",                        1},    /* "[" U+0005B */
    {"lbrke",                            5, "\xe2\xa6\x8b",                3},    /* "⦋" U+0298B */
    {"lbrksld",                          7, "\xe2\xa6\x8f",                3},    /* "⦏" U+0298F */
    {"lbrkslu",                          7, "\xe2\xa6\x8d",                3},    /* "⦍" U+0298D */
    {"lcaron",                           6, "\xc4\xbe",                    2},    /* "ľ" U+0013E */
    {"lcedil",                           6, "\xc4\xbc",                    2},    /* "ļ" U+0013C */
    {"lceil",                            5, "\xe2\x8c\x88",                3},    /* "⌈" U+02308 */
    {"lcub",                             4, "\x7b",                        1},    /* "{" U+0007B */
    {"lcy",                              3, "\xd0\xbb",                    2},    /* "л" U+0043B */
    {"ldca",                             4, "\xe2\xa4\xb6",                3},    /* "⤶" U+02936 */
    {"ldquo",                            5, "\xe2\x80\x9c",                3},    /* "“" U+0201C */
    {"ldquor",                           6, "\xe2\x80\x9e",                3},    /* "„" U+0201E */
    {"ldrdhar",                          7, "\xe2\xa5\xa7",                3},    /* "⥧" U+02967 */
    {"ldrushar",                         8, "\xe2\xa5\x8b",                3},    /* "⥋" U+0294B */
    {"ldsh",                             4, "\xe2\x86\xb2",                3},    /* "↲" U+021B2 */
    {"le",                               2, "\xe2\x89\xa4",                3},    /* "≤" U+02264 */
    {"leftarrow",                        9, "\xe2\x86\x90",                3},    /* "←" U+02190 */
    {"leftarrowtail",                   13, "\xe2\x86\xa2",                3},    /* "↢" U+021A2 */
    {"leftharpoondown",                 15, "\xe2\x86\xbd",                3},    /* "↽" U+021BD */
    {"leftharpoonup",                   13, "\xe2\x86\xbc",                3},    /* "↼" U+021BC */
    {"leftleftarrows",                  14, "\xe2\x87\x87",                3},    /* "⇇" U+021C7 */
    {"leftrightarrow",                  14, "\xe2\x86\x94",                3},    /* "↔" U+02194 */
    {"leftrightarrows",                 15, "\xe2\x87\x86",                3},    /* "⇆" U+021C6 */
    {"leftrightharpoons",               17, "\xe2\x87\x8b",                3},    /* "⇋" U+021CB */
    {"leftrightsquigarrow",             19, "\xe2\x86\xad",                3},    /* "↭" U+021AD */
    {"leftthreetimes",                  14, "\xe2\x8b\x8b",                3},    /* "⋋" U+022CB */
    {"leg",                              3, "\xe2\x8b\x9a",                3},    /* "⋚" U+022DA */
    {"leq",                              3, "\xe2\x89\xa4",                3},    /* "≤" U+02264 */
    {"leqq",                             4, "\xe2\x89\xa6",                3},    /* "≦" U+02266 */
    {"leqslant",                         8, "\xe2\xa9\xbd",                3},    /* "⩽" U+02A7D */
    {"les",                              3, "\xe2\xa9\xbd",                3},    /* "⩽" U+02A7D */
    {"lescc",                            5, "\xe2\xaa\xa8",                3},    /* "⪨" U+02AA8 */
    {"lesdot",                           6, "\xe2\xa9\xbf",                3},    /* "⩿" U+02A7F */
    {"lesdoto",                          7, "\xe2\xaa\x81",                3},    /* "⪁" U+02A81 */
    {"lesdotor",                         8, "\xe2\xaa\x83",                3},    /* "⪃" U+02A83 */
    {"lesg",                             4, "\xe2\x8b\x9a\xef\xb8\x80",    6},    /* "⋚︀" U+022DA U+0FE00 */
    {"lesges",                           6, "\xe2\xaa\x93",                3},    /* "⪓" U+02A93 */
    {"lessapprox",                      10, "\xe2\xaa\x85",                3},    /* "⪅" U+02A85 */
    {"lessdot",                          7, "\xe2\x8b\x96",                3},    /* "⋖" U+022D6 */
    {"lesseqgtr",                        9, "\xe2\x8b\x9a",                3},    /* "⋚" U+022DA */
    {"lesseqqgtr",                      10, "\xe2\xaa\x8b",                3},    /* "⪋" U+02A8B */
    {"lessgtr",                          7, "\xe2\x89\xb6",                3},    /* "≶" U+02276 */
    {"lesssim",                          7, "\xe2\x89\xb2",                3},    /* "≲" U+02272 */
    {"lfisht",                           6, "\xe2\xa5\xbc",                3},    /* "⥼" U+0297C */
    {"lfloor",                           6, "\xe2\x8c\x8a",                3},    /* "⌊" U+0230A */
    {"lfr",                              3, "\xf0\x9d\x94\xa9",            4},    /* "𝔩" U+1D529 */
    {"lg",                               2, "\xe2\x89\xb6",                3},    /* "≶" U+02276 */
    {"lgE",                              3, "\xe2\xaa\x91",                3},    /* "⪑" U+02A91 */
    {"lhard",                            5, "\xe2\x86\xbd",                3},    /* "↽" U+021BD */
    {"lharu",                            5, "\xe2\x86\xbc",                3},    /* "↼" U+021BC */
    {"lharul",                           6, "\xe2\xa5\xaa",                3},    /* "⥪" U+0296A */
    {"lhblk",                            5, "\xe2\x96\x84",                3},    /* "▄" U+02584 */
    {"ljcy",                             4, "\xd1\x99",                    2},    /* "љ" U+00459 */
    {"ll",                               2, "\xe2\x89\xaa",                3},    /* "≪" U+0226A */
    {"llarr",                            5, "\xe2\x87\x87",                3},    /* "⇇" U+021C7 */
    {"llcorner",                         8, "\xe2\x8c\x9e",                3},    /* "⌞" U+0231E */
    {"llhard",                           6, "\xe2\xa5\xab",                3},    /* "⥫" U+0296B */
    {"lltri",                            5, "\xe2\x97\xba",                3},    /* "◺" U+025FA */
    {"lmidot",                           6, "\xc5\x80",                    2},    /* "ŀ" U+00140 */
    {"lmoust",                           6, "\xe2\x8e\xb0",                3},    /* "⎰" U+023B0 */
    {"lmoustache",                      10, "\xe2\x8e\xb0",                3},    /* "⎰" U+023B0 */
    {"lnE",                              3, "\xe2\x89\xa8",                3},    /* "≨" U+02268 */
    {"lnap",                             4, "\xe2\xaa\x89",                3},    /* "⪉" U+02A89 */
    {"lnapprox",                         8, "\xe2\xaa\x89",                3},    /* "⪉" U+02A89 */
    {"lne",                              3, "\xe2\xaa\x87",                3},    /* "⪇" U+02A87 */
    {"lneq",                             4, "\xe2\xaa\x87",                3},    /* "⪇" U+02A87 */
    {"lneqq",                            5, "\xe2\x89\xa8",                3},    /* "≨" U+02268 */
    {"lnsim",                            5, "\xe2\x8b\xa6",                3},    /* "⋦" U+022E6 */
    {"loang",                            5, "\xe2\x9f\xac",                3},    /* "⟬" U+027EC */
    {"loarr",                            5, "\xe2\x87\xbd",                3},    /* "⇽" U+021FD */
    {"lobrk",                            5, "\xe2\x9f\xa6",                3},    /* "⟦" U+027E6 */
    {"longleftarrow",                   13, "\xe2\x9f\xb5",                3},    /* "⟵" U+027F5 */
    {"longleftrightarrow",              18, "\xe2\x9f\xb7",                3},    /* "⟷" U+027F7 */
    {"longmapsto",                      10, "\xe2\x9f\xbc",                3},    /* "⟼" U+027FC */
    {"longrightarrow",                  14, "\xe2\x9f\xb6",                3},    /* "⟶" U+027F6 */
    {"looparrowleft",                   13, "\xe2\x86\xab",                3},    /* "↫" U+021AB */
    {"looparrowright",                  14, "\xe2\x86\xac",                3},    /* "↬" U+021AC */
    {"lopar",                            5, "\xe2\xa6\x85",                3},    /* "⦅" U+02985 */
    {"lopf",                             4, "\xf0\x9d\x95\x9d",            4},    /* "𝕝" U+1D55D */
    {"loplus",                           6, "\xe2\xa8\xad",                3},    /* "⨭" U+02A2D */
    {"lotimes",                          7, "\xe2\xa8\xb4",                3},    /* "⨴" U+02A34 */
    {"lowast",                           6, "\xe2\x88\x97",                3},    /* "∗" U+02217 */
    {"lowbar",                           6, "\x5f",                        1},    /* "_" U+0005F */
    {"loz",                              3, "\xe2\x97\x8a",                3},    /* "◊" U+025CA */
    {"lozenge",                          7, "\xe2\x97\x8a",                3},    /* "◊" U+025CA */
    {"lozf",                             4, "\xe2\xa7\xab",                3},    /* "⧫" U+029EB */
    {"lpar",                             4, "\x28",                        1},    /* "(" U+00028 */
    {"lparlt",                           6, "\xe2\xa6\x93",                3},    /* "⦓" U+02993 */
    {"lrarr",                            5, "\xe2\x87\x86",                3},    /* "⇆" U+021C6 */
    {"lrcorner",                         8, "\xe2\x8c\x9f",                3},    /* "⌟" U+0231F */
    {"lrhar",                            5, "\xe2\x87\x8b",                3},    /* "⇋" U+021CB */
    {"lrhard",                           6, "\xe2\xa5\xad",                3},    /* "⥭" U+0296D */
    {"lrm",                              3, "\xe2\x80\x8e",                3},    /* "‎" U+0200E */
    {"lrtri",                            5, "\xe2\x8a\xbf",                3},    /* "⊿" U+022BF */
    {"lsaquo",                           6, "\xe2\x80\xb9",                3},    /* "‹" U+02039 */
    {"lscr",                             4, "\xf0\x9d\x93\x81",            4},    /* "𝓁" U+1D4C1 */
    {"lsh",                              3, "\xe2\x86\xb0",                3},    /* "↰" U+021B0 */
    {"lsim",                             4, "\xe2\x89\xb2",                3},    /* "≲" U+02272 */
    {"lsime",                            5, "\xe2\xaa\x8d",                3},    /* "⪍" U+02A8D */
    {"lsimg",                            5, "\xe2\xaa\x8f",                3},    /* "⪏" U+02A8F */
    {"lsqb",                             4, "\x5b",                        1},    /* "[" U+0005B */
    {"lsquo",                            5, "\xe2\x80\x98",                3},    /* "‘" U+02018 */
    {"lsquor",                           6, "\xe2\x80\x9a",                3},    /* "‚" U+0201A */
    {"lstrok",                           6, "\xc5\x82",                    2},    /* "ł" U+00142 */
    {"lt",                               2, "\x3c",                        1},    /* "<" U+0003C */
    {"ltcc",                             4, "\xe2\xaa\xa6",                3},    /* "⪦" U+02AA6 */
    {"ltcir",                            5, "\xe2\xa9\xb9",                3},    /* "⩹" U+02A79 */
    {"ltdot",                            5, "\xe2\x8b\x96",                3},    /* "⋖" U+022D6 */
    {"lthree",                           6, "\xe2\x8b\x8b",                3},    /* "⋋" U+022CB */
    {"ltimes",                           6, "\xe2\x8b\x89",                3},    /* "⋉" U+022C9 */
    {"ltlarr",                           6, "\xe2\xa5\xb6",                3},    /* "⥶" U+02976 */
    {"ltquest",                          7, "\xe2\xa9\xbb",                3},    /* "⩻" U+02A7B */
    {"ltrPar",                           6, "\xe2\xa6\x96",                3},    /* "⦖" U+02996 */
    {"ltri",                             4, "\xe2\x97\x83",                3},    /* "◃" U+025C3 */
    {"ltrie",                            5, "\xe2\x8a\xb4",                3},    /* "⊴" U+022B4 */
    {"ltrif",                            5, "\xe2\x97\x82",                3},    /* "◂" U+025C2 */
    {"lurdshar",                         8, "\xe2\xa5\x8a",                3},    /* "⥊" U+0294A */
    {"luruhar",                          7, "\xe2\xa5\xa6",                3},    /* "⥦" U+02966 */
    {"lvertneqq",                        9, "\xe2\x89\xa8\xef\xb8\x80",    6},    /* "≨︀" U+02268 U+0FE00 */
    {"lvnE",                             4, "\xe2\x89\xa8\xef\xb8\x80",    6},    /* "≨︀" U+02268 U+0FE00 */
    {"mDDot",                            5, "\xe2\x88\xba",                3},    /* "∺" U+0223A */
    {"macr",                             4, "\xc2\xaf",                    2},    /* "¯" U+000AF */
    {"male",                             4, "\xe2\x99\x82",                3},    /* "♂" U+02642 */
    {"malt",                             4, "\xe2\x9c\xa0",                3},    /* "✠" U+02720 */
    {"maltese",                          7, "\xe2\x9c\xa0",                3},    /* "✠" U+02720 */
    {"map",                              3, "\xe2\x86\xa6",                3},    /* "↦" U+021A6 */
    {"mapsto",                           6, "\xe2\x86\xa6",                3},    /* "↦" U+021A6 */
    {"mapstodown",                      10, "\xe2\x86\xa7",                3},    /* "↧" U+021A7 */
    {"mapstoleft",                      10, "\xe2\x86\xa4",                3},    /* "↤" U+021A4 */
    {"mapstoup",                         8, "\xe2\x86\xa5",                3},    /* "↥" U+021A5 */
    {"marker",                           6, "\xe2\x96\xae",                3},    /* "▮" U+025AE */
    {"mcomma",                           6, "\xe2\xa8\xa9",                3},    /* "⨩" U+02A29 */
    {"mcy",                              3, "\xd0\xbc",                    2},    /* "м" U+0043C */
    {"mdash",                            5, "\xe2\x80\x94",                3},    /* "—" U+02014 */
    {"measuredangle",                   13, "\xe2\x88\xa1",                3},    /* "∡" U+02221 */
    {"mfr",                              3, "\xf0\x9d\x94\xaa",            4},    /* "𝔪" U+1D52A */
    {"mho",                              3, "\xe2\x84\xa7",                3},    /* "℧" U+02127 */
    {"micro",                            5, "\xc2\xb5",                    2},    /* "µ" U+000B5 */
    {"mid",                              3, "\xe2\x88\xa3",                3},    /* "∣" U+02223 */
    {"midast",                           6, "\x2a",                        1},    /* "*" U+0002A */
    {"midcir",                           6, "\xe2\xab\xb0",                3},    /* "⫰" U+02AF0 */
    {"middot",                           6, "\xc2\xb7",                    2},    /* "·" U+000B7 */
    {"minus",                            5, "\xe2\x88\x92",                3},    /* "−" U+02212 */
    {"minusb",                           6, "\xe2\x8a\x9f",                3},    /* "⊟" U+0229F */
    {"minusd",                           6, "\xe2\x88\xb8",                3},    /* "∸" U+02238 */
    {"minusdu",                          7, "\xe2\xa8\xaa",                3},    /* "⨪" U+02A2A */
    {"mlcp",                             4, "\xe2\xab\x9b",                3},    /* "⫛" U+02ADB */
    {"mldr",                             4, "\xe2\x80\xa6",                3},    /* "…" U+02026 */
    {"mnplus",                           6, "\xe2\x88\x93",                3},    /* "∓" U+02213 */
    {"models",                           6, "\xe2\x8a\xa7",                3},    /* "⊧" U+022A7 */
    {"mopf",                             4, "\xf0\x9d\x95\x9e",            4},    /* "𝕞" U+1D55E */
    {"mp",                               2, "\xe2\x88\x93",                3},    /* "∓" U+02213 */
    {"mscr",                             4, "\xf0\x9d\x93\x82",            4},    /* "𝓂" U+1D4C2 */
    {"mstpos",                           6, "\xe2\x88\xbe",                3},    /* "∾" U+0223E */
    {"mu",                               2, "\xce\xbc",                    2},    /* "μ" U+003BC */
    {"multimap",                         8, "\xe2\x8a\xb8",                3},    /* "⊸" U+022B8 */
    {"mumap",                            5, "\xe2\x8a\xb8",                3},    /* "⊸" U+022B8 */
    {"nGg",                              3, "\xe2\x8b\x99\xcc\xb8",        5},    /* "⋙̸" U+022D9 U+00338 */
    {"nGt",                              3, "\xe2\x89\xab\xe2\x83\x92",    6},    /* "≫⃒" U+0226B U+020D2 */
    {"nGtv",                             4, "\xe2\x89\xab\xcc\xb8",        5},    /* "≫̸" U+0226B U+00338 */
    {"nLeftarrow",                      10, "\xe2\x87\x8d",                3},    /* "⇍" U+021CD */
    {"nLeftrightarrow",                 15, "\xe2\x87\x8e",                3},    /* "⇎" U+021CE */
    {"nLl",                              3, "\xe2\x8b\x98\xcc\xb8",        5},    /* "⋘̸" U+022D8 U+00338 */
    {"nLt",                              3, "\xe2\x89\xaa\xe2\x83\x92",    6},    /* "≪⃒" U+0226A U+020D2 */
    {"nLtv",                             4, "\xe2\x89\xaa\xcc\xb8",        5},    /* "≪̸" U+0226A U+00338 */
    {"nRightarrow",                     11, "\xe2\x87\x8f",                3},    /* "⇏" U+021CF */
    {"nVDash",                           6, "\xe2\x8a\xaf",                3},    /* "⊯" U+022AF */
    {"nVdash",                           6, "\xe2\x8a\xae",                3},    /* "⊮" U+022AE */
    {"nabla",                            5, "\xe2\x88\x87",                3},    /* "∇" U+02207 */
    {"nacute",                           6, "\xc5\x84",                    2},    /* "ń" U+00144 */
    {"nang",                             4, "\xe2\x88\xa0\xe2\x83\x92",    6},    /* "∠⃒" U+02220 U+020D2 */
    {"nap",                              3, "\xe2\x89\x89",                3},    /* "≉" U+02249 */
    {"napE",                             4, "\xe2\xa9\xb0\xcc\xb8",        5},    /* "⩰̸" U+02A70 U+00338 */
    {"napid",                            5, "\xe2\x89\x8b\xcc\xb8",        5},    /* "≋̸" U+0224B U+00338 */
    {"napos",                            5, "\xc5\x89",                    2},    /* "ŉ" U+00149 */
    {"napprox",                          7, "\xe2\x89\x89",                3},    /* "≉" U+02249 */
    {"natur",                            5, "\xe2\x99\xae",                3},    /* "♮" U+0266E */
    {"natural",                          7, "\xe2\x99\xae",                3},    /* "♮" U+0266E */
    {"naturals",                         8, "\xe2\x84\x95",                3},    /* "ℕ" U+02115 */
    {"nbsp",                             4, "\xc2\xa0",                    2},    /* " " U+000A0 */
    {"nbump",                            5, "\xe2\x89\x8e\xcc\xb8",        5},    /* "≎̸" U+0224E U+00338 */
    {"nbumpe",                           6, "\xe2\x89\x8f\xcc\xb8",        5},    /* "≏̸" U+0224F U+00338 */
    {"ncap",                             4, "\xe2\xa9\x83",                3},    /* "⩃" U+02A43 */
    {"ncaron",                           6, "\xc5\x88",                    2},    /* "ň" U+00148 */
    {"ncedil",                           6, "\xc5\x86",                    2},    /* "ņ" U+00146 */
    {"ncong",                            5, "\xe2\x89\x87",                3},    /* "≇" U+02247 */
    {"ncongdot",                         8, "\xe2\xa9\xad\xcc\xb8",        5},    /* "⩭̸" U+02A6D U+00338 */
    {"ncup",                             4, "\xe2\xa9\x82",                3},    /* "⩂" U+02A42 */
    {"ncy",                              3, "\xd0\xbd",                    2},    /* "н" U+0043D */
    {"ndash",                            5, "\xe2\x80\x93",                3},    /* "–" U+02013 */
    {"ne",                               2, "\xe2\x89\xa0",                3},    /* "≠" U+02260 */
    {"neArr",                            5, "\xe2\x87\x97",                3},    /* "⇗" U+021D7 */
    {"nearhk",                           6, "\xe2\xa4\xa4",                3},    /* "⤤" U+02924 */
    {"nearr",                            5, "\xe2\x86\x97",                3},    /* "↗" U+02197 */
    {"nearrow",                          7, "\xe2\x86\x97",                3},    /* "↗" U+02197 */
    {"nedot",                            5, "\xe2\x89\x90\xcc\xb8",        5},    /* "≐̸" U+02250 U+00338 */
    {"nequiv",                           6, "\xe2\x89\xa2",                3},    /* "≢" U+02262 */
    {"nesear",                           6, "\xe2\xa4\xa8",                3},    /* "⤨" U+02928 */
    {"nesim",                            5, "\xe2\x89\x82\xcc\xb8",        5},    /* "≂̸" U+02242 U+00338 */
    {"nexist",                           6, "\xe2\x88\x84",                3},    /* "∄" U+02204 */
    {"nexists",                          7, "\xe2\x88\x84",                3},    /* "∄" U+02204 */
    {"nfr",                              3, "\xf0\x9d\x94\xab",            4},    /* "𝔫" U+1D52B */
    {"ngE",                              3, "\xe2\x89\xa7\xcc\xb8",        5},    /* "≧̸" U+02267 U+00338 */
    {"nge",                              3, "\xe2\x89\xb1",                3},    /* "≱" U+02271 */
    {"ngeq",                             4, "\xe2\x89\xb1",                3},    /* "≱" U+02271 */
    {"ngeqq",                            5, "\xe2\x89\xa7\xcc\xb8",        5},    /* "≧̸" U+02267 U+00338 */
    {"ngeqslant",                        9, "\xe2\xa9\xbe\xcc\xb8",        5},    /* "⩾̸" U+02A7E U+00338 */
    {"nges",                             4, "\xe2\xa9\xbe\xcc\xb8",        5},    /* "⩾̸" U+02A7E U+00338 */
    {"ngsim",                            5, "\xe2\x89\xb5",                3},    /* "≵" U+02275 */
    {"ngt",                              3, "\xe2\x89\xaf",                3},    /* "≯" U+0226F */
    {"ngtr",                             4, "\xe2\x89\xaf",                3},    /* "≯" U+0226F */
    {"nhArr",                            5, "\xe2\x87\x8e",                3},    /* "⇎" U+021CE */
    {"nharr",                            5, "\xe2\x86\xae",                3},    /* "↮" U+021AE */
    {"nhpar",                            5, "\xe2\xab\xb2",                3},    /* "⫲" U+02AF2 */
    {"ni",                               2, "\xe2\x88\x8b",                3},    /* "∋" U+0220B */
    {"nis",                              3, "\xe2\x8b\xbc",                3},    /* "⋼" U+022FC */
    {"nisd",                             4, "\xe2\x8b\xba",                3},    /* "⋺" U+022FA */
    {"niv",                              3, "\xe2\x88\x8b",                3},    /* "∋" U+0220B */
    {"njcy",                             4, "\xd1\x9a",                    2},    /* "њ" U+0045A */
    {"nlArr",                            5, "\xe2\x87\x8d",                3},    /* "⇍" U+021CD */
    {"nlE",                              3, "\xe2\x89\xa6\xcc\xb8",        5},    /* "≦̸" U+02266 U+00338 */
    {"nlarr",                            5, "\xe2\x86\x9a",                3},    /* "↚" U+0219A */
    {"nldr",                             4, "\xe2\x80\xa5",                3},    /* "‥" U+02025 */
    {"nle",                              3, "\xe2\x89\xb0",                3},    /* "≰" U+02270 */
    {"nleftarrow",                      10, "\xe2\x86\x9a",                3},    /* "↚" U+0219A */
    {"nleftrightarrow",                 15, "\xe2\x86\xae",                3},    /* "↮" U+021AE */
    {"nleq",                             4, "\xe2\x89\xb0",                3},    /* "≰" U+02270 */
    {"nleqq",                            5, "\xe2\x89\xa6\xcc\xb8",        5},    /* "≦̸" U+02266 U+00338 */
    {"nleqslant",                        9, "\xe2\xa9\xbd\xcc\xb8",        5},    /* "⩽̸" U+02A7D U+00338 */
    {"nles",                             4, "\xe2\xa9\xbd\xcc\xb8",        5},    /* "⩽̸" U+02A7D U+00338 */
    {"nless",                            5, "\xe2\x89\xae",                3},    /* "≮" U+0226E */
    {"nlsim",                            5, "\xe2\x89\xb4",                3},    /* "≴" U+02274 */
    {"nlt",                              3, "\xe2\x89\xae",                3},    /* "≮" U+0226E */
    {"nltri",                            5, "\xe2\x8b\xaa",                3},    /* "⋪" U+022EA */
    {"nltrie",                           6, "\xe2\x8b\xac",                3},    /* "⋬" U+022EC */
    {"nmid",                             4, "\xe2\x88\xa4",                3},    /* "∤" U+02224 */
    {"nopf",                             4, "\xf0\x9d\x95\x9f",            4},    /* "𝕟" U+1D55F */
    {"not",                              3, "\xc2\xac",                    2},    /* "¬" U+000AC */
    {"notin",                            5, "\xe2\x88\x89",                3},    /* "∉" U+02209 */
    {"notinE",                           6, "\xe2\x8b\xb9\xcc\xb8",        5},    /* "⋹̸" U+022F9 U+00338 */
    {"notindot",                         8, "\xe2\x8b\xb5\xcc\xb8",        5},    /* "⋵̸" U+022F5 U+00338 */
    {"notinva",                          7, "\xe2\x88\x89",                3},    /* "∉" U+02209 */
    {"notinvb",                          7, "\xe2\x8b\xb7",                3},    /* "⋷" U+022F7 */
    {"notinvc",                          7, "\xe2\x8b\xb6",                3},    /* "⋶" U+022F6 */
    {"notni",                            5, "\xe2\x88\x8c",                3},    /* "∌" U+0220C */
    {"notniva",                          7, "\xe2\x88\x8c",                3},    /* "∌" U+0220C */
    {"notnivb",                          7, "\xe2\x8b\xbe",                3},    /* "⋾" U+022FE */
    {"notnivc",                          7, "\xe2\x8b\xbd",                3},    /* "⋽" U+022FD */
    {"npar",                             4, "\xe2\x88\xa6",                3},    /* "∦" U+02226 */
    {"nparallel",                        9, "\xe2\x88\xa6",                3},    /* "∦" U+02226 */
    {"nparsl",                           6, "\xe2\xab\xbd\xe2\x83\xa5",    6},    /* "⫽⃥" U+02AFD U+020E5 */
    {"npart",                            5, "\xe2\x88\x82\xcc\xb8",        5},    /* "∂̸" U+02202 U+00338 */
    {"npolint",                          7, "\xe2\xa8\x94",                3},    /* "⨔" U+02A14 */
    {"npr",                              3, "\xe2\x8a\x80",                3},    /* "⊀" U+02280 */
    {"nprcue",                           6, "\xe2\x8b\xa0",                3},    /* "⋠" U+022E0 */
    {"npre",                             4, "\xe2\xaa\xaf\xcc\xb8",        5},    /* "⪯̸" U+02AAF U+00338 */
    {"nprec",                            5, "\xe2\x8a\x80",                3},    /* "⊀" U+02280 */
    {"npreceq",                          7, "\xe2\xaa\xaf\xcc\xb8",        5},    /* "⪯̸" U+02AAF U+00338 */
    {"nrArr",                            5, "\xe2\x87\x8f",                3},    /* "⇏" U+021CF */
    {"nrarr",                            5, "\xe2\x86\x9b",                3},    /* "↛" U+0219B */
    {"nrarrc",                           6, "\xe2\xa4\xb3\xcc\xb8",        5},    /* "⤳̸" U+02933 U+00338 */
    {"nrarrw",                           6, "\xe2\x86\x9d\xcc\xb8",        5},    /* "↝̸" U+0219D U+00338 */
    {"nrightarrow",                     11, "\xe2\x86\x9b",                3},    /* "↛" U+0219B */
    {"nrtri",                            5, "\xe2\x8b\xab",                3},    /* "⋫" U+022EB */
    {"nrtrie",                           6, "\xe2\x8b\xad",                3},    /* "⋭" U+022ED */
    {"nsc",                              3, "\xe2\x8a\x81",                3},    /* "⊁" U+02281 */
    {"nsccue",                           6, "\xe2\x8b\xa1",                3},    /* "⋡" U+022E1 */
    {"nsce",                             4, "\xe2\xaa\xb0\xcc\xb8",        5},    /* "⪰̸" U+02AB0 U+00338 */
    {"nscr",                             4, "\xf0\x9d\x93\x83",            4},    /* "𝓃" U+1D4C3 */
    {"nshortmid",                        9, "\xe2\x88\xa4",                3},    /* "∤" U+02224 */
    {"nshortparallel",                  14, "\xe2\x88\xa6",                3},    /* "∦" U+02226 */
    {"nsim",                             4, "\xe2\x89\x81",                3},    /* "≁" U+02241 */
    {"nsime",                            5, "\xe2\x89\x84",                3},    /* "≄" U+02244 */
    {"nsimeq",                           6, "\xe2\x89\x84",                3},    /* "≄" U+02244 */
    {"nsmid",                            5, "\xe2\x88\xa4",                3},    /* "∤" U+02224 */
    {"nspar",                            5, "\xe2\x88\xa6",                3},    /* "∦" U+02226 */
    {"nsqsube",                          7, "\xe2\x8b\xa2",                3},    /* "⋢" U+022E2 */
    {"nsqsupe",                          7, "\xe2\x8b\xa3",                3},    /* "⋣" U+022E3 */
    {"nsub",                             4, "\xe2\x8a\x84",                3},    /* "⊄" U+02284 */
    {"nsubE",                            5, "\xe2\xab\x85\xcc\xb8",        5},    /* "⫅̸" U+02AC5 U+00338 */
    {"nsube",                            5, "\xe2\x8a\x88",                3},    /* "⊈" U+02288 */
    {"nsubset",                          7, "\xe2\x8a\x82\xe2\x83\x92",    6},    /* "⊂⃒" U+02282 U+020D2 */
    {"nsubseteq",                        9, "\xe2\x8a\x88",                3},    /* "⊈" U+02288 */
    {"nsubseteqq",                      10, "\xe2\xab\x85\xcc\xb8",        5},    /* "⫅̸" U+02AC5 U+00338 */
    {"nsucc",                            5, "\xe2\x8a\x81",                3},    /* "⊁" U+02281 */
    {"nsucceq",                          7, "\xe2\xaa\xb0\xcc\xb8",        5},    /* "⪰̸" U+02AB0 U+00338 */
    {"nsup",                             4, "\xe2\x8a\x85",                3},    /* "⊅" U+02285 */
    {"nsupE",                            5, "\xe2\xab\x86\xcc\xb8",        5},    /* "⫆̸" U+02AC6 U+00338 */
    {"nsupe",                            5, "\xe2\x8a\x89",                3},    /* "⊉" U+02289 */
    {"nsupset",                          7, "\xe2\x8a\x83\xe2\x83\x92",    6},    /* "⊃⃒" U+02283 U+020D2 */
    {"nsupseteq",                        9, "\xe2\x8a\x89",                3},    /* "⊉" U+02289 */
    {"nsupseteqq",                      10, "\xe2\xab\x86\xcc\xb8",        5},    /* "⫆̸" U+02AC6 U+00338 */
    {"ntgl",                             4, "\xe2\x89\xb9",                3},    /* "≹" U+02279 */
    {"ntilde",                           6, "\xc3\xb1",                    2},    /* "ñ" U+000F1 */
    {"ntlg",                             4, "\xe2\x89\xb8",                3},    /* "≸" U+02278 */
    {"ntriangleleft",                   13, "\xe2\x8b\xaa",                3},    /* "⋪" U+022EA */
    {"ntrianglelefteq",                 15, "\xe2\x8b\xac",                3},    /* "⋬" U+022EC */
    {"ntriangleright",                  14, "\xe2\x8b\xab",                3},    /* "⋫" U+022EB */
    {"ntrianglerighteq",                16, "\xe2\x8b\xad",                3},    /* "⋭" U+022ED */
    {"nu",                               2, "\xce\xbd",                    2},    /* "ν" U+003BD */
    {"num",                              3, "\x23",                        1},    /* "#" U+00023 */
    {"numero",                           6, "\xe2\x84\x96",                3},    /* "№" U+02116 */
    {"numsp",                            5, "\xe2\x80\x87",                3},    /* " " U+02007 */
    {"nvDash",                           6, "\xe2\x8a\xad",                3},    /* "⊭" U+022AD */
    {"nvHarr",                           6, "\xe2\xa4\x84",                3},    /* "⤄" U+02904 */
    {"nvap",                             4, "\xe2\x89\x8d\xe2\x83\x92",    6},    /* "≍⃒" U+0224D U+020D2 */
    {"nvdash",                           6, "\xe2\x8a\xac",                3},    /* "⊬" U+022AC */
    {"nvge",                             4, "\xe2\x89\xa5\xe2\x83\x92",    6},    /* "≥⃒" U+02265 U+020D2 */
    {"nvgt",                             4, "\x3e\xe2\x83\x92",            4},    /* ">⃒" U+0003E U+020D2 */
    {"nvinfin",                          7, "\xe2\xa7\x9e",                3},    /* "⧞" U+029DE */
    {"nvlArr",                           6, "\xe2\xa4\x82",                3},    /* "⤂" U+02902 */
    {"nvle",                             4, "\xe2\x89\xa4\xe2\x83\x92",    6},    /* "≤⃒" U+02264 U+020D2 */
    {"nvlt",                             4, "\x3c\xe2\x83\x92",            4},    /* "<⃒" U+0003C U+020D2 */
    {"nvltrie",                          7, "\xe2\x8a\xb4\xe2\x83\x92",    6},    /* "⊴⃒" U+022B4 U+020D2 */
    {"nvrArr",                           6, "\xe2\xa4\x83",                3},    /* "⤃" U+02903 */
    {"nvrtrie",                          7, "\xe2\x8a\xb5\xe2\x83\x92",    6},    /* "⊵⃒" U+022B5 U+020D2 */
    {"nvsim",                            5, "\xe2\x88\xbc\xe2\x83\x92",    6},    /* "∼⃒" U+0223C U+020D2 */
    {"nwArr",                            5, "\xe2\x87\x96",                3},    /* "⇖" U+021D6 */
    {"nwarhk",                           6, "\xe2\xa4\xa3",                3},    /* "⤣" U+02923 */
    {"nwarr",                            5, "\xe2\x86\x96",                3},    /* "↖" U+02196 */
    {"nwarrow",                          7, "\xe2\x86\x96",                3},    /* "↖" U+02196 */
    {"nwnear",                           6, "\xe2\xa4\xa7",                3},    /* "⤧" U+02927 */
    {"oS",                               2, "\xe2\x93\x88",                3},    /* "Ⓢ" U+024C8 */
    {"oacute",                           6, "\xc3\xb3",                    2},    /* "ó" U+000F3 */
    {"oast",                             4, "\xe2\x8a\x9b",                3},    /* "⊛" U+0229B */
    {"ocir",                             4, "\xe2\x8a\x9a",                3},    /* "⊚" U+0229A */
    {"ocirc",                            5, "\xc3\xb4",                    2},    /* "ô" U+000F4 */
    {"ocy",                              3, "\xd0\xbe",                    2},    /* "о" U+0043E */
    {"odash",                            5, "\xe2\x8a\x9d",                3},    /* "⊝" U+0229D */
    {"odblac",                           6, "\xc5\x91",                    2},    /* "ő" U+00151 */
    {"odiv",                             4, "\xe2\xa8\xb8",                3},    /* "⨸" U+02A38 */
    {"odot",                             4, "\xe2\x8a\x99",                3},    /* "⊙" U+02299 */
    {"odsold",                           6, "\xe2\xa6\xbc",                3},    /* "⦼" U+029BC */
    {"oelig",                            5, "\xc5\x93",                    2},    /* "œ" U+00153 */
    {"ofcir",                            5, "\xe2\xa6\xbf",                3},    /* "⦿" U+029BF */
    {"ofr",                              3, "\xf0\x9d\x94\xac",            4},    /* "𝔬" U+1D52C */
    {"ogon",                             4, "\xcb\x9b",                    2},    /* "˛" U+002DB */
    {"ograve",                           6, "\xc3\xb2",                    2},    /* "ò" U+000F2 */
    {"ogt",                              3, "\xe2\xa7\x81",                3},    /* "⧁" U+029C1 */
    {"ohbar",                            5, "\xe2\xa6\xb5",                3},    /* "⦵" U+029B5 */
    {"ohm",                              3, "\xce\xa9",                    2},    /* "Ω" U+003A9 */
    {"oint",                             4, "\xe2\x88\xae",                3},    /* "∮" U+0222E */
    {"olarr",                            5, "\xe2\x86\xba",                3},    /* "↺" U+021BA */
    {"olcir",                            5, "\xe2\xa6\xbe",                3},    /* "⦾" U+029BE */
    {"olcross",                          7, "\xe2\xa6\xbb",                3},    /* "⦻" U+029BB */
    {"oline",                            5, "\xe2\x80\xbe",                3},    /* "‾" U+0203E */
    {"olt",                              3, "\xe2\xa7\x80",                3},    /* "⧀" U+029C0 */
    {"omacr",                            5, "\xc5\x8d",                    2},    /* "ō" U+0014D */
    {"omega",                            5, "\xcf\x89",                    2},    /* "ω" U+003C9 */
    {"omicron",                          7, "\xce\xbf",                    2},    /* "ο" U+003BF */
    {"omid",                             4, "\xe2\xa6\xb6",                3},    /* "⦶" U+029B6 */
    {"ominus",                           6, "\xe2\x8a\x96",                3},    /* "⊖" U+02296 */
    {"oopf",                             4, "\xf0\x9d\x95\xa0",            4},    /* "𝕠" U+1D560 */
    {"opar",                             4, "\xe2\xa6\xb7",                3},    /* "⦷" U+029B7 */
    {"operp",                            5, "\xe2\xa6\xb9",                3},    /* "⦹" U+029B9 */
    {"oplus",                            5, "\xe2\x8a\x95",                3},    /* "⊕" U+02295 */
    {"or",                               2, "\xe2\x88\xa8",                3},    /* "∨" U+02228 */
    {"orarr",                            5, "\xe2\x86\xbb",                3},    /* "↻" U+021BB */
    {"ord",                              3, "\xe2\xa9\x9d",                3},    /* "⩝" U+02A5D */
    {"order",                            5, "\xe2\x84\xb4",                3},    /* "ℴ" U+02134 */
    {"orderof",                          7, "\xe2\x84\xb4",                3},    /* "ℴ" U+02134 */
    {"ordf",                             4, "\xc2\xaa",                    2},    /* "ª" U+000AA */
    {"ordm",                             4, "\xc2\xba",                    2},    /* "º" U+000BA */
    {"origof",                           6, "\xe2\x8a\xb6",                3},    /* "⊶" U+022B6 */
    {"oror",                             4, "\xe2\xa9\x96",                3},    /* "⩖" U+02A56 */
    {"orslope",                          7, "\xe2\xa9\x97",                3},    /* "⩗" U+02A57 */
    {"orv",                              3, "\xe2\xa9\x9b",                3},    /* "⩛" U+02A5B */
    {"oscr",                             4, "\xe2\x84\xb4",                3},    /* "ℴ" U+02134 */
    {"oslash",                           6, "\xc3\xb8",                    2},    /* "ø" U+000F8 */
    {"osol",                             4, "\xe2\x8a\x98",                3},    /* "⊘" U+02298 */
    {"otilde",                           6, "\xc3\xb5",                    2},    /* "õ" U+000F5 */
    {"otimes",                           6, "\xe2\x8a\x97",                3},    /* "⊗" U+02297 */
    {"otimesas",                         8, "\xe2\xa8\xb6",                3},    /* "⨶" U+02A36 */
    {"ouml",                             4, "\xc3\xb6",                    2},    /* "ö" U+000F6 */
    {"ovbar",                            5, "\xe2\x8c\xbd",                3},    /* "⌽" U+0233D */
    {"par",                              3, "\xe2\x88\xa5",                3},    /* "∥" U+02225 */
    {"para",                             4, "\xc2\xb6",                    2},    /* "¶" U+000B6 */
    {"parallel",                         8, "\xe2\x88\xa5",                3},    /* "∥" U+02225 */
    {"parsim",                           6, "\xe2\xab\xb3",                3},    /* "⫳" U+02AF3 */
    {"parsl",                            5, "\xe2\xab\xbd",                3},    /* "⫽" U+02AFD */
    {"part",                             4, "\xe2\x88\x82",                3},    /* "∂" U+02202 */
    {"pcy",                              3, "\xd0\xbf",                    2},    /* "п" U+0043F */
    {"percnt",                           6, "\x25",                        1},    /* "%" U+00025 */
    {"period",                           6, "\x2e",                        1},    /* "." U+0002E */
    {"permil",                           6, "\xe2\x80\xb0",                3},    /* "‰" U+02030 */
    {"perp",                             4, "\xe2\x8a\xa5",                3},    /* "⊥" U+022A5 */
    {"pertenk",                          7, "\xe2\x80\xb1",                3},    /* "‱" U+02031 */
    {"pfr",                              3, "\xf0\x9d\x94\xad",            4},    /* "𝔭" U+1D52D */
    {"phi",                              3, "\xcf\x86",                    2},    /* "φ" U+003C6 */
    {"phiv",                             4, "\xcf\x95",                    2},    /* "ϕ" U+003D5 */
    {"phmmat",                           6, "\xe2\x84\xb3",                3},    /* "ℳ" U+02133 */
    {"phone",                            5, "\xe2\x98\x8e",                3},    /* "☎" U+0260E */
    {"pi",                               2, "\xcf\x80",                    2},    /* "π" U+003C0 */
    {"pitchfork",                        9, "\xe2\x8b\x94",                3},    /* "⋔" U+022D4 */
    {"piv",                              3, "\xcf\x96",                    2},    /* "ϖ" U+003D6 */
    {"planck",                           6, "\xe2\x84\x8f",                3},    /* "ℏ" U+0210F */
    {"planckh",                          7, "\xe2\x84\x8e",                3},    /* "ℎ" U+0210E */
    {"plankv",                           6, "\xe2\x84\x8f",                3},    /* "ℏ" U+0210F */
    {"plus",                             4, "\x2b",                        1},    /* "+" U+0002B */
    {"plusacir",                         8, "\xe2\xa8\xa3",                3},    /* "⨣" U+02A23 */
    {"plusb",                            5, "\xe2\x8a\x9e",                3},    /* "⊞" U+0229E */
    {"pluscir",                          7, "\xe2\xa8\xa2",                3},    /* "⨢" U+02A22 */
    {"plusdo",                           6, "\xe2\x88\x94",                3},    /* "∔" U+02214 */
    {"plusdu",                           6, "\xe2\xa8\xa5",                3},    /* "⨥" U+02A25 */
    {"pluse",                            5, "\xe2\xa9\xb2",                3},    /* "⩲" U+02A72 */
    {"plusmn",                           6, "\xc2\xb1",                    2},    /* "±" U+000B1 */
    {"plussim",                          7, "\xe2\xa8\xa6",                3},    /* "⨦" U+02A26 */
    {"plustwo",                          7, "\xe2\xa8\xa7",                3},    /* "⨧" U+02A27 */
    {"pm",                               2, "\xc2\xb1",                    2},    /* "±" U+000B1 */
    {"pointint",                         8, "\xe2\xa8\x95",                3},    /* "⨕" U+02A15 */
    {"popf",                             4, "\xf0\x9d\x95\xa1",            4},    /* "𝕡" U+1D561 */
    {"pound",                            5, "\xc2\xa3",                    2},    /* "£" U+000A3 */
    {"pr",                               2, "\xe2\x89\xba",                3},    /* "≺" U+0227A */
    {"prE",                              3, "\xe2\xaa\xb3",                3},    /* "⪳" U+02AB3 */
    {"prap",                             4, "\xe2\xaa\xb7",                3},    /* "⪷" U+02AB7 */
    {"prcue",                            5, "\xe2\x89\xbc",                3},    /* "≼" U+0227C */
    {"pre",                              3, "\xe2\xaa\xaf",                3},    /* "⪯" U+02AAF */
    {"prec",                             4, "\xe2\x89\xba",                3},    /* "≺" U+0227A */
    {"precapprox",                      10, "\xe2\xaa\xb7",                3},    /* "⪷" U+02AB7 */
    {"preccurlyeq",                     11, "\xe2\x89\xbc",                3},    /* "≼" U+0227C */
    {"preceq",                           6, "\xe2\xaa\xaf",                3},    /* "⪯" U+02AAF */
    {"precnapprox",                     11, "\xe2\xaa\xb9",                3},    /* "⪹" U+02AB9 */
    {"precneqq",                         8, "\xe2\xaa\xb5",                3},    /* "⪵" U+02AB5 */
    {"precnsim",                         8, "\xe2\x8b\xa8",                3},    /* "⋨" U+022E8 */
    {"precsim",                          7, "\xe2\x89\xbe",                3},    /* "≾" U+0227E */
    {"prime",                            5, "\xe2\x80\xb2",                3},    /* "′" U+02032 */
    {"primes",                           6, "\xe2\x84\x99",                3},    /* "ℙ" U+02119 */
    {"prnE",                             4, "\xe2\xaa\xb5",                3},    /* "⪵" U+02AB5 */
    {"prnap",                            5, "\xe2\xaa\xb9",                3},    /* "⪹" U+02AB9 */
    {"prnsim",                           6, "\xe2\x8b\xa8",                3},    /* "⋨" U+022E8 */
    {"prod",                             4, "\xe2\x88\x8f",                3},    /* "∏" U+0220F */
    {"profalar",                         8, "\xe2\x8c\xae",                3},    /* "⌮" U+0232E */
    {"profline",                         8, "\xe2\x8c\x92",                3},    /* "⌒" U+02312 */
    {"profsurf",                         8, "\xe2\x8c\x93",                3},    /* "⌓" U+02313 */
    {"prop",                             4, "\xe2\x88\x9d",                3},    /* "∝" U+0221D */
    {"propto",                           6, "\xe2\x88\x9d",                3},    /* "∝" U+0221D */
    {"prsim",                            5, "\xe2\x89\xbe",                3},    /* "≾" U+0227E */
    {"prurel",                           6, "\xe2\x8a\xb0",                3},    /* "⊰" U+022B0 */
    {"pscr",                             4, "\xf0\x9d\x93\x85",            4},    /* "𝓅" U+1D4C5 */
    {"psi",                              3, "\xcf\x88",                    2},    /* "ψ" U+003C8 */
    {"puncsp",                           6, "\xe2\x80\x88",                3},    /* " " U+02008 */
    {"qfr",                              3, "\xf0\x9d\x94\xae",            4},    /* "𝔮" U+1D52E */
    {"qint",                             4, "\xe2\xa8\x8c",                3},    /* "⨌" U+02A0C */
    {"qopf",                             4, "\xf0\x9d\x95\xa2",            4},    /* "𝕢" U+1D562 */
    {"qprime",                           6, "\xe2\x81\x97",                3},    /* "⁗" U+02057 */
    {"qscr",                             4, "\xf0\x9d\x93\x86",            4},    /* "𝓆" U+1D4C6 */
    {"quaternions",                     11, "\xe2\x84\x8d",                3},    /* "ℍ" U+0210D */
    {"quatint",                          7, "\xe2\xa8\x96",                3},    /* "⨖" U+02A16 */
    {"quest",                            5, "\x3f",                        1},    /* "?" U+0003F */
    {"questeq",                          7, "\xe2\x89\x9f",                3},    /* "≟" U+0225F */
    {"quot",                             4, "\x22",                        1},    /* """ U+00022 */
    {"rAarr",                            5, "\xe2\x87\x9b",                3},    /* "⇛" U+021DB */
    {"rArr",                             4, "\xe2\x87\x92",                3},    /* "⇒" U+021D2 */
    {"rAtail",                           6, "\xe2\xa4\x9c",                3},    /* "⤜" U+0291C */
    {"rBarr",                            5, "\xe2\xa4\x8f",                3},    /* "⤏" U+0290F */
    {"rHar",                             4, "\xe2\xa5\xa4",                3},    /* "⥤" U+02964 */
    {"race",                             4, "\xe2\x88\xbd\xcc\xb1",        5},    /* "∽̱" U+0223D U+00331 */
    {"racute",                           6, "\xc5\x95",                    2},    /* "ŕ" U+00155 */
    {"radic",                            5, "\xe2\x88\x9a",                3},    /* "√" U+0221A */
    {"raemptyv",                         8, "\xe2\xa6\xb3",                3},    /* "⦳" U+029B3 */
    {"rang",                             4, "\xe2\x9f\xa9",                3},    /* "⟩" U+027E9 */
    {"rangd",                            5, "\xe2\xa6\x92",                3},    /* "⦒" U+02992 */
    {"range",                            5, "\xe2\xa6\xa5",                3},    /* "⦥" U+029A5 */
    {"rangle",                           6, "\xe2\x9f\xa9",                3},    /* "⟩" U+027E9 */
    {"raquo",                            5, "\xc2\xbb",                    2},    /* "»" U+000BB */
    {"rarr",                             4, "\xe2\x86\x92",                3},    /* "→" U+02192 */
    {"rarrap",                           6, "\xe2\xa5\xb5",                3},    /* "⥵" U+02975 */
    {"rarrb",                            5, "\xe2\x87\xa5",                3},    /* "⇥" U+021E5 */
    {"rarrbfs",                          7, "\xe2\xa4\xa0",                3},    /* "⤠" U+02920 */
    {"rarrc",                            5, "\xe2\xa4\xb3",                3},    /* "⤳" U+02933 */
    {"rarrfs",                           6, "\xe2\xa4\x9e",                3},    /* "⤞" U+0291E */
    {"rarrhk",                           6, "\xe2\x86\xaa",                3},    /* "↪" U+021AA */
    {"rarrlp",                           6, "\xe2\x86\xac",                3},    /* "↬" U+021AC */
    {"rarrpl",                           6, "\xe2\xa5\x85",                3},    /* "⥅" U+02945 */
    {"rarrsim",                          7, "\xe2\xa5\xb4",                3},    /* "⥴" U+02974 */
    {"rarrtl",                           6, "\xe2\x86\xa3",                3},    /* "↣" U+021A3 */
    {"rarrw",                            5, "\xe2\x86\x9d",                3},    /* "↝" U+0219D */
    {"ratail",                           6, "\xe2\xa4\x9a",                3},    /* "⤚" U+0291A */
    {"ratio",                            5, "\xe2\x88\xb6",                3},    /* "∶" U+02236 */
    {"rationals",                        9, "\xe2\x84\x9a",                3},    /* "ℚ" U+0211A */
    {"rbarr",                            5, "\xe2\xa4\x8d",                3},    /* "⤍" U+0290D */
    {"rbbrk",                            5, "\xe2\x9d\xb3",                3},    /* "❳" U+02773 */
    {"rbrace",                           6, "\x7d",                        1},    /* "}" U+0007D */
    {"rbrack",                           6, "\x5d",                        1},    /* "]" U+0005D */
    {"rbrke",                            5, "\xe2\xa6\x8c",                3},    /* "⦌" U+0298C */
    {"rbrksld",                          7, "\xe2\xa6\x8e",                3},    /* "⦎" U+0298E */
    {"rbrkslu",                          7, "\xe2\xa6\x90",                3},    /* "⦐" U+02990 */
    {"rcaron",                           6, "\xc5\x99",                    2},    /* "ř" U+00159 */
    {"rcedil",                           6, "\xc5\x97",                    2},    /* "ŗ" U+00157 */
    {"rceil",                            5, "\xe2\x8c\x89",                3},    /* "⌉" U+02309 */
    {"rcub",                             4, "\x7d",                        1},    /* "}" U+0007D */
    {"rcy",                              3, "\xd1\x80",                    2},    /* "р" U+00440 */
    {"rdca",                             4, "\xe2\xa4\xb7",                3},    /* "⤷" U+02937 */
    {"rdldhar",                          7, "\xe2\xa5\xa9",                3},    /* "⥩" U+02969 */
    {"rdquo",                            5, "\xe2\x80\x9d",                3},    /* "”" U+0201D */
    {"rdquor",                           6, "\xe2\x80\x9d",                3},    /* "”" U+0201D */
    {"rdsh",                             4, "\xe2\x86\xb3",                3},    /* "↳" U+021B3 */
    {"real",                             4, "\xe2\x84\x9c",                3},    /* "ℜ" U+0211C */
    {"realine",                          7, "\xe2\x84\x9b",                3},    /* "ℛ" U+0211B */
    {"realpart",                         8, "\xe2\x84\x9c",                3},    /* "ℜ" U+0211C */
    {"reals",                            5, "\xe2\x84\x9d",                3},    /* "ℝ" U+0211D */
    {"rect",                             4, "\xe2\x96\xad",                3},    /* "▭" U+025AD */
    {"reg",                              3, "\xc2\xae",                    2},    /* "®" U+000AE */
    {"rfisht",                           6, "\xe2\xa5\xbd",                3},    /* "⥽" U+0297D */
    {"rfloor",                           6, "\xe2\x8c\x8b",                3},    /* "⌋" U+0230B */
    {"rfr",                              3, "\xf0\x9d\x94\xaf",            4},    /* "𝔯" U+1D52F */
    {"rhard",                            5, "\xe2\x87\x81",                3},    /* "⇁" U+021C1 */
    {"rharu",                            5, "\xe2\x87\x80",                3},    /* "⇀" U+021C0 */
    {"rharul",                           6, "\xe2\xa5\xac",                3},    /* "⥬" U+0296C */
    {"rho",                              3, "\xcf\x81",                    2},    /* "ρ" U+003C1 */
    {"rhov",                             4, "\xcf\xb1",                    2},    /* "ϱ" U+003F1 */
    {"rightarrow",                      10, "\xe2\x86\x92",                3},    /* "→" U+02192 */
    {"rightarrowtail",                  14, "\xe2\x86\xa3",                3},    /* "↣" U+021A3 */
    {"rightharpoondown",                16, "\xe2\x87\x81",                3},    /* "⇁" U+021C1 */
    {"rightharpoonup",                  14, "\xe2\x87\x80",                3},    /* "⇀" U+021C0 */
    {"rightleftarrows",                 15, "\xe2\x87\x84",                3},    /* "⇄" U+021C4 */
    {"rightleftharpoons",               17, "\xe2\x87\x8c",                3},    /* "⇌" U+021CC */
    {"rightrightarrows",                16, "\xe2\x87\x89",                3},    /* "⇉" U+021C9 */
    {"rightsquigarrow",                 15, "\xe2\x86\x9d",                3},    /* "↝" U+0219D */
    {"rightthreetimes",                 15, "\xe2\x8b\x8c",                3},    /* "⋌" U+022CC */
    {"ring",                             4, "\xcb\x9a",                    2},    /* "˚" U+002DA */
    {"risingdotseq",                    12, "\xe2\x89\x93",                3},    /* "≓" U+02253 */
    {"rlarr",                            5, "\xe2\x87\x84",                3},    /* "⇄" U+021C4 */
    {"rlhar",                            5, "\xe2\x87\x8c",                3},    /* "⇌" U+021CC */
    {"rlm",                              3, "\xe2\x80\x8f",                3},    /* "‏" U+0200F */
    {"rmoust",                           6, "\xe2\x8e\xb1",                3},    /* "⎱" U+023B1 */
    {"rmoustache",                      10, "\xe2\x8e\xb1",                3},    /* "⎱" U+023B1 */
    {"rnmid",                            5, "\xe2\xab\xae",                3},    /* "⫮" U+02AEE */
    {"roang",                            5, "\xe2\x9f\xad",                3},    /* "⟭" U+027ED */
    {"roarr",                            5, "\xe2\x87\xbe",                3},    /* "⇾" U+021FE */
    {"robrk",                            5, "\xe2\x9f\xa7",                3},    /* "⟧" U+027E7 */
    {"ropar",                            5, "\xe2\xa6\x86",                3},    /* "⦆" U+02986 */
    {"ropf",                             4, "\xf0\x9d\x95\xa3",            4},    /* "𝕣" U+1D563 */
    {"roplus",                           6, "\xe2\xa8\xae",                3},    /* "⨮" U+02A2E */
    {"rotimes",                          7, "\xe2\xa8\xb5",                3},    /* "⨵" U+02A35 */
    {"rpar",                             4, "\x29",                        1},    /* ")" U+00029 */
    {"rpargt",                           6, "\xe2\xa6\x94",                3},    /* "⦔" U+02994 */
    {"rppolint",                         8, "\xe2\xa8\x92",                3},    /* "⨒" U+02A12 */
    {"rrarr",                            5, "\xe2\x87\x89",                3},    /* "⇉" U+021C9 */
    {"rsaquo",                           6, "\xe2\x80\xba",                3},    /* "›" U+0203A */
    {"rscr",                             4, "\xf0\x9d\x93\x87",            4},    /* "𝓇" U+1D4C7 */
    {"rsh",                              3, "\xe2\x86\xb1",                3},    /* "↱" U+021B1 */
    {"rsqb",                             4, "\x5d",                        1},    /* "]" U+0005D */
    {"rsquo",                            5, "\xe2\x80\x99",                3},    /* "’" U+02019 */
    {"rsquor",                           6, "\xe2\x80\x99",                3},    /* "’" U+02019 */
    {"rthree",                           6, "\xe2\x8b\x8c",                3},    /* "⋌" U+022CC */
    {"rtimes",                           6, "\xe2\x8b\x8a",                3},    /* "⋊" U+022CA */
    {"rtri",                             4, "\xe2\x96\xb9",                3},    /* "▹" U+025B9 */
    {"rtrie",                            5, "\xe2\x8a\xb5",                3},    /* "⊵" U+022B5 */
    {"rtrif",                            5, "\xe2\x96\xb8",                3},    /* "▸" U+025B8 */
    {"rtriltri",                         8, "\xe2\xa7\x8e",                3},    /* "⧎" U+029CE */
    {"ruluhar",                          7, "\xe2\xa5\xa8",                3},    /* "⥨" U+02968 */
    {"rx",                               2, "\xe2\x84\x9e",                3},    /* "℞" U+0211E */
    {"sacute",                           6, "\xc5\x9b",                    2},    /* "ś" U+0015B */
    {"sbquo",                            5, "\xe2\x80\x9a",                3},    /* "‚" U+0201A */
    {"sc",                               2, "\xe2\x89\xbb",                3},    /* "≻" U+0227B */
    {"scE",                              3, "\xe2\xaa\xb4",                3},    /* "⪴" U+02AB4 */
    {"scap",                             4, "\xe2\xaa\xb8",                3},    /* "⪸" U+02AB8 */
    {"scaron",                           6, "\xc5\xa1",                    2},    /* "š" U+00161 */
    {"sccue",                            5, "\xe2\x89\xbd",                3},    /* "≽" U+0227D */
    {"sce",                              3, "\xe2\xaa\xb0",                3},    /* "⪰" U+02AB0 */
    {"scedil",                           6, "\xc5\x9f",                    2},    /* "ş" U+0015F */
    {"scirc",                            5, "\xc5\x9d",                    2},    /* "ŝ" U+0015D */
    {"scnE",                             4, "\xe2\xaa\xb6",                3},    /* "⪶" U+02AB6 */
    {"scnap",                            5, "\xe2\xaa\xba",                3},    /* "⪺" U+02ABA */
    {"scnsim",                           6, "\xe2\x8b\xa9",                3},    /* "⋩" U+022E9 */
    {"scpolint",                         8, "\xe2\xa8\x93",                3},    /* "⨓" U+02A13 */
    {"scsim",                            5, "\xe2\x89\xbf",                3},    /* "≿" U+0227F */
    {"scy",                              3, "\xd1\x81",                    2},    /* "с" U+00441 */
    {"sdot",                             4, "\xe2\x8b\x85",                3},    /* "⋅" U+022C5 */
    {"sdotb",                            5, "\xe2\x8a\xa1",                3},    /* "⊡" U+022A1 */
    {"sdote",                            5, "\xe2\xa9\xa6",                3},    /* "⩦" U+02A66 */
    {"seArr",                            5, "\xe2\x87\x98",                3},    /* "⇘" U+021D8 */
    {"searhk",                           6, "\xe2\xa4\xa5",                3},    /* "⤥" U+02925 */
    {"searr",                            5, "\xe2\x86\x98",                3},    /* "↘" U+02198 */
    {"searrow",                          7, "\xe2\x86\x98",                3},    /* "↘" U+02198 */
    {"sect",                             4, "\xc2\xa7",                    2},    /* "§" U+000A7 */
    {"semi",                             4, "\x3b",                        1},    /* ";" U+0003B */
    {"seswar",                           6, "\xe2\xa4\xa9",                3},    /* "⤩" U+02929 */
    {"setminus",                         8, "\xe2\x88\x96",                3},    /* "∖" U+02216 */
    {"setmn",                            5, "\xe2\x88\x96",                3},    /* "∖" U+02216 */
    {"sext",                             4, "\xe2\x9c\xb6",                3},    /* "✶" U+02736 */
    {"sfr",                              3, "\xf0\x9d\x94\xb0",            4},    /* "𝔰" U+1D530 */
    {"sfrown",                           6, "\xe2\x8c\xa2",                3},    /* "⌢" U+02322 */
    {"sharp",                            5, "\xe2\x99\xaf",                3},    /* "♯" U+0266F */
    {"shchcy",                           6, "\xd1\x89",                    2},    /* "щ" U+00449 */
    {"shcy",                             4, "\xd1\x88",                    2},    /* "ш" U+00448 */
    {"shortmid",                         8, "\xe2\x88\xa3",                3},    /* "∣" U+02223 */
    {"shortparallel",                   13, "\xe2\x88\xa5",                3},    /* "∥" U+02225 */
    {"shy",                              3, "\xc2\xad",                    2},    /* "­" U+000AD */
    {"sigma",                            5, "\xcf\x83",                    2},    /* "σ" U+003C3 */
    {"sigmaf",                           6, "\xcf\x82",                    2},    /* "ς" U+003C2 */
    {"sigmav",                           6, "\xcf\x82",                    2},    /* "ς" U+003C2 */
    {"sim",                              3, "\xe2\x88\xbc",                3},    /* "∼" U+0223C */
    {"simdot",                           6, "\xe2\xa9\xaa",                3},    /* "⩪" U+02A6A */
    {"sime",                             4, "\xe2\x89\x83",                3},    /* "≃" U+02243 */
    {"simeq",                            5, "\xe2\x89\x83",                3},    /* "≃" U+02243 */
    {"simg",                             4, "\xe2\xaa\x9e",                3},    /* "⪞" U+02A9E */
    {"simgE",                            5, "\xe2\xaa\xa0",                3},    /* "⪠" U+02AA0 */
    {"siml",                             4, "\xe2\xaa\x9d",                3},    /* "⪝" U+02A9D */
    {"simlE",                            5, "\xe2\xaa\x9f",                3},    /* "⪟" U+02A9F */
    {"simne",                            5, "\xe2\x89\x86",                3},    /* "≆" U+02246 */
    {"simplus",                          7, "\xe2\xa8\xa4",                3},    /* "⨤" U+02A24 */
    {"simrarr",                          7, "\xe2\xa5\xb2",                3},    /* "⥲" U+02972 */
    {"slarr",                            5, "\xe2\x86\x90",                3},    /* "←" U+02190 */
    {"smallsetminus",                   13, "\xe2\x88\x96",                3},    /* "∖" U+02216 */
    {"smashp",                           6, "\xe2\xa8\xb3",                3},    /* "⨳" U+02A33 */
    {"smeparsl",                         8, "\xe2\xa7\xa4",                3},    /* "⧤" U+029E4 */
    {"smid",                             4, "\xe2\x88\xa3",                3},    /* "∣" U+02223 */
    {"smile",                            5, "\xe2\x8c\xa3",                3},    /* "⌣" U+02323 */
    {"smt",                              3, "\xe2\xaa\xaa",                3},    /* "⪪" U+02AAA */
    {"smte",                             4, "\xe2\xaa\xac",                3},    /* "⪬" U+02AAC */
    {"smtes",                            5, "\xe2\xaa\xac\xef\xb8\x80",    6},    /* "⪬︀" U+02AAC U+0FE00 */
    {"softcy",                           6, "\xd1\x8c",                    2},    /* "ь" U+0044C */
    {"sol",                              3, "\x2f",                        1},    /* "/" U+0002F */
    {"solb",                             4, "\xe2\xa7\x84",                3},    /* "⧄" U+029C4 */
    {"solbar",                           6, "\xe2\x8c\xbf",                3},    /* "⌿" U+0233F */
    {"sopf",                             4, "\xf0\x9d\x95\xa4",            4},    /* "𝕤" U+1D564 */
    {"spades",                           6, "\xe2\x99\xa0",                3},    /* "♠" U+02660 */
    {"spadesuit",                        9, "\xe2\x99\xa0",                3},    /* "♠" U+02660 */
    {"spar",                             4, "\xe2\x88\xa5",                3},    /* "∥" U+02225 */
    {"sqcap",                            5, "\xe2\x8a\x93",                3},    /* "⊓" U+02293 */
    {"sqcaps",                           6, "\xe2\x8a\x93\xef\xb8\x80",    6},    /* "⊓︀" U+02293 U+0FE00 */
    {"sqcup",                            5, "\xe2\x8a\x94",                3},    /* "⊔" U+02294 */
    {"sqcups",                           6, "\xe2\x8a\x94\xef\xb8\x80",    6},    /* "⊔︀" U+02294 U+0FE00 */
    {"sqsub",                            5, "\xe2\x8a\x8f",                3},    /* "⊏" U+0228F */
    {"sqsube",                           6, "\xe2\x8a\x91",                3},    /* "⊑" U+02291 */
    {"sqsubset",                         8, "\xe2\x8a\x8f",                3},    /* "⊏" U+0228F */
    {"sqsubseteq",                      10, "\xe2\x8a\x91",                3},    /* "⊑" U+02291 */
    {"sqsup",                            5, "\xe2\x8a\x90",                3},    /* "⊐" U+02290 */
    {"sqsupe",                           6, "\xe2\x8a\x92",                3},    /* "⊒" U+02292 */
    {"sqsupset",                         8, "\xe2\x8a\x90",                3},    /* "⊐" U+02290 */
    {"sqsupseteq",                      10, "\xe2\x8a\x92",                3},    /* "⊒" U+02292 */
    {"squ",                              3, "\xe2\x96\xa1",                3},    /* "□" U+025A1 */
    {"square",                           6, "\xe2\x96\xa1",                3},    /* "□" U+025A1 */
    {"squarf",                           6, "\xe2\x96\xaa",                3},    /* "▪" U+025AA */
    {"squf",                             4, "\xe2\x96\xaa",                3},    /* "▪" U+025AA */
    {"srarr",                            5, "\xe2\x86\x92",                3},    /* "→" U+02192 */
    {"sscr",                             4, "\xf0\x9d\x93\x88",            4},    /* "𝓈" U+1D4C8 */
    {"ssetmn",                           6, "\xe2\x88\x96",                3},    /* "∖" U+02216 */
    {"ssmile",                           6, "\xe2\x8c\xa3",                3},    /* "⌣" U+02323 */
    {"sstarf",                           6, "\xe2\x8b\x86",                3},    /* "⋆" U+022C6 */
    {"star",                             4, "\xe2\x98\x86",                3},    /* "☆" U+02606 */
    {"starf",                            5, "\xe2\x98\x85",                3},    /* "★" U+02605 */
    {"straightepsilon",                 15, "\xcf\xb5",                    2},    /* "ϵ" U+003F5 */
    {"straightphi",                     11, "\xcf\x95",                    2},    /* "ϕ" U+003D5 */
    {"strns",                            5, "\xc2\xaf",                    2},    /* "¯" U+000AF */
    {"sub",                              3, "\xe2\x8a\x82",                3},    /* "⊂" U+02282 */
    {"subE",                             4, "\xe2\xab\x85",                3},    /* "⫅" U+02AC5 */
    {"subdot",                           6, "\xe2\xaa\xbd",                3},    /* "⪽" U+02ABD */
    {"sube",                             4, "\xe2\x8a\x86",                3},    /* "⊆" U+02286 */
    {"subedot",                          7, "\xe2\xab\x83",                3},    /* "⫃" U+02AC3 */
    {"submult",                          7, "\xe2\xab\x81",                3},    /* "⫁" U+02AC1 */
    {"subnE",                            5, "\xe2\xab\x8b",                3},    /* "⫋" U+02ACB */
    {"subne",                            5, "\xe2\x8a\x8a",                3},    /* "⊊" U+0228A */
    {"subplus",                          7, "\xe2\xaa\xbf",                3},    /* "⪿" U+02ABF */
    {"subrarr",                          7, "\xe2\xa5\xb9",                3},    /* "⥹" U+02979 */
    {"subset",                           6, "\xe2\x8a\x82",                3},    /* "⊂" U+02282 */
    {"subseteq",                         8, "\xe2\x8a\x86",                3},    /* "⊆" U+02286 */
    {"subseteqq",                        9, "\xe2\xab\x85",                3},    /* "⫅" U+02AC5 */
    {"subsetneq",                        9, "\xe2\x8a\x8a",                3},    /* "⊊" U+0228A */
    {"subsetneqq",                      10, "\xe2\xab\x8b",                3},    /* "⫋" U+02ACB */
    {"subsim",                           6, "\xe2\xab\x87",                3},    /* "⫇" U+02AC7 */
    {"subsub",                           6, "\xe2\xab\x95",                3},    /* "⫕" U+02AD5 */
    {"subsup",                           6, "\xe2\xab\x93",                3},    /* "⫓" U+02AD3 */
    {"succ",                             4, "\xe2\x89\xbb",                3},    /* "≻" U+0227B */
    {"succapprox",                      10, "\xe2\xaa\xb8",                3},    /* "⪸" U+02AB8 */
    {"succcurlyeq",                     11, "\xe2\x89\xbd",                3},    /* "≽" U+0227D */
    {"succeq",                           6, "\xe2\xaa\xb0",                3},    /* "⪰" U+02AB0 */
    {"succnapprox",                     11, "\xe2\xaa\xba",                3},    /* "⪺" U+02ABA */
    {"succneqq",                         8, "\xe2\xaa\xb6",                3},    /* "⪶" U+02AB6 */
    {"succnsim",                         8, "\xe2\x8b\xa9",                3},    /* "⋩" U+022E9 */
    {"succsim",                          7, "\xe2\x89\xbf",                3},    /* "≿" U+0227F */
    {"sum",                              3, "\xe2\x88\x91",                3},    /* "∑" U+02211 */
    {"sung",                             4, "\xe2\x99\xaa",                3},    /* "♪" U+0266A */
    {"sup",                              3, "\xe2\x8a\x83",                3},    /* "⊃" U+02283 */
    {"sup1",                             4, "\xc2\xb9",                    2},    /* "¹" U+000B9 */
    {"sup2",                             4, "\xc2\xb2",                    2},    /* "²" U+000B2 */
    {"sup3",                             4, "\xc2\xb3",                    2},    /* "³" U+000B3 */
    {"supE",                             4, "\xe2\xab\x86",                3},    /* "⫆" U+02AC6 */
    {"supdot",                           6, "\xe2\xaa\xbe",                3},    /* "⪾" U+02ABE */
    {"supdsub",                          7, "\xe2\xab\x98",                3},    /* "⫘" U+02AD8 */
    {"supe",                             4, "\xe2\x8a\x87",                3},    /* "⊇" U+02287 */
    {"supedot",                          7, "\xe2\xab\x84",                3},    /* "⫄" U+02AC4 */
    {"suphsol",                          7, "\xe2\x9f\x89",                3},    /* "⟉" U+027C9 */
    {"suphsub",                          7, "\xe2\xab\x97",                3},    /* "⫗" U+02AD7 */
    {"suplarr",                          7, "\xe2\xa5\xbb",                3},    /* "⥻" U+0297B */
    {"supmult",                          7, "\xe2\xab\x82",                3},    /* "⫂" U+02AC2 */
    {"supnE",                            5, "\xe2\xab\x8c",                3},    /* "⫌" U+02ACC */
    {"supne",                            5, "\xe2\x8a\x8b",                3},    /* "⊋" U+0228B */
    {"supplus",                          7, "\xe2\xab\x80",                3},    /* "⫀" U+02AC0 */
    {"supset",                           6, "\xe2\x8a\x83",                3},    /* "⊃" U+02283 */
    {"supseteq",                         8, "\xe2\x8a\x87",                3},    /* "⊇" U+02287 */
    {"supseteqq",                        9, "\xe2\xab\x86",                3},    /* "⫆" U+02AC6 */
    {"supsetneq",                        9, "\xe2\x8a\x8b",                3},    /* "⊋" U+0228B */
    {"supsetneqq",                      10, "\xe2\xab\x8c",                3},    /* "⫌" U+02ACC */
    {"supsim",                           6, "\xe2\xab\x88",                3},    /* "⫈" U+02AC8 */
    {"supsub",                           6, "\xe2\xab\x94",                3},    /* "⫔" U+02AD4 */
    {"supsup",                           6, "\xe2\xab\x96",                3},    /* "⫖" U+02AD6 */
    {"swArr",                            5, "\xe2\x87\x99",                3},    /* "⇙" U+021D9 */
    {"swarhk",                           6, "\xe2\xa4\xa6",                3},    /* "⤦" U+02926 */
    {"swarr",                            5, "\xe2\x86\x99",                3},    /* "↙" U+02199 */
    {"swarrow",                          7, "\xe2\x86\x99",                3},    /* "↙" U+02199 */
    {"swnwar",                           6, "\xe2\xa4\xaa",                3},    /* "⤪" U+0292A */
    {"szlig",                            5, "\xc3\x9f",                    2},    /* "ß" U+000DF */
    {"target",                           6, "\xe2\x8c\x96",                3},    /* "⌖" U+02316 */
    {"tau",                              3, "\xcf\x84",                    2},    /* "τ" U+003C4 */
    {"tbrk",                             4, "\xe2\x8e\xb4",                3},    /* "⎴" U+023B4 */
    {"tcaron",                           6, "\xc5\xa5",                    2},    /* "ť" U+00165 */
    {"tcedil",                           6, "\xc5\xa3",                    2},    /* "ţ" U+00163 */
    {"tcy",                              3, "\xd1\x82",                    2},    /* "т" U+00442 */
    {"tdot",                             4, "\xe2\x83\x9b",                3},    /* "⃛" U+020DB */
    {"telrec",                           6, "\xe2\x8c\x95",                3},    /* "⌕" U+02315 */
    {"tfr",                              3, "\xf0\x9d\x94\xb1",            4},    /* "𝔱" U+1D531 */
    {"there4",                           6, "\xe2\x88\xb4",                3},    /* "∴" U+02234 */
    {"therefore",                        9, "\xe2\x88\xb4",                3},    /* "∴" U+02234 */
    {"theta",                            5, "\xce\xb8",                    2},    /* "θ" U+003B8 */
    {"thetasym",                         8, "\xcf\x91",                    2},    /* "ϑ" U+003D1 */
    {"thetav",                           6, "\xcf\x91",                    2},    /* "ϑ" U+003D1 */
    {"thickapprox",                     11, "\xe2\x89\x88",                3},    /* "≈" U+02248 */
    {"thicksim",                         8, "\xe2\x88\xbc",                3},    /* "∼" U+0223C */
    {"thinsp",                           6, "\xe2\x80\x89",                3},    /* " " U+02009 */
    {"thkap",                            5, "\xe2\x89\x88",                3},    /* "≈" U+02248 */
    {"thksim",                           6, "\xe2\x88\xbc",                3},    /* "∼" U+0223C */
    {"thorn",                            5, "\xc3\xbe",                    2},    /* "þ" U+000FE */
    {"tilde",                            5, "\xcb\x9c",                    2},    /* "˜" U+002DC */
    {"times",                            5, "\xc3\x97",                    2},    /* "×" U+000D7 */
    {"timesb",                           6, "\xe2\x8a\xa0",                3},    /* "⊠" U+022A0 */
    {"timesbar",                         8, "\xe2\xa8\xb1",                3},    /* "⨱" U+02A31 */
    {"timesd",                           6, "\xe2\xa8\xb0",                3},    /* "⨰" U+02A30 */
    {"tint",                             4, "\xe2\x88\xad",                3},    /* "∭" U+0222D */
    {"toea",                             4, "\xe2\xa4\xa8",                3},    /* "⤨" U+02928 */
    {"top",                              3, "\xe2\x8a\xa4",                3},    /* "⊤" U+022A4 */
    {"topbot",                           6, "\xe2\x8c\xb6",                3},    /* "⌶" U+02336 */
    {"topcir",                           6, "\xe2\xab\xb1",                3},    /* "⫱" U+02AF1 */
    {"topf",                             4, "\xf0\x9d\x95\xa5",            4},    /* "𝕥" U+1D565 */
    {"topfork",                          7, "\xe2\xab\x9a",                3},    /* "⫚" U+02ADA */
    {"tosa",                             4, "\xe2\xa4\xa9",                3},    /* "⤩" U+02929 */
    {"tprime",                           6, "\xe2\x80\xb4",                3},    /* "‴" U+02034 */
    {"trade",                            5, "\xe2\x84\xa2",                3},    /* "™" U+02122 */
    {"triangle",                         8, "\xe2\x96\xb5",                3},    /* "▵" U+025B5 */
    {"triangledown",                    12, "\xe2\x96\xbf",                3},    /* "▿" U+025BF */
    {"triangleleft",                    12, "\xe2\x97\x83",                3},    /* "◃" U+025C3 */
    {"trianglelefteq",                  14, "\xe2\x8a\xb4",                3},    /* "⊴" U+022B4 */
    {"triangleq",                        9, "\xe2\x89\x9c",                3},    /* "≜" U+0225C */
    {"triangleright",                   13, "\xe2\x96\xb9",                3},    /* "▹" U+025B9 */
    {"trianglerighteq",                 15, "\xe2\x8a\xb5",                3},    /* "⊵" U+022B5 */
    {"tridot",                           6, "\xe2\x97\xac",                3},    /* "◬" U+025EC */
    {"trie",                             4, "\xe2\x89\x9c",                3},    /* "≜" U+0225C */
    {"triminus",                         8, "\xe2\xa8\xba",                3},    /* "⨺" U+02A3A */
    {"triplus",                          7, "\xe2\xa8\xb9",                3},    /* "⨹" U+02A39 */
    {"trisb",                            5, "\xe2\xa7\x8d",                3},    /* "⧍" U+029CD */
    {"tritime",                          7, "\xe2\xa8\xbb",                3},    /* "⨻" U+02A3B */
    {"trpezium",                         8, "\xe2\x8f\xa2",                3},    /* "⏢" U+023E2 */
    {"tscr",                             4, "\xf0\x9d\x93\x89",            4},    /* "𝓉" U+1D4C9 */
    {"tscy",                             4, "\xd1\x86",                    2},    /* "ц" U+00446 */
    {"tshcy",                            5, "\xd1\x9b",                    2},    /* "ћ" U+0045B */
    {"tstrok",                           6, "\xc5\xa7",                    2},    /* "ŧ" U+00167 */
    {"twixt",                            5, "\xe2\x89\xac",                3},    /* "≬" U+0226C */
    {"twoheadleftarrow",                16, "\xe2\x86\x9e",                3},    /* "↞" U+0219E */
    {"twoheadrightarrow",               17, "\xe2\x86\xa0",                3},    /* "↠" U+021A0 */
    {"uArr",                             4, "\xe2\x87\x91",                3},    /* "⇑" U+021D1 */
    {"uHar",                             4, "\xe2\xa5\xa3",                3},    /* "⥣" U+02963 */
    {"uacute",                           6, "\xc3\xba",                    2},    /* "ú" U+000FA */
    {"uarr",                             4, "\xe2\x86\x91",                3},    /* "↑" U+02191 */
    {"ubrcy",                            5, "\xd1\x9e",                    2},    /* "ў" U+0045E */
    {"ubreve",                           6, "\xc5\xad",                    2},    /* "ŭ" U+0016D */
    {"ucirc",                            5, "\xc3\xbb",                    2},    /* "û" U+000FB */
    {"ucy",                              3, "\xd1\x83",                    2},    /* "у" U+00443 */
    {"udarr",                            5, "\xe2\x87\x85",                3},    /* "⇅" U+021C5 */
    {"udblac",                           6, "\xc5\xb1",                    2},    /* "ű" U+00171 */
    {"udhar",                            5, "\xe2\xa5\xae",                3},    /* "⥮" U+0296E */
    {"ufisht",                           6, "\xe2\xa5\xbe",                3},    /* "⥾" U+0297E */
    {"ufr",                              3, "\xf0\x9d\x94\xb2",            4},    /* "𝔲" U+1D532 */
    {"ugrave",                           6, "\xc3\xb9",                    2},    /* "ù" U+000F9 */
    {"uharl",                            5, "\xe2\x86\xbf",                3},    /* "↿" U+021BF */
    {"uharr",                            5, "\xe2\x86\xbe",                3},    /* "↾" U+021BE */
    {"uhblk",                            5, "\xe2\x96\x80",                3},    /* "▀" U+02580 */
    {"ulcorn",                           6, "\xe2\x8c\x9c",                3},    /* "⌜" U+0231C */
    {"ulcorner",                         8, "\xe2\x8c\x9c",                3},    /* "⌜" U+0231C */
    {"ulcrop",                           6, "\xe2\x8c\x8f",                3},    /* "⌏" U+0230F */
    {"ultri",                            5, "\xe2\x97\xb8",                3},    /* "◸" U+025F8 */
    {"umacr",                            5, "\xc5\xab",                    2},    /* "ū" U+0016B */
    {"uml",                              3, "\xc2\xa8",                    2},    /* "¨" U+000A8 */
    {"uogon",                            5, "\xc5\xb3",                    2},    /* "ų" U+00173 */
    {"uopf",                             4, "\xf0\x9d\x95\xa6",            4},    /* "𝕦" U+1D566 */
    {"uparrow",                          7, "\xe2\x86\x91",                3},    /* "↑" U+02191 */
    {"updownarrow",                     11, "\xe2\x86\x95",                3},    /* "↕" U+02195 */
    {"upharpoonleft",                   13, "\xe2\x86\xbf",                3},    /* "↿" U+021BF */
    {"upharpoonright",                  14, "\xe2\x86\xbe",                3},    /* "↾" U+021BE */
    {"uplus",                            5, "\xe2\x8a\x8e",                3},    /* "⊎" U+0228E */
    {"upsi",                             4, "\xcf\x85",                    2},    /* "υ" U+003C5 */
    {"upsih",                            5, "\xcf\x92",                    2},    /* "ϒ" U+003D2 */
    {"upsilon",                          7, "\xcf\x85",                    2},    /* "υ" U+003C5 */
    {"upuparrows",                      10, "\xe2\x87\x88",                3},    /* "⇈" U+021C8 */
    {"urcorn",                           6, "\xe2\x8c\x9d",                3},    /* "⌝" U+0231D */
    {"urcorner",                         8, "\xe2\x8c\x9d",                3},    /* "⌝" U+0231D */
    {"urcrop",                           6, "\xe2\x8c\x8e",                3},    /* "⌎" U+0230E */
    {"uring",                            5, "\xc5\xaf",                    2},    /* "ů" U+0016F */
    {"urtri",                            5, "\xe2\x97\xb9",                3},    /* "◹" U+025F9 */
    {"uscr",                             4, "\xf0\x9d\x93\x8a",            4},    /* "𝓊" U+1D4CA */
    {"utdot",                            5, "\xe2\x8b\xb0",                3},    /* "⋰" U+022F0 */
    {"utilde",                           6, "\xc5\xa9",                    2},    /* "ũ" U+00169 */
    {"utri",                             4, "\xe2\x96\xb5",                3},    /* "▵" U+025B5 */
    {"utrif",                            5, "\xe2\x96\xb4",                3},    /* "▴" U+025B4 */
    {"uuarr",                            5, "\xe2\x87\x88",                3},    /* "⇈" U+021C8 */
    {"uuml",                             4, "\xc3\xbc",                    2},    /* "ü" U+000FC */
    {"uwangle",                          7, "\xe2\xa6\xa7",                3},    /* "⦧" U+029A7 */
    {"vArr",                             4, "\xe2\x87\x95",                3},    /* "⇕" U+021D5 */
    {"vBar",                             4, "\xe2\xab\xa8",                3},    /* "⫨" U+02AE8 */
    {"vBarv",                            5, "\xe2\xab\xa9",                3},    /* "⫩" U+02AE9 */
    {"vDash",                            5, "\xe2\x8a\xa8",                3},    /* "⊨" U+022A8 */
    {"vangrt",                           6, "\xe2\xa6\x9c",                3},    /* "⦜" U+0299C */
    {"varepsilon",                      10, "\xcf\xb5",                    2},    /* "ϵ" U+003F5 */
    {"varkappa",                         8, "\xcf\xb0",                    2},    /* "ϰ" U+003F0 */
    {"varnothing",                      10, "\xe2\x88\x85",                3},    /* "∅" U+02205 */
    {"varphi",                           6, "\xcf\x95",                    2},    /* "ϕ" U+003D5 */
    {"varpi",                            5, "\xcf\x96",                    2},    /* "ϖ" U+003D6 */
    {"varpropto",                        9, "\xe2\x88\x9d",                3},    /* "∝" U+0221D */
    {"varr",                             4, "\xe2\x86\x95",                3},    /* "↕" U+02195 */
    {"varrho",                           6, "\xcf\xb1",                    2},    /* "ϱ" U+003F1 */
    {"varsigma",                         8, "\xcf\x82",                    2},    /* "ς" U+003C2 */
    {"varsubsetneq",                    12, "\xe2\x8a\x8a\xef\xb8\x80",    6},    /* "⊊︀" U+0228A U+0FE00 */
    {"varsubsetneqq",                   13, "\xe2\xab\x8b\xef\xb8\x80",    6},    /* "⫋︀" U+02ACB U+0FE00 */
    {"varsupsetneq",                    12, "\xe2\x8a\x8b\xef\xb8\x80",    6},    /* "⊋︀" U+0228B U+0FE00 */
    {"varsupsetneqq",                   13, "\xe2\xab\x8c\xef\xb8\x80",    6},    /* "⫌︀" U+02ACC U+0FE00 */
    {"vartheta",                         8, "\xcf\x91",                    2},    /* "ϑ" U+003D1 */
    {"vartriangleleft",                 15, "\xe2\x8a\xb2",                3},    /* "⊲" U+022B2 */
    {"vartriangleright",                16, "\xe2\x8a\xb3",                3},    /* "⊳" U+022B3 */
    {"vcy",                              3, "\xd0\xb2",                    2},    /* "в" U+00432 */
    {"vdash",                            5, "\xe2\x8a\xa2",                3},    /* "⊢" U+022A2 */
    {"vee",                              3, "\xe2\x88\xa8",                3},    /* "∨" U+02228 */
    {"veebar",                           6, "\xe2\x8a\xbb",                3},    /* "⊻" U+022BB */
    {"veeeq",                            5, "\xe2\x89\x9a",                3},    /* "≚" U+0225A */
    {"vellip",                           6, "\xe2\x8b\xae",                3},    /* "⋮" U+022EE */
    {"verbar",                           6, "\x7c",                        1},    /* "|" U+0007C */
    {"vert",                             4, "\x7c",                        1},    /* "|" U+0007C */
    {"vfr",                              3, "\xf0\x9d\x94\xb3",            4},    /* "𝔳" U+1D533 */
    {"vltri",                            5, "\xe2\x8a\xb2",                3},    /* "⊲" U+022B2 */
    {"vnsub",                            5, "\xe2\x8a\x82\xe2\x83\x92",    6},    /* "⊂⃒" U+02282 U+020D2 */
    {"vnsup",                            5, "\xe2\x8a\x83\xe2\x83\x92",    6},    /* "⊃⃒" U+02283 U+020D2 */
    {"vopf",                             4, "\xf0\x9d\x95\xa7",            4},    /* "𝕧" U+1D567 */
    {"vprop",                            5, "\xe2\x88\x9d",                3},    /* "∝" U+0221D */
    {"vrtri",                            5, "\xe2\x8a\xb3",                3},    /* "⊳" U+022B3 */
    {"vscr",                             4, "\xf0\x9d\x93\x8b",            4},    /* "𝓋" U+1D4CB */
    {"vsubnE",                           6, "\xe2\xab\x8b\xef\xb8\x80",    6},    /* "⫋︀" U+02ACB U+0FE00 */
    {"vsubne",                           6, "\xe2\x8a\x8a\xef\xb8\x80",    6},    /* "⊊︀" U+0228A U+0FE00 */
    {"vsupnE",                           6, "\xe2\xab\x8c\xef\xb8\x80",    6},    /* "⫌︀" U+02ACC U+0FE00 */
    {"vsupne",                           6, "\xe2\x8a\x8b\xef\xb8\x80",    6},    /* "⊋︀" U+0228B U+0FE00 */
    {"vzigzag",                          7, "\xe2\xa6\x9a",                3},    /* "⦚" U+0299A */
    {"wcirc",                            5, "\xc5\xb5",                    2},    /* "ŵ" U+00175 */
    {"wedbar",                           6, "\xe2\xa9\x9f",                3},    /* "⩟" U+02A5F */
    {"wedge",                            5, "\xe2\x88\xa7",                3},    /* "∧" U+02227 */
    {"wedgeq",                           6, "\xe2\x89\x99",                3},    /* "≙" U+02259 */
    {"weierp",                           6, "\xe2\x84\x98",                3},    /* "℘" U+02118 */
    {"wfr",                              3, "\xf0\x9d\x94\xb4",            4},    /* "𝔴" U+1D534 */
    {"wopf",                             4, "\xf0\x9d\x95\xa8",            4},    /* "𝕨" U+1D568 */
    {"wp",                               2, "\xe2\x84\x98",                3},    /* "℘" U+02118 */
    {"wr",                               2, "\xe2\x89\x80",                3},    /* "≀" U+02240 */
    {"wreath",                           6, "\xe2\x89\x80",                3},    /* "≀" U+02240 */
    {"wscr",                             4, "\xf0\x9d\x93\x8c",            4},    /* "𝓌" U+1D4CC */
    {"xcap",                             4, "\xe2\x8b\x82",                3},    /* "⋂" U+022C2 */
    {"xcirc",                            5, "\xe2\x97\xaf",                3},    /* "◯" U+025EF */
    {"xcup",                             4, "\xe2\x8b\x83",                3},    /* "⋃" U+022C3 */
    {"xdtri",                            5, "\xe2\x96\xbd",                3},    /* "▽" U+025BD */
    {"xfr",                              3, "\xf0\x9d\x94\xb5",            4},    /* "𝔵" U+1D535 */
    {"xhArr",                            5, "\xe2\x9f\xba",                3},    /* "⟺" U+027FA */
    {"xharr",                            5, "\xe2\x9f\xb7",                3},    /* "⟷" U+027F7 */
    {"xi",                               2, "\xce\xbe",                    2},    /* "ξ" U+003BE */
    {"xlArr",                            5, "\xe2\x9f\xb8",                3},    /* "⟸" U+027F8 */
    {"xlarr",                            5, "\xe2\x9f\xb5",                3},    /* "⟵" U+027F5 */
    {"xmap",                             4, "\xe2\x9f\xbc",                3},    /* "⟼" U+027FC */
    {"xnis",                             4, "\xe2\x8b\xbb",                3},    /* "⋻" U+022FB */
    {"xodot",                            5, "\xe2\xa8\x80",                3},    /* "⨀" U+02A00 */
    {"xopf",                             4, "\xf0\x9d\x95\xa9",            4},    /* "𝕩" U+1D569 */
    {"xoplus",                           6, "\xe2\xa8\x81",                3},    /* "⨁" U+02A01 */
    {"xotime",                           6, "\xe2\xa8\x82",                3},    /* "⨂" U+02A02 */
    {"xrArr",                            5, "\xe2\x9f\xb9",                3},    /* "⟹" U+027F9 */
    {"xrarr",                            5, "\xe2\x9f\xb6",                3},    /* "⟶" U+027F6 */
    {"xscr",                             4, "\xf0\x9d\x93\x8d",            4},    /* "𝓍" U+1D4CD */
    {"xsqcup",                           6, "\xe2\xa8\x86",                3},    /* "⨆" U+02A06 */
    {"xuplus",                           6, "\xe2\xa8\x84",                3},    /* "⨄" U+02A04 */
    {"xutri",                            5, "\xe2\x96\xb3",                3},    /* "△" U+025B3 */
    {"xvee",                             4, "\xe2\x8b\x81",                3},    /* "⋁" U+022C1 */
    {"xwedge",                           6, "\xe2\x8b\x80",                3},    /* "⋀" U+022C0 */
    {"yacute",                           6, "\xc3\xbd",                    2},    /* "ý" U+000FD */
    {"yacy",                             4, "\xd1\x8f",                    2},    /* "я" U+0044F */
    {"ycirc",                            5, "\xc5\xb7",                    2},    /* "ŷ" U+00177 */
    {"ycy",                              3, "\xd1\x8b",                    2},    /* "ы" U+0044B */
    {"yen",                              3, "\xc2\xa5",                    2},    /* "¥" U+000A5 */
    {"yfr",                              3, "\xf0\x9d\x94\xb6",            4},    /* "𝔶" U+1D536 */
    {"yicy",                             4, "\xd1\x97",                    2},    /* "ї" U+00457 */
    {"yopf",                             4, "\xf0\x9d\x95\xaa",            4},    /* "𝕪" U+1D56A */
    {"yscr",                             4, "\xf0\x9d\x93\x8e",            4},    /* "𝓎" U+1D4CE */
    {"yucy",                             4, "\xd1\x8e",                    2},    /* "ю" U+0044E */
    {"yuml",                             4, "\xc3\xbf",                    2},    /* "ÿ" U+000FF */
    {"zacute",                           6, "\xc5\xba",                    2},    /* "ź" U+0017A */
    {"zcaron",                           6, "\xc5\xbe",                    2},    /* "ž" U+0017E */
    {"zcy",                              3, "\xd0\xb7",                    2},    /* "з" U+00437 */
    {"zdot",                             4, "\xc5\xbc",                    2},    /* "ż" U+0017C */
    {"zeetrf",                           6, "\xe2\x84\xa8",                3},    /* "ℨ" U+02128 */
    {"zeta",                             4, "\xce\xb6",                    2},    /* "ζ" U+003B6 */
    {"zfr",                              3, "\xf0\x9d\x94\xb7",            4},    /* "𝔷" U+1D537 */
    {"zhcy",                             4, "\xd0\xb6",                    2},    /* "ж" U+00436 */
    {"zigrarr",                          7, "\xe2\x87\x9d",                3},    /* "⇝" U+021DD */
    {"zopf",                             4, "\xf0\x9d\x95\xab",            4},    /* "𝕫" U+1D56B */
    {"zscr",                             4, "\xf0\x9d\x93\x8f",            4},    /* "𝓏" U+1D4CF */
    {"zwj",                              3, "\xe2\x80\x8d",                3},    /* "‍" U+0200D */
    {"zwnj",                             4, "\xe2\x80\x8c",                3},    /* "‌" U+0200C */

    {NULL,                               0, "",                            0}
};

static const namedEntity_t namedLegacyEntities[] = {
    {"AElig",                            5, "\xc3\x86",                    2},    /* "Æ" U+000C6  */
    {"AMP",                              3, "\x26",                        1},    /* "&" U+00026  */
    {"Aacute",                           6, "\xc3\x81",                    2},    /* "Á" U+000C1  */
    {"Acirc",                            5, "\xc3\x82",                    2},    /* "Â" U+000C2  */
    {"Agrave",                           6, "\xc3\x80",                    2},    /* "À" U+000C0  */
    {"Aring",                            5, "\xc3\x85",                    2},    /* "Å" U+000C5  */
    {"Atilde",                           6, "\xc3\x83",                    2},    /* "Ã" U+000C3  */
    {"Auml",                             4, "\xc3\x84",                    2},    /* "Ä" U+000C4  */
    {"COPY",                             4, "\xc2\xa9",                    2},    /* "©" U+000A9  */
    {"Ccedil",                           6, "\xc3\x87",                    2},    /* "Ç" U+000C7  */
    {"ETH",                              3, "\xc3\x90",                    2},    /* "Ð" U+000D0  */
    {"Eacute",                           6, "\xc3\x89",                    2},    /* "É" U+000C9  */
    {"Ecirc",                            5, "\xc3\x8a",                    2},    /* "Ê" U+000CA  */
    {"Egrave",                           6, "\xc3\x88",                    2},    /* "È" U+000C8  */
    {"Euml",                             4, "\xc3\x8b",                    2},    /* "Ë" U+000CB  */
    {"GT",                               2, "\x3e",                        1},    /* ">" U+0003E  */
    {"Iacute",                           6, "\xc3\x8d",                    2},    /* "Í" U+000CD  */
    {"Icirc",                            5, "\xc3\x8e",                    2},    /* "Î" U+000CE  */
    {"Igrave",                           6, "\xc3\x8c",                    2},    /* "Ì" U+000CC  */
    {"Iuml",                             4, "\xc3\x8f",                    2},    /* "Ï" U+000CF  */
    {"LT",                               2, "\x3c",                        1},    /* "<" U+0003C  */
    {"Ntilde",                           6, "\xc3\x91",                    2},    /* "Ñ" U+000D1  */
    {"Oacute",                           6, "\xc3\x93",                    2},    /* "Ó" U+000D3  */
    {"Ocirc",                            5, "\xc3\x94",                    2},    /* "Ô" U+000D4  */
    {"Ograve",                           6, "\xc3\x92",                    2},    /* "Ò" U+000D2  */
    {"Oslash",                           6, "\xc3\x98",                    2},    /* "Ø" U+000D8  */
    {"Otilde",                           6, "\xc3\x95",                    2},    /* "Õ" U+000D5  */
    {"Ouml",                             4, "\xc3\x96",                    2},    /* "Ö" U+000D6  */
    {"QUOT",                             4, "\x22",                        1},    /* """ U+00022  */
    {"REG",                              3, "\xc2\xae",                    2},    /* "®" U+000AE  */
    {"THORN",                            5, "\xc3\x9e",                    2},    /* "Þ" U+000DE  */
    {"Uacute",                           6, "\xc3\x9a",                    2},    /* "Ú" U+000DA  */
    {"Ucirc",                            5, "\xc3\x9b",                    2},    /* "Û" U+000DB  */
    {"Ugrave",                           6, "\xc3\x99",                    2},    /* "Ù" U+000D9  */
    {"Uuml",                             4, "\xc3\x9c",                    2},    /* "Ü" U+000DC  */
    {"Yacute",                           6, "\xc3\x9d",                    2},    /* "Ý" U+000DD  */
    {"aacute",                           6, "\xc3\xa1",                    2},    /* "á" U+000E1  */
    {"acirc",                            5, "\xc3\xa2",                    2},    /* "â" U+000E2  */
    {"acute",                            5, "\xc2\xb4",                    2},    /* "´" U+000B4  */
    {"aelig",                            5, "\xc3\xa6",                    2},    /* "æ" U+000E6  */
    {"agrave",                           6, "\xc3\xa0",                    2},    /* "à" U+000E0  */
    {"amp",                              3, "\x26",                        1},    /* "&" U+00026  */
    {"aring",                            5, "\xc3\xa5",                    2},    /* "å" U+000E5  */
    {"atilde",                           6, "\xc3\xa3",                    2},    /* "ã" U+000E3  */
    {"auml",                             4, "\xc3\xa4",                    2},    /* "ä" U+000E4  */
    {"brvbar",                           6, "\xc2\xa6",                    2},    /* "¦" U+000A6  */
    {"ccedil",                           6, "\xc3\xa7",                    2},    /* "ç" U+000E7  */
    {"cedil",                            5, "\xc2\xb8",                    2},    /* "¸" U+000B8  */
    {"cent",                             4, "\xc2\xa2",                    2},    /* "¢" U+000A2  */
    {"copy",                             4, "\xc2\xa9",                    2},    /* "©" U+000A9  */
    {"curren",                           6, "\xc2\xa4",                    2},    /* "¤" U+000A4  */
    {"deg",                              3, "\xc2\xb0",                    2},    /* "°" U+000B0  */
    {"divide",                           6, "\xc3\xb7",                    2},    /* "÷" U+000F7  */
    {"eacute",                           6, "\xc3\xa9",                    2},    /* "é" U+000E9  */
    {"ecirc",                            5, "\xc3\xaa",                    2},    /* "ê" U+000EA  */
    {"egrave",                           6, "\xc3\xa8",                    2},    /* "è" U+000E8  */
    {"eth",                              3, "\xc3\xb0",                    2},    /* "ð" U+000F0  */
    {"euml",                             4, "\xc3\xab",                    2},    /* "ë" U+000EB  */
    {"frac12",                           6, "\xc2\xbd",                    2},    /* "½" U+000BD  */
    {"frac14",                           6, "\xc2\xbc",                    2},    /* "¼" U+000BC  */
    {"frac34",                           6, "\xc2\xbe",                    2},    /* "¾" U+000BE  */
    {"gt",                               2, "\x3e",                        1},    /* ">" U+0003E  */
    {"iacute",                           6, "\xc3\xad",                    2},    /* "í" U+000ED  */
    {"icirc",                            5, "\xc3\xae",                    2},    /* "î" U+000EE  */
    {"iexcl",                            5, "\xc2\xa1",                    2},    /* "¡" U+000A1  */
    {"igrave",                           6, "\xc3\xac",                    2},    /* "ì" U+000EC  */
    {"iquest",                           6, "\xc2\xbf",                    2},    /* "¿" U+000BF  */
    {"iuml",                             4, "\xc3\xaf",                    2},    /* "ï" U+000EF  */
    {"laquo",                            5, "\xc2\xab",                    2},    /* "«" U+000AB  */
    {"lt",                               2, "\x3c",                        1},    /* "<" U+0003C  */
    {"macr",                             4, "\xc2\xaf",                    2},    /* "¯" U+000AF  */
    {"micro",                            5, "\xc2\xb5",                    2},    /* "µ" U+000B5  */
    {"middot",                           6, "\xc2\xb7",                    2},    /* "·" U+000B7  */
    {"nbsp",                             4, "\xc2\xa0",                    2},    /* " " U+000A0  */
    {"not",                              3, "\xc2\xac",                    2},    /* "¬" U+000AC  */
    {"ntilde",                           6, "\xc3\xb1",                    2},    /* "ñ" U+000F1  */
    {"oacute",                           6, "\xc3\xb3",                    2},    /* "ó" U+000F3  */
    {"ocirc",                            5, "\xc3\xb4",                    2},    /* "ô" U+000F4  */
    {"ograve",                           6, "\xc3\xb2",                    2},    /* "ò" U+000F2  */
    {"ordf",                             4, "\xc2\xaa",                    2},    /* "ª" U+000AA  */
    {"ordm",                             4, "\xc2\xba",                    2},    /* "º" U+000BA  */
    {"oslash",                           6, "\xc3\xb8",                    2},    /* "ø" U+000F8  */
    {"otilde",                           6, "\xc3\xb5",                    2},    /* "õ" U+000F5  */
    {"ouml",                             4, "\xc3\xb6",                    2},    /* "ö" U+000F6  */
    {"para",                             4, "\xc2\xb6",                    2},    /* "¶" U+000B6  */
    {"plusmn",                           6, "\xc2\xb1",                    2},    /* "±" U+000B1  */
    {"pound",                            5, "\xc2\xa3",                    2},    /* "£" U+000A3  */
    {"quot",                             4, "\x22",                        1},    /* """ U+00022  */
    {"raquo",                            5, "\xc2\xbb",                    2},    /* "»" U+000BB  */
    {"reg",                              3, "\xc2\xae",                    2},    /* "®" U+000AE  */
    {"sect",                             4, "\xc2\xa7",                    2},    /* "§" U+000A7  */
    {"shy",                              3, "\xc2\xad",                    2},    /* "­" U+000AD  */
    {"sup1",                             4, "\xc2\xb9",                    2},    /* "¹" U+000B9  */
    {"sup2",                             4, "\xc2\xb2",                    2},    /* "²" U+000B2  */
    {"sup3",                             4, "\xc2\xb3",                    2},    /* "³" U+000B3  */
    {"szlig",                            5, "\xc3\x9f",                    2},    /* "ß" U+000DF  */
    {"thorn",                            5, "\xc3\xbe",                    2},    /* "þ" U+000FE  */
    {"times",                            5, "\xc3\x97",                    2},    /* "×" U+000D7  */
    {"uacute",                           6, "\xc3\xba",                    2},    /* "ú" U+000FA  */
    {"ucirc",                            5, "\xc3\xbb",                    2},    /* "û" U+000FB  */
    {"ugrave",                           6, "\xc3\xb9",                    2},    /* "ù" U+000F9  */
    {"uml",                              3, "\xc2\xa8",                    2},    /* "¨" U+000A8  */
    {"uuml",                             4, "\xc3\xbc",                    2},    /* "ü" U+000FC  */
    {"yacute",                           6, "\xc3\xbd",                    2},    /* "ý" U+000FD  */
    {"yen",                              3, "\xc2\xa5",                    2},    /* "¥" U+000A5  */
    {"yuml",                             4, "\xc3\xbf",                    2},    /* "ÿ" U+000FF  */

    {NULL,                               0, "",                            0}
};

static bool InitOnce(void) {
    size_t i;
    char   lastChar;

    for (i = 0u, lastChar = 0; namedEntities[i].name != NULL; i++) {
        char firstChar = *namedEntities[i].name;

        if (lastChar != firstChar) {
            entityIndexTable[UCHAR(firstChar)] = i+1;
            lastChar = firstChar;
        }
    }

    for (i = 0u, lastChar = 0; namedLegacyEntities[i].name != NULL; i++) {
        char firstChar = *namedLegacyEntities[i].name;

        if (lastChar != firstChar) {
            legacyEntityIndexTable[UCHAR(firstChar)] = i+1;
            lastChar = firstChar;
        }
    }

    return NS_TRUE;
}

static size_t
EntityDecode(const char *entity, ssize_t length, bool *needEncodePtr, char *outPtr, const char **toParse)
{
    size_t decoded = 0u;

    NS_NONNULL_ASSERT(entity != NULL);
    NS_NONNULL_ASSERT(outPtr != NULL);
    NS_NONNULL_ASSERT(needEncodePtr != NULL);
    NS_NONNULL_ASSERT(toParse != NULL);

    NS_INIT_ONCE(InitOnce);

    assert(length == 0 || *entity != '\0');

    /*
     * Handle numeric entities.
     */
    if (*entity == '#' && length > 0) {
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
            decoded = (size_t)ToUTF8(value, outPtr);

            Ns_Log(Debug, "entity decode: code point %.2lx %.2lx "
                   "corresponds to %ld UTF-8 characters",
                   ((value >> 8) & 0xff), (value & 0xff), decoded);

            if (value > 127) {
                *needEncodePtr = NS_TRUE;
            }
        } else {
            /*
             * ASCII device control characters should not be present in HTML.
             */
            Ns_Log(Notice, "entity decode: ignore numeric entity with value %ld", value);
        }
        *toParse = entity + length;
    } else {
        /*
         * Named entities.
         */
        bool   found = NS_FALSE;
        char   firstCharOfEntity = *entity;
        size_t i = entityIndexTable[UCHAR(firstCharOfEntity)];

        if (length > 0 && i > 0) {
            char   secondCharOfEntity = *(entity + 1);
            size_t len = (size_t)length;

            for (--i; namedEntities[i].name != NULL
                     && firstCharOfEntity == *namedEntities[i].name;
                 i++) {

                if (len == namedEntities[i].length
                    && secondCharOfEntity == *(namedEntities[i].name + 1)
                    && strncmp(entity, namedEntities[i].name, len) == 0
                    ) {
                    found = NS_TRUE;
                    memcpy(outPtr, namedEntities[i].value, namedEntities[i].outputLength);
                    decoded = namedEntities[i].outputLength;
                    *toParse = entity + length;
                    break;
                }
            }
        } else {
            i = legacyEntityIndexTable[UCHAR(firstCharOfEntity)];
            if (i > 0) {
                char secondCharOfEntity = *(entity + 1);
                size_t len;

                assert(length < 0);

                len = (size_t)(-length);

                for (--i; namedLegacyEntities[i].name != NULL
                         && firstCharOfEntity == *namedLegacyEntities[i].name;
                     i++) {
                    /*fprintf(stderr, "[%lu] legacy entity: first %c second %c compare with table entry 2nd %c len %lu full comparison %d\n",
                            i, firstCharOfEntity, secondCharOfEntity, *(namedLegacyEntities[i].name + 1),  namedLegacyEntities[i].length,
                            (length >= namedLegacyEntities[i].length && secondCharOfEntity == *(namedLegacyEntities[i].name + 1))
                            );*/
                    if (len >= namedLegacyEntities[i].length
                        && secondCharOfEntity == *(namedLegacyEntities[i].name + 1)
                        && (strncmp(entity, namedLegacyEntities[i].name, namedLegacyEntities[i].length) == 0)
                        ) {
                        found = NS_TRUE;
                        memcpy(outPtr, namedLegacyEntities[i].value, namedLegacyEntities[i].outputLength);
                        decoded = namedLegacyEntities[i].outputLength;
                        *toParse = entity + namedLegacyEntities[i].length - 1;
                        break;
                    }
                }
            }
        }

        if (!found) {
            Ns_Log(Debug, "ignore unknown named entity '%s'", entity);
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
    NS_NONNULL_ASSERT(lengthPtr != NULL);

    /*
     * Advance past the first '&' so we can check for a second
     *  (i.e. to handle "ben&jerry&nbsp;")
     */
    if (*word == '&') {
        word++;
    }
    start = word;

#if 0
    word = strpbrk(word, " ;&");
    if (word != NULL) {
        *lengthPtr = (size_t)(word - start);

        return (*word == ';');
    } else {
        return NS_FALSE;
    }
    return result;
#else
    while((*word != '\0') && (*word != ' ') && (*word != ';') && (*word != '&')) {
        word++;
    }
    *lengthPtr = (size_t)(word - start);

    return (*word == ';');
#endif
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
NsTclStripHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
        TCL_SIZE_T  htmlLength;
        const char *htmlString = Tcl_GetStringFromObj(htmlObj, &htmlLength);
        const char *endOfString;
        bool        intag;     /* flag to see if are we inside a tag */
        bool        incomment; /* flag to see if we are inside a comment */
        const char *inPtr;     /* moving pointer to input string */
        bool        needEncode;
        Tcl_DString outputDs, *outputDsPtr = &outputDs;

        /*
         * Remember the endPosition for dealing with the length.
         */
        endOfString = htmlString + htmlLength;

        Tcl_DStringInit(outputDsPtr);
        Tcl_DStringSetLength(outputDsPtr, (TCL_SIZE_T)htmlLength + 1);
        Tcl_DStringSetLength(outputDsPtr, 0);

        inPtr      = htmlString;
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
                    /*
                     * Starting an entity.
                     */
                    char      *outPtr;    /* moving pointer to output string */
                    size_t     entityLength = 0u, decoded = 0u;
                    TCL_SIZE_T oldLength;

                    oldLength = outputDsPtr->length;
                    Tcl_DStringSetLength(outputDsPtr, oldLength + (TCL_SIZE_T)entityLength + 6);
                    Tcl_DStringSetLength(outputDsPtr, oldLength);

                    outPtr = outputDsPtr->string + oldLength;
                    // ns_striphtml "a&lt;b"
                    if (likely(WordEndsInSemi(inPtr, &entityLength))) {
                        /*
                         * Regular entity candidate, ends with a semicolon. In
                         * case, decoded > 0, it was a registered entity.
                         */
                        decoded = EntityDecode(inPtr + 1u, (ssize_t)entityLength, &needEncode, outPtr, &inPtr);
                    }

                    if (unlikely(decoded == 0)) {
                        decoded = EntityDecode(inPtr + 1u, - (endOfString - (inPtr + 1u)), &needEncode, outPtr, &inPtr);
                    }
                    if (unlikely(decoded == 0)) {
                        /*
                         * Copy ampersand literally;
                         */
                        Ns_DStringNAppend(outputDsPtr, "&", 1);

                    } else {
                        Tcl_DStringSetLength(outputDsPtr, oldLength + (TCL_SIZE_T)decoded);
                    }
                    Ns_Log(Debug, "...... after entity inptr '%c' intag %d incomment %d string <%s> needEncode %d",
                           *inPtr, intag, incomment, inPtr, needEncode);
                } else {
                    /*
                     * Plain Text output
                     */
                    Ns_DStringNAppend(outputDsPtr, inPtr, 1);
                }

            } else {
                /*
                 * Must be intag
                 */
            }
            ++inPtr;
        }

        /*
         * Make sure the output string is NUL termeinated
         */
        *(outputDsPtr->string + outputDsPtr->length) = '\0';

        if (needEncode) {
            Tcl_DString ds;
            //fprintf(stderr, "needEncode\n");

            (void)Tcl_ExternalToUtfDString(Ns_GetCharsetEncoding("utf-8"),
                                           outputDsPtr->string, outputDsPtr->length, &ds);
            Tcl_DStringResult(interp, &ds);
            Tcl_DStringFree(outputDsPtr);
        } else {
            Tcl_DStringResult(interp, outputDsPtr);
        }
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
NsTclParseHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
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
