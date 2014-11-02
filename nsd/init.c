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
 *	    Library entry point for libnsd. This routine calls various
 *	    data structure initialization functions throughout the core.
 *
 * Results:
 *	    None.
 *
 * Side effects:
 *	    Numerous.
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
    static int once = 0;

    if (once == 0) {
        once = 1;
        Nsthreads_LibInit();
        NsInitSls();  /* Checks if server started. */
    	NsInitConf(); /* <- Server marked 'started' during library load. */
    	NsInitLog();
        NsInitFd();
    	NsInitBinder();
    	NsInitListen();
        NsInitLimits();
    	NsInitInfo();
    	NsInitProcInfo();
    	NsInitDrivers();
    	NsInitQueue();
    	NsInitSched();
    	NsInitTcl();
    	NsInitRequests();
        NsInitUrl2File();
    }
}
