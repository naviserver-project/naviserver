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
 * init.c --
 *
 *      NaviServer libnsd entry.
 */

#include "nsd.h"


/*
 *----------------------------------------------------------------------
 *
 * Nsd_LibInit --
 *
 *          Library entry point for libnsd. This routine calls various
 *          data structure initialization functions throughout the core.
 *
 * Results:
 *          None.
 *
 * Side effects:
 *          Numerous.
 *      Also, note that this one is called prior getting the Tcl library
 *      initialized by calling Tcl_FindExecutable() in nsmain().
 *      Therefore, no Tcl VFS calls to the filesystem should be done in
 *      any of the NsInitX() below.
 *
 *----------------------------------------------------------------------
 */

void
Nsd_LibInit(void)
{
    static bool initialized = NS_FALSE;

    if (!initialized) {
        initialized = NS_TRUE;

        nsconf.state.lock = NULL;

        Nsthreads_LibInit();

        Ns_MutexInit(&nsconf.state.lock);
        Ns_MutexSetName(&nsconf.state.lock, "nsd:conf");

        NsInitSls();
        NsInitConf(); /* <- Server marked 'started' during library load. */
        NsInitLog();
        NsInitOpenSSL();
        NsInitFd();
        NsInitBinder();
        NsInitListen();
        NsInitLimits();
        NsInitInfo();
        NsInitSockCallback();
        NsInitTask();
        NsInitProcInfo();
        NsInitDrivers();
        NsInitQueue();
        NsInitSched();
        NsInitTclEnv();
        NsInitTcl();
        NsInitRequests();
        NsInitUrl2File();
        NsInitHttptime();
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
