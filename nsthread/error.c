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
 * error.c --
 *
 *      Routines for dealing with fatal errors.
 */

#include "thread.h"


/*
 *----------------------------------------------------------------------
 *
 * NsThreadFatal --
 *
 *      Call NsThreadAbort when an operating system function fails.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Process is aborted through NsThreadAbort.
 *
 *----------------------------------------------------------------------
 */

void
NsThreadFatal(const char *func, const char *osfunc, int err)
{
#ifdef _WIN32
    Tcl_Panic("nsthreads: %s failed in %s: win32 err: %d", osfunc, func, err);
#else
    Tcl_Panic("nsthreads: %s failed in %s: %s", osfunc, func, strerror(err));
#endif
}

#ifdef _WIN32
/*
 *----------------------------------------------------------------------
 *
 * ns_snprintf --
 *
 *      Provide an emulation of snprintf for windows, to keep code smells
 *      little. The function is defined here and not in nswin32.c, since
 *      functions defined in nsthread use this.
 *
 * Results:
 *      return number of "written" chars (not including NUL character)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
ns_snprintf(char *buf, size_t len, const char *fmt, ...)
{
    va_list ap;
    int     chars;

    va_start(ap, fmt);
    chars = vsnprintf(buf, len, fmt, ap);
    va_end(ap);

    return chars;
}
#endif


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
