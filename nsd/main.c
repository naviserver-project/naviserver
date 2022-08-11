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
 * main.c --
 *
 *      Example NaviServer main() startup routine.
 */

#include "nsd.h"

static Ns_ServerInitProc ServerInit;


/*
 *----------------------------------------------------------------------
 *
 * main --
 *
 *      NaviServer startup routine which simply calls Ns_Main().
 *      Ns_Main() will later call ServerInit() if not NULL.
 *
 * Results:
 *      Result of Ns_Main.
 *
 * Side effects:
 *      Server runs.
 *
 *----------------------------------------------------------------------
 */

int
main(int argc, char *const* argv)
{
    return Ns_Main(argc, argv, ServerInit);
}


/*
 *----------------------------------------------------------------------
 *
 * ServerInit --
 *
 *      Example ServerInit() which does nothing by default.  This
 *      routine is called by Ns_Main() just before loading dynamic
 *      modules.
 *
 * Results:
 *      NS_OK.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Ns_ReturnCode
ServerInit(const char *UNUSED(server))
{
    /*
     * Add code here to initialize your server much like an ordinary
     * dynamic module.
     */

    return NS_OK;
}
