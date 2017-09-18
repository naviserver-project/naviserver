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
 *	Take text and make it safe for HTML.
 */

#include "nsd.h"

/*
 * Static functions defined in this file.
 */
static void
QuoteHtml(Ns_DString *dsPtr, const char *breakChar, const char *htmlString)
    NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);


/*
 *----------------------------------------------------------------------
 *
 * Ns_QuoteHtml --
 *
 *	Quote an HTML string.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Copies quoted HTML to given dstring.
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
     * Append the last part if non-empty.
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
 *	Implements ns_quotehtml.
 *
 * Results:
 *	Tcl result.
 *
 * Side effects:
 *	See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclQuoteHtmlObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
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
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
