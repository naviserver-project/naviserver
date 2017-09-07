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

void
Ns_QuoteHtml(Ns_DString *dsPtr, const char *htmlString)
{
    NS_NONNULL_ASSERT(dsPtr != NULL);
    NS_NONNULL_ASSERT(htmlString != NULL);

    /*
     * If the first character is a null character, there is nothing to do.
     */
    if (*htmlString != '\0') {
        const char *p, *toProcess;

        for (toProcess = htmlString;;toProcess = ++p) {
            /*
             * Check for protected characters.
             */
            p = strpbrk(toProcess, "<>&'\"");

            if (p == NULL) {
                /*
                 * No protected char found, append the string and finish.
                 */
                Ns_DStringAppend(dsPtr, toProcess);
                break;
            } else {
                /*
                 * Append the first part, escape the protected char, and
                 * continue.
                 */
                Ns_DStringNAppend(dsPtr, toProcess, (int)(p - toProcess));
                switch (*p) {
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
            }
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
    char        *htmlString;
    Ns_ObjvSpec  args[] = {
        {"html", Ns_ObjvString,  &htmlString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else {
        Ns_DString ds;

        Ns_DStringInit(&ds);
        Ns_QuoteHtml(&ds, htmlString);

        Tcl_DStringResult(interp, &ds);
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
