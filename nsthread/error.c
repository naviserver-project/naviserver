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
