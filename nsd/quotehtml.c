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
    
    while (likely(*htmlString != '\0')) {
        switch (*htmlString) {
        case '<':
            Ns_DStringAppend(dsPtr, "&lt;");
            break;

        case '>':
            Ns_DStringAppend(dsPtr, "&gt;");
            break;

	case '&':
            Ns_DStringAppend(dsPtr, "&amp;");
            break;

	case '\'':
            Ns_DStringAppend(dsPtr, "&#39;");
	    break;

	case '"':
            Ns_DStringAppend(dsPtr, "&#34;");
	    break;
	    
	default:
            Ns_DStringNAppend(dsPtr, htmlString, 1);
            break;
        }
        ++htmlString;
    }
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclQuoteHtmlCmd --
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
NsTclQuoteHtmlCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int argc, CONST84 char *argv[])
{
    Ns_DString ds;

    if (unlikely(argc != 2)) {
        Tcl_AppendResult(interp, "wrong # args:  should be \"",
                         argv[0], " html\"", NULL);
        return TCL_ERROR;
    }
    Ns_DStringInit(&ds);
    Ns_QuoteHtml(&ds, argv[1]);

    Tcl_DStringResult(interp, &ds);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
