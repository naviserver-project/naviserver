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
 * master.c --
 *
 *      Master lock critical section.
 */

#include "thread.h"

static Ns_Cs master;
static bool initialized = NS_FALSE;


/*
 *----------------------------------------------------------------------
 *
 * Ns_MasterLock, Ns_MasterUnlock --
 *
 *      Enter the single master critical section lock.
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
NsInitMaster(void)
{
    Ns_CsInit(&master);
    initialized = NS_TRUE;
}

void
Ns_MasterLock(void)
{
    if (initialized) {
        Ns_CsEnter(&master);
    }
}

void
Ns_MasterUnlock(void)
{
    if (initialized) {
        Ns_CsLeave(&master);
    }
}


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
