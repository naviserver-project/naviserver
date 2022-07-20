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
 * NsInitInfo --
 *
 *      Initialize the elements of the nsconf structure which may
 *      require Ns_Log to be initialized first.
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
NsInitInfo(void)
{
    Ns_DString addr;

    if (gethostname((char *)nsconf.hostname, sizeof(nsconf.hostname)) != 0) {
        memcpy(nsconf.hostname, "localhost", 10u);
    }
    Ns_DStringInit(&addr);
    if (Ns_GetAddrByHost(&addr, nsconf.hostname)) {
        assert(addr.length < (int)sizeof(nsconf.address));
        memcpy(nsconf.address, addr.string, (size_t)addr.length + 1u);
    } else {
        memcpy(nsconf.address, NS_IP_UNSPECIFIED, strlen(NS_IP_UNSPECIFIED));
    }
    Ns_DStringFree(&addr);
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
    const char *path = NS_GLOBAL_CONFIG_PARAMETERS;

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

    /*
     * Set a default stacksize, if specified. Use OS default otherwise.
     */

    size = (size_t)Ns_ConfigMemUnitRange(NS_CONFIG_THREADS, "stacksize", NULL, 0, 0, INT_MAX);
    if (size == 0u) {
        size = (size_t)Ns_ConfigMemUnitRange(path, "stacksize", NULL, 0, 0, INT_MAX);
    }
    if (size > 0u) {
        (void) Ns_ThreadStackSize((ssize_t)size);
    }

    /*
     * nsmain.c
     */
    Ns_ConfigTimeUnitRange(path, "shutdowntimeout",
                           "20s", 0, 0, LONG_MAX, 0,
                           &nsconf.shutdowntimeout);
    /*
     * sched.c
     */
    nsconf.sched.jobsperthread = Ns_ConfigIntRange(path, "schedsperthread", 0, 0, INT_MAX);
    Ns_ConfigTimeUnitRange(path, "schedlogminduration",
                           "2s", 1, 0, LONG_MAX, 0,
                           &nsconf.sched.maxelapsed);
    /*
     * binder.c, win32.c
     */

    nsconf.backlog = Ns_ConfigIntRange(path, "listenbacklog", 32, 0, INT_MAX);

    /*
     * tcljob.c
     */
    nsconf.job.jobsperthread = Ns_ConfigIntRange(path, "jobsperthread", 0, 0, INT_MAX);
    Ns_ConfigTimeUnitRange(path, "jobtimeout",
                           "5m", 0, 0, LONG_MAX, 0,
                           &nsconf.job.timeout);
    Ns_ConfigTimeUnitRange(path, "joblogminduration",
                           "1s", 0, 0, LONG_MAX, 0,
                           &nsconf.job.logminduration);

    /*
     * tclinit.c
     */

    Ns_DStringInit(&ds);
    nsconf.tcl.sharedlibrary = Ns_ConfigString(path, "tcllibrary", "tcl");
    if (Ns_PathIsAbsolute(nsconf.tcl.sharedlibrary) == NS_FALSE) {
        Ns_Set *set = Ns_ConfigCreateSection(NS_GLOBAL_CONFIG_PARAMETERS);
        int     length;

        (void)Ns_HomePath(&ds, nsconf.tcl.sharedlibrary, (char *)0L);
        length = ds.length;
        nsconf.tcl.sharedlibrary = Ns_DStringExport(&ds);

        Ns_SetUpdateSz(set, "tcllibrary", 10, nsconf.tcl.sharedlibrary, length);
    }
    nsconf.tcl.lockoninit = Ns_ConfigBool(path, "tclinitlock", NS_FALSE);
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
