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
 * nsconf.c --
 *
 *      Various core configuration.
 */

#include "nsd.h"

struct nsconf nsconf;



/*
 *----------------------------------------------------------------------
 *
 * NsInitConf --
 *
 *      Initialize core elements of the nsconf structure at startup.
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
NsInitConf(void)
{
    Ns_ThreadSetName("-main:%s-", "conf");

    /*
     * At library load time the server is considered started.
     * Normally it is marked stopped immediately by Ns_Main unless
     * libnsd is being used for some other, non-server program.
     */

    nsconf.state.started = NS_TRUE;

    nsconf.build = nsBuildDate;
    nsconf.name = PACKAGE_NAME;
    nsconf.version = PACKAGE_VERSION;
    nsconf.tcl.version = TCL_VERSION;

    (void)time(&nsconf.boot_t);
    nsconf.pid = getpid();

   /*
    * At the time we are called here, the Tcl_VFS may not be
    * initialized, hence we cannot figure out the current
    * process home directory. Therefore, delegate this task
    * to the nsmain() call, after the Tcl_FindExecutable().
    */

    nsconf.home = "/";

    Tcl_InitHashTable(&nsconf.sections, TCL_STRING_KEYS);
    Tcl_DStringInit(&nsconf.servers);
    Tcl_InitHashTable(&nsconf.servertable, TCL_STRING_KEYS);
}


/*
 *----------------------------------------------------------------------
 *
 * NsConfUpdate --
 *
 *      Update various elements of the nsconf structure now that
 *      the config script has been evaluated.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Various, depending on config.
 *
 *----------------------------------------------------------------------
 */

void
NsConfUpdate(void)
{
    size_t      size;
    Ns_DString  ds;
    const char *section = NS_GLOBAL_CONFIG_PARAMETERS;

    NsConfigTcl();
    NsConfigLog();
    NsConfigAdp();
    NsConfigFastpath();
    NsConfigMimeTypes();
    NsConfigProgress();
    NsConfigDNS();
    NsConfigRedirects();
    NsConfigVhost();
    NsConfigEncodings();
    NsConfigTclHttp();

    /*
     * Set a default stacksize, if specified. Use OS default otherwise.
     */

    size = (size_t)Ns_ConfigMemUnitRange(NS_CONFIG_THREADS, "stacksize", NULL, 0, 0, INT_MAX);
    if (size == 0u) {
        size = (size_t)Ns_ConfigMemUnitRange(section, "stacksize", NULL, 0, 0, INT_MAX);
    }
    if (size > 0u) {
        (void) Ns_ThreadStackSize((ssize_t)size);
    }

    /*
     * nsmain.c
     */
    Ns_ConfigTimeUnitRange(section, "shutdowntimeout",
                           "20s", 0, 0, LONG_MAX, 0,
                           &nsconf.shutdowntimeout);
    /*
     * sched.c
     */
    nsconf.sched.jobsperthread = Ns_ConfigIntRange(section, "schedsperthread", 0, 0, INT_MAX);
    Ns_ConfigTimeUnitRange(section, "schedlogminduration",
                           "2s", 1, 0, LONG_MAX, 0,
                           &nsconf.sched.maxelapsed);
    /*
     * binder.c, win32.c
     */

    nsconf.listenbacklog = Ns_ConfigIntRange(section, "listenbacklog", 32, 0, INT_MAX);
    nsconf.sockacceptlog = Ns_ConfigIntRange(section, "sockacceptlog", 4,  2, 100);

    /*
     * tcljob.c
     */
    nsconf.job.jobsperthread = Ns_ConfigIntRange(section, "jobsperthread", 0, 0, INT_MAX);
    Ns_ConfigTimeUnitRange(section, "jobtimeout",
                           "5m", 0, 0, LONG_MAX, 0,
                           &nsconf.job.timeout);
    Ns_ConfigTimeUnitRange(section, "joblogminduration",
                           "1s", 0, 0, LONG_MAX, 0,
                           &nsconf.job.logminduration);

    /*
     * tclinit.c
     */

    Ns_DStringInit(&ds);
    nsconf.tcl.sharedlibrary = ns_strcopy(Ns_ConfigString(section, "tcllibrary", "tcl"));
    if (Ns_PathIsAbsolute(nsconf.tcl.sharedlibrary) == NS_FALSE) {
        Ns_Set    *set = Ns_ConfigCreateSection(NS_GLOBAL_CONFIG_PARAMETERS);
        TCL_SIZE_T length;

        (void)Ns_HomePath(&ds, nsconf.tcl.sharedlibrary, NS_SENTINEL);
        length = ds.length;
        ns_free((void*)nsconf.tcl.sharedlibrary);
        nsconf.tcl.sharedlibrary = Ns_DStringExport(&ds);

        Ns_SetIUpdateSz(set, "tcllibrary", 10, nsconf.tcl.sharedlibrary, length);
    }
    nsconf.tcl.lockoninit = Ns_ConfigBool(section, "tclinitlock", NS_FALSE);
    Ns_DStringFree(&ds);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
