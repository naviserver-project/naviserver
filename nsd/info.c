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
 * info.c --
 *
 *  Ns_Info* API and ns_info command support.
 */

#include "nsd.h"

EXTERN void Tcl_GetMemoryInfo(Tcl_DString *dsPtr);
NS_EXTERN char *nsBuildDate;

/*
 * Static variables defined in this file.
 */

static Ns_ThreadArgProc ThreadArgProc;


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoHomePath --
 *
 *      Return the home dir.
 *
 * Results:
 *      Home dir.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoHomePath(void)
{
    return nsconf.home;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoServerName --
 *
 *      Return the server name.
 *
 * Results:
 *      Server name
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoServerName(void)
{
    return nsconf.name;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoServerVersion --
 *
 *      Returns the server version
 *
 * Results:
 *      String server version.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoServerVersion(void)
{
    return nsconf.version;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoConfigFile --
 *
 *      Returns path to config file.
 *
 * Results:
 *      Path to config file.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoConfigFile(void)
{
    return nsconf.config;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoPid --
 *
 *      Returns server's PID
 *
 * Results:
 *      PID (tread like pid_t)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_InfoPid(void)
{
    return nsconf.pid;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoNameOfExecutable --
 *
 *      Returns the name of the nsd executable.  Quirky name is from Tcl.
 *
 * Results:
 *      Name of executable, string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoNameOfExecutable(void)
{
    return nsconf.nsd;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoPlatform --
 *
 *      Return platform name
 *
 * Results:
 *      Platform name, string.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoPlatform(void)
{

#if defined(__linux)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#elif defined(__sgi)
    return "irix";
#elif defined(__sun)

#if defined(__i386)
    return "solaris/intel";
#else
    return "solaris";
#endif

#elif defined(__alpha)
    return "OSF/1 - Alpha";
#elif defined(__hp10)
    return "hp10";
#elif defined(__hp11)
    return "hp11";
#elif defined(__unixware)
    return "UnixWare";
#elif defined(__APPLE__)
    return "osx";
#elif defined(_WIN32)
    return "win32";
#else
    return "?";
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoUptime --
 *
 *      Returns time server has been up.
 *
 * Results:
 *      Seconds server has been running.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_InfoUptime(void)
{
    return (int) difftime(time(NULL), nsconf.boot_t);
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoBootTime --
 *
 *      Returns time server started.
 *
 * Results:
 *      Treat as time_t.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

time_t
Ns_InfoBootTime(void)
{
    return nsconf.boot_t;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoHostname --
 *
 *      Return server hostname
 *
 * Results:
 *      Hostname
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoHostname(void)
{
    return nsconf.hostname;
}



/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoAddress --
 *
 *      Return server IP address
 *
 * Results:
 *      Primary (first) IP address of this machine.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoAddress(void)
{
    return nsconf.address;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoBuildDate --
 *
 *      Returns time server was compiled.
 *
 * Results:
 *      String build date and time.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoBuildDate(void)
{
    return nsconf.build;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoShutdownPending --
 *
 *      Boolean: is a shutdown pending?
 *
 * Results:
 *      NS_TRUE: yes, NS_FALSE: no
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_InfoShutdownPending(void)
{
    int stopping;

    Ns_MutexLock(&nsconf.state.lock);
    stopping = nsconf.state.stopping;
    Ns_MutexUnlock(&nsconf.state.lock);

    return stopping;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoStarted --
 *
 *      Boolean: has the server started up all the way yet?
 *
 * Results:
 *      NS_TRUE: yes, NS_FALSE: no
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_InfoStarted(void)
{
    int             started;

    Ns_MutexLock(&nsconf.state.lock);
    started = nsconf.state.started;
    Ns_MutexUnlock(&nsconf.state.lock);

    return started;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoServersStarted --
 *
 *      Compatability function, same as Ns_InfoStarted
 *
 * Results:
 *      See Ns_InfoStarted
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_InfoServersStarted(void)
{
    return Ns_InfoStarted();
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoTag --
 *
 *      Returns revision tag of this build
 *
 * Results:
 *      A string version name.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char *
Ns_InfoTag(void)
{
    return PACKAGE_TAG;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclInfoObjCmd --
 *
 *      Implements ns_info.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclInfoObjCmd(ClientData arg, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    int         opt;
    NsInterp    *itPtr = arg;
    char        *server, *elog;
    Tcl_DString ds;


    static CONST char *opts[] = {
        "address", "argv0", "boottime", "builddate", "callbacks",
        "config", "home", "hostname", "locks", "log",
        "major", "minor", "mimetypes", "name", "nsd", "pagedir", 
	"pageroot", "patchlevel", "pid", "platform", "pools", 
	"scheduled", "server", "servers",
        "sockcallbacks", "tag", "tcllib", "threads", "uptime",
        "version", "winnt", "filters", "traces", "requestprocs",
        "url2file", "shutdownpending", "started", NULL
    };

    enum {
        IAddressIdx, IArgv0Idx, IBoottimeIdx, IBuilddateIdx, ICallbacksIdx,
        IConfigIdx, IHomeIdx, IHostNameIdx, ILocksIdx, ILogIdx,
        IMajorIdx, IMinorIdx, IMimeIdx, INameIdx, INsdIdx, 
	IPageDirIdx, IPageRootIdx, IPatchLevelIdx,
        IPidIdx, IPlatformIdx, IPoolsIdx, 
	IScheduledIdx, IServerIdx, IServersIdx,
        ISockCallbacksIdx, ITagIdx, ITclLibIdx, IThreadsIdx, IUptimeIdx,
        IVersionIdx, IWinntIdx, IFiltersIdx, ITracesIdx, IRequestProcsIdx,
        IUrl2FileIdx, IShutdownPendingIdx, IStartedIdx
    };

    if (unlikely(objc != 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "option");
        return TCL_ERROR;
    }
    if (unlikely(Tcl_GetIndexFromObj(interp, objv[1], opts, "option", 0,
				     &opt) != TCL_OK)) {
        return TCL_ERROR;
    }

    Tcl_DStringInit(&ds);

    switch (opt) {
    case IArgv0Idx:
        Tcl_SetResult(interp, nsconf.argv0, TCL_STATIC);
        return TCL_OK;

    case IStartedIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_InfoStarted()));
        return NS_OK;

    case IShutdownPendingIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_InfoShutdownPending()));
        return NS_OK;

    case INsdIdx:
        Tcl_SetResult(interp, nsconf.nsd, TCL_STATIC);
        return TCL_OK;

    case INameIdx:
        Tcl_SetResult(interp, Ns_InfoServerName(), TCL_STATIC);
        return TCL_OK;

    case IConfigIdx:
        Tcl_SetResult(interp, Ns_InfoConfigFile(), TCL_STATIC);
        return TCL_OK;

    case ICallbacksIdx:
        NsGetCallbacks(&ds);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case ISockCallbacksIdx:
        NsGetSockCallbacks(&ds);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case IScheduledIdx:
        NsGetScheduled(&ds);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case ILocksIdx:
        Ns_MutexList(&ds);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case IThreadsIdx:
        Ns_ThreadList(&ds, ThreadArgProc);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case IPoolsIdx:
#ifdef HAVE_TCL_GETMEMORYINFO
        Tcl_GetMemoryInfo(&ds);
        Tcl_DStringResult(interp, &ds);
#endif
        return TCL_OK;

    case ILogIdx:
        elog = Ns_InfoErrorLog();
        Tcl_SetResult(interp, elog == NULL ? "STDOUT" : elog, TCL_STATIC);
        return TCL_OK;

    case IPlatformIdx:
	Ns_LogDeprecated(objv, 2, "$::tcl_platform(platform)", NULL);
        Tcl_SetResult(interp, Ns_InfoPlatform(), TCL_STATIC);
        return TCL_OK;

    case IHostNameIdx:
        Tcl_SetResult(interp, Ns_InfoHostname(), TCL_STATIC);
        return TCL_OK;

    case IAddressIdx:
        Tcl_SetResult(interp, Ns_InfoAddress(), TCL_STATIC);
        return TCL_OK;

    case IUptimeIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_InfoUptime()));
        return TCL_OK;

    case IBoottimeIdx:
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj(Ns_InfoBootTime()));
        return TCL_OK;

    case IPidIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_InfoPid()));
        return TCL_OK;

    case IMajorIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(NS_MAJOR_VERSION));
        return TCL_OK;

    case IMinorIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(NS_MINOR_VERSION));
        return TCL_OK;

    case IMimeIdx:
        NsGetMimeTypes(&ds);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case IVersionIdx:
        Tcl_SetResult(interp, NS_VERSION, TCL_STATIC);
        return TCL_OK;

    case IPatchLevelIdx:
        Tcl_SetResult(interp, NS_PATCH_LEVEL, TCL_STATIC);
        return TCL_OK;

    case IHomeIdx:
        Tcl_SetResult(interp, Ns_InfoHomePath(), TCL_STATIC);
        return TCL_OK;

    case IWinntIdx:
	Ns_LogDeprecated(objv, 2, "$::tcl_platform(platform)", NULL);
#ifdef _WIN32
        Tcl_SetResult(interp, "1", TCL_STATIC);
#else
        Tcl_SetResult(interp, "0", TCL_STATIC);
#endif
        return TCL_OK;

    case IBuilddateIdx:
        Tcl_SetResult(interp, Ns_InfoBuildDate(), TCL_STATIC);
        return TCL_OK;

    case ITagIdx:
        Tcl_SetResult(interp, Ns_InfoTag(), TCL_STATIC);
        return TCL_OK;

    case IServersIdx:
        Tcl_SetResult(interp, nsconf.servers.string, TCL_STATIC);
        return TCL_OK;
    }

    /*
     * The following subcommands require a virtual server.
     */

    if (unlikely(itPtr->servPtr == NULL)) {
        Tcl_SetResult(interp, "no server", TCL_STATIC);
        return TCL_ERROR;
    }

    server = itPtr->servPtr->server;

    switch (opt) {
    case IPageDirIdx:
    case IPageRootIdx:
	Ns_LogDeprecated(objv, 2, "ns_server ?-server s? pagedir", NULL);
        NsPageRoot(&ds, itPtr->servPtr, NULL);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case IServerIdx:
        Tcl_SetResult(interp, server, TCL_STATIC);
        return TCL_OK;

    case ITclLibIdx:
	Ns_LogDeprecated(objv, 2, "ns_server ?-server s? tcllib", NULL);
        Tcl_SetResult(interp, itPtr->servPtr->tcl.library, TCL_STATIC);
        return TCL_OK;

    case IFiltersIdx:
	Ns_LogDeprecated(objv, 2, "ns_server ?-server s? filters", NULL);
        NsGetFilters(&ds, server);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case ITracesIdx:
	Ns_LogDeprecated(objv, 2, "ns_server ?-server s? traces", NULL);
        NsGetTraces(&ds, server);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case IRequestProcsIdx:
	Ns_LogDeprecated(objv, 2, "ns_server ?-server s? requestprocs", NULL);
        NsGetRequestProcs(&ds, server);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;

    case IUrl2FileIdx:
	Ns_LogDeprecated(objv, 2, "ns_server ?-server s? url2file", NULL);
        NsGetUrl2FileProcs(&ds, server);
        Tcl_DStringResult(interp, &ds);
        return TCL_OK;
    }

    Tcl_SetResult(interp, "unrecognized option", TCL_STATIC);

    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLibraryCmd --
 *
 *  Implements ns_library.
 *
 * Results:
 *  Tcl result.
 *
 * Side effects:
 *  See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclLibraryCmd(ClientData arg, Tcl_Interp *interp, int argc, CONST char* argv[])
{
    NsInterp *itPtr = arg;
    char *lib;
    Ns_DString ds;

    if (argc != 2 && argc != 3) {
	Tcl_AppendResult(interp, "wrong # args: should be \"",
			 argv[0], " library ?module?\"", NULL);
	return TCL_ERROR;
    }
    if (STREQ(argv[1], "private")) {
        lib = itPtr->servPtr->tcl.library;
    } else if (STREQ(argv[1], "shared")) {
        lib = nsconf.tcl.sharedlibrary;
    } else {
	Tcl_AppendResult(interp, "unknown library \"",
			 argv[1], "\": should be private or shared", NULL);
	return TCL_ERROR;
    }
    Ns_DStringInit(&ds);
    Ns_MakePath(&ds, lib, argv[2], NULL);
    Tcl_SetResult(interp, ds.string, TCL_VOLATILE);
    Ns_DStringFree(&ds);

    return TCL_OK;
}


static void
ThreadArgProc(Tcl_DString *dsPtr, void *proc, void *arg)
{
    Ns_GetProcInfo(dsPtr, proc, arg);
}
