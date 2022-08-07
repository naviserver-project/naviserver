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
 * nsthread.c --
 *
 *      Compatibility wrappers for thread calls.
 */

#include "nsd.h"


/*
 *----------------------------------------------------------------------
 *
 * Ns_SetThreadServer --
 *
 *      Sets the thread name to the name of the server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Ns_SetThreadServer(const char *server)
{
    Ns_ThreadSetName("%s", server);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetThreadServer --
 *
 *      Get the name of this server/thread.
 *
 * Results:
 *      A thread/server name
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_GetThreadServer(void)
{
  return Ns_ThreadGetName();
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
