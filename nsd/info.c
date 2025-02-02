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
 * info.c --
 *
 *  Ns_Info* API and ns_info command support.
 */

#include "nsd.h"

#ifndef _MSC_VER
# include <dlfcn.h>
#endif

/*
 * Static variables defined in this file.
 */

static Ns_ThreadArgProc ThreadArgProc;
#ifndef _MSC_VER
typedef void (*MallocExtension_GetStats_t)(char *, int);
typedef void (*MallocExtension_ReleaseFreeMemory_t)(void);
static MallocExtension_GetStats_t MallocExtensionGetStats = NULL;
static MallocExtension_ReleaseFreeMemory_t MallocExtensionReleaseFreeMemory = NULL;
static void* preload_library_handle = NULL;
static const char *preload_library_name = NULL;
static const char *mallocLibraryVersionString = "unknown";

#endif

/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoHomePath --
 *
 *      Returns the home directory.
 *
 * Results:
 *      String with the full path.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_InfoHomePath(void)
{
    return nsconf.home;
}

/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoLogPath --
 *
 *      Returns the absolute path of the log directory.
 *
 * Results:
 *      String with the full path.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_InfoLogPath(void)
{
    return nsconf.logDir;
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

const char *
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

const char *
Ns_InfoServerVersion(void)
{
    return nsconf.version;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoConfigFile --
 *
 *      Returns path to configuration file.
 *
 * Results:
 *      Path to configuration file.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_InfoConfigFile(void)
{
    return nsconf.configFile;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoPid --
 *
 *      Returns server's PID
 *
 * Results:
 *      PID (thread like pid_t)
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

pid_t
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

const char *
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

const char *
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

long
Ns_InfoUptime(void)
{
    double diff = difftime(time(NULL), nsconf.boot_t);

    return (long)diff;
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

const char *
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

const char *
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

const char *
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

bool
Ns_InfoShutdownPending(void)
{
    bool stopping;

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

bool
Ns_InfoStarted(void)
{
    bool started;

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
 *      Compatibility function, same as Ns_InfoStarted
 *
 * Results:
 *      See Ns_InfoStarted
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
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

const char *
Ns_InfoTag(void)
{
    return PACKAGE_TAG;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoIPv6 --
 *
 *      Returns information if the binary was compiled with IPv6 support
 *
 * Results:
 *      Boolean result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_InfoIPv6(void)
{
#ifdef HAVE_IPV6
    return NS_TRUE;
#else
    return NS_FALSE;
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_InfoSSL --
 *
 *      Returns information if the binary was compiled with OpenSSL support
 *
 * Results:
 *      Boolean result.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

bool
Ns_InfoSSL(void)
{
#ifdef HAVE_OPENSSL_EVP_H
    return NS_TRUE;
#else
    return NS_FALSE;
#endif
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


#ifndef _MSC_VER
    {
        char *preloadString = getenv("LD_PRELOAD");

        if (preloadString != NULL) {
            Tcl_DString ds;
            char       *token;

            /*
             * The content of LD_PRELOAD might contain multiple
             * libraries.  Look for the first one with a plausible
             * name and just open this library later.
             */
            Tcl_DStringInit(&ds);
            Tcl_DStringAppend(&ds, preloadString, TCL_INDEX_NONE);
            token = ns_strtok(ds.string, ": ");

            if (token == NULL) {
                /*
                 * Single token.
                 */
                if (ns_memmem(ds.string, (size_t)ds.length, "tcmalloc", 8)) {
                    preload_library_name = ns_strdup(ds.string);
                }
            } else {
                /*
                 * Multiple tokens.
                 */
                while (token != NULL) {
                    if (ns_memmem(token, strlen(token), "tcmalloc", 8)) {
                        preload_library_name = ns_strdup(token);
                        break;
                    }
                    token = ns_strtok(NULL, ": ");
                }
            }
            Tcl_DStringFree(&ds);
        }

        if (preload_library_name != NULL) {
            typedef const char *(*MallocExtension_GetVersion_t)(int *, int *, const char**);
            static MallocExtension_GetVersion_t MallocExtensionGetVersion = NULL;

            /*
             * Get a handle to the malloc library to be able to obtain
             * symbols from there. The library is kept open, it could be
             * closed during shutdown with "dlclose(preload_library_handle)".
             */
            preload_library_handle = dlopen(preload_library_name, RTLD_LAZY);
            if (preload_library_handle == NULL) {
                Ns_Log(Warning, "could not open preload library '%s'", preload_library_name);
            } else {
                const void *symbol;

                symbol = dlsym(preload_library_handle, "MallocExtension_GetStats");
                memcpy(&MallocExtensionGetStats, &symbol, sizeof(ns_funcptr_t));

                symbol = dlsym(preload_library_handle, "tc_version");
                memcpy(&MallocExtensionGetVersion, &symbol, sizeof(ns_funcptr_t));

                symbol = dlsym(preload_library_handle, "MallocExtension_ReleaseFreeMemory");
                memcpy(&MallocExtensionReleaseFreeMemory, &symbol, sizeof(ns_funcptr_t));

                if (MallocExtensionGetVersion != NULL) {
                    mallocLibraryVersionString = MallocExtensionGetVersion(NULL, NULL, NULL);
                }

                Ns_Log(Notice, "preload library '%s' opened, version %s, found stats symbol: %d",
                       preload_library_name, mallocLibraryVersionString, MallocExtensionGetStats != NULL);
# if 0
                {
                    int i = 0;
                    const char *symbolName, *tab[] =  {
                        "malloc",
                        "free",
                        "MallocExtension_GetStats",
                        "MallocExtension_ReleaseFreeMemory",
                        "malloc_stats",
                        "tc_version",
                        NULL
                    };
                    for (symbolName = tab[0]; symbolName != NULL; symbolName=tab[++i]) {
                        Ns_Log(Notice, "symbol lookup %s -> %p",
                               symbolName, dlsym(preload_library_handle, symbolName));
                    }
                }
# endif
            }
        }
    }
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclInfoObjCmd --
 *
 *      Implements "ns_info".
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
NsTclInfoObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int             opt, result = TCL_OK;
    bool            done = NS_TRUE;
    const NsInterp *itPtr = clientData;
    Tcl_DString     ds;

    static const char *const opts[] = {
        "address", "argv", "argv0", "boottime", "builddate", "buildinfo", "callbacks",
        "config", "home", "hostname", "ipv6", "locks", "log",
        "major", "meminfo", "minor", "mimetypes", "name", "nsd",
        "patchlevel", "pid", "pools",
        "scheduled", "server", "servers",
        "sockcallbacks", "ssl", "tag", "threads", "uptime",
        "version",
        "shutdownpending", "started",
#ifdef NS_WITH_DEPRECATED
        "filters", "pagedir", "pageroot", "platform", "traces",
        "requestprocs", "tcllib", "url2file", "winnt",
#endif
        NULL
    };

    enum {
        IAddressIdx, IArgvIdx, IArgv0Idx, IBoottimeIdx, IBuilddateIdx, IBuildinfoIdx, ICallbacksIdx,
        IConfigIdx, IHomeIdx, IHostNameIdx, IIpv6Idx, ILocksIdx, ILogIdx,
        IMajorIdx, IMeminfoIdx, IMinorIdx, IMimeIdx, INameIdx, INsdIdx,
        IPatchLevelIdx,
        IPidIdx, IPoolsIdx,
        IScheduledIdx, IServerIdx, IServersIdx,
        ISockCallbacksIdx, ISSLIdx, ITagIdx, IThreadsIdx, IUptimeIdx,
        IVersionIdx,
        IShutdownPendingIdx, IStartedIdx,
#ifdef NS_WITH_DEPRECATED
        IFiltersIdx, IPageDirIdx, IPageRootIdx, IPlatformIdx, ITracesIdx,
        IRequestProcsIdx, ITclLibIdx, IUrl2FileIdx, IWinntIdx,
#endif
        INULL
    };

    if (unlikely(objc < 2)) {
        Tcl_WrongNumArgs(interp, 1, objv, "/subcommand/");
        return TCL_ERROR;
    } else if (unlikely(Tcl_GetIndexFromObj(interp, objv[1], opts, "subcommand", 0,
                                     &opt) != TCL_OK)) {
        return TCL_ERROR;
    }
    if ((opt != IMeminfoIdx && objc != 2)
        || (opt == IMeminfoIdx && objc > 3)) {
        if (Ns_ParseObjv(NULL, NULL, interp, 2, objc, objv) != NS_OK) {
            return TCL_ERROR;
        } else {
            Tcl_WrongNumArgs(interp, 1, objv, "/subcommand/");
            return TCL_ERROR;
        }
    }

    Tcl_DStringInit(&ds);

    switch (opt) {
    case IArgvIdx:
        Tcl_SetObjResult(interp, nsconf.argvObj);
        break;

    case IArgv0Idx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(nsconf.argv0, TCL_INDEX_NONE));
        break;

    case IStartedIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_InfoStarted() ? 1 : 0));
        break;

    case IShutdownPendingIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(Ns_InfoShutdownPending() ? 1 : 0));
        break;

    case INsdIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(nsconf.nsd, TCL_INDEX_NONE));
        break;

    case INameIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoServerName(), TCL_INDEX_NONE));
        break;

    case IConfigIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoConfigFile(), TCL_INDEX_NONE));
        break;

    case ICallbacksIdx:
        NsGetCallbacks(&ds);
        Tcl_DStringResult(interp, &ds);
        break;

    case ISockCallbacksIdx:
        NsGetSockCallbacks(&ds);
        Tcl_DStringResult(interp, &ds);
        break;

    case IScheduledIdx:
        NsGetScheduled(&ds);
        Tcl_DStringResult(interp, &ds);
        break;

    case ILocksIdx:
        Ns_MutexList(&ds);
        Ns_RWLockList(&ds);
        Tcl_DStringResult(interp, &ds);
        break;

    case IThreadsIdx:
        Ns_ThreadList(&ds, ThreadArgProc);
        Tcl_DStringResult(interp, &ds);
        break;

    case IPoolsIdx:
#ifdef HAVE_TCL_GETMEMORYINFO
        Tcl_GetMemoryInfo(&ds);
        Tcl_DStringResult(interp, &ds);
#endif
        break;

    case ILogIdx:
        {
            const char *elog = Ns_InfoErrorLog();
            Tcl_SetObjResult(interp, Tcl_NewStringObj(elog == NULL ? "STDOUT" : elog, TCL_INDEX_NONE));
        }
        break;

    case IHostNameIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoHostname(), TCL_INDEX_NONE));
        break;

    case IIpv6Idx:
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(Ns_InfoIPv6()));
        break;

    case IAddressIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoAddress(), TCL_INDEX_NONE));
        break;

    case IUptimeIdx:
        Tcl_SetObjResult(interp, Tcl_NewLongObj(Ns_InfoUptime()));
        break;

    case IBoottimeIdx:
        Tcl_SetObjResult(interp, Tcl_NewLongObj((long)Ns_InfoBootTime()));
        break;

    case IPidIdx:
        Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt)Ns_InfoPid()));
        break;

    case IMajorIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(NS_MAJOR_VERSION));
        break;

    case IMinorIdx:
        Tcl_SetObjResult(interp, Tcl_NewIntObj(NS_MINOR_VERSION));
        break;

    case IMimeIdx:
        NsGetMimeTypes(&ds);
        Tcl_DStringResult(interp, &ds);
        break;

    case IVersionIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(NS_VERSION, TCL_INDEX_NONE));
        break;

    case IPatchLevelIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(NS_PATCH_LEVEL, TCL_INDEX_NONE));
        break;

    case IHomeIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoHomePath(), TCL_INDEX_NONE));
        break;

    case IBuilddateIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoBuildDate(), TCL_INDEX_NONE));
        break;

    case ITagIdx:
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoTag(), TCL_INDEX_NONE));
        break;

    case IServersIdx:
        {
            const Tcl_DString *dsPtr = &nsconf.servers;
            Tcl_SetObjResult(interp, Tcl_NewStringObj(dsPtr->string, dsPtr->length));
            break;
        }

    case ISSLIdx:
        Tcl_SetObjResult(interp, Tcl_NewBooleanObj(Ns_InfoSSL()));
        break;

    case IBuildinfoIdx:
        {
            Tcl_Obj *dictObj = Tcl_NewDictObj();
            int defined_NDEBUG, defined_SYSTEM_MALLOC, defined_NS_WITH_DEPRECATED;

            /*
             * Detect the compiler.
             */
#if defined(__GNUC__)
# if defined (__MINGW32__)
            Ns_DStringPrintf(&ds, "MinGW gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
# elif defined(__clang__)
            Ns_DStringPrintf(&ds, "clang %s",__clang_version__);
# else
            Ns_DStringPrintf(&ds, "gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
# endif
#elif defined(_MSC_VER)
            Ns_DStringPrintf(&ds, "MSC %d", _MSC_VER);
#else
            Tcl_DStringAppend(&ds, "unknown", 7);
#endif
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("compiler", 8),
                           Tcl_NewStringObj(ds.string, ds.length));
            /*
             * Compiled with assertion support? Actually, without
             * -DNDEBUG.
             *
             * Note: we use the variables defined_NDEBUG and
             * defined_SYSTEM_MALLOC since Tcl_NewIntObj() is defined
             * as a macro since using the ifdef check as arguments
             * leads to an undefined behavior. Tcl 9 uses macros more
             * extensively for the API.
             */
            defined_NDEBUG =
#if defined(NDEBUG)
                                         0
#else
                                         1
#endif
                ;
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("assertions", 10),
                           Tcl_NewIntObj(defined_NDEBUG));
            /*
             * Compiled with SYSTEM_MALLOC?
             */
            defined_SYSTEM_MALLOC =
#if defined(SYSTEM_MALLOC)
                                         1
#else
                                         0
#endif
                ;
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("system_malloc", 13),
                           Tcl_NewIntObj(defined_SYSTEM_MALLOC));

            /*
             * Compiled with NS_WITH_DEPRECATED?
             */
            defined_NS_WITH_DEPRECATED =
#if defined(NS_WITH_DEPRECATED)
                                         1
#else
                                         0
#endif
                ;
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("with_deprecated", 15),
                           Tcl_NewIntObj(defined_NS_WITH_DEPRECATED));

            /*
             * The nsd binary was built against this version of Tcl
             */
            Tcl_DictObjPut(NULL, dictObj,
                           Tcl_NewStringObj("tcl", 3),
                           Tcl_NewStringObj(TCL_PATCH_LEVEL, -1));

            Tcl_SetObjResult(interp, dictObj);
            Tcl_DStringFree(&ds);
        }
        break;

    case IMeminfoIdx: {
        Tcl_Obj    *resultObj = Tcl_NewDictObj();
#ifndef _MSC_VER
        char        memStatsBuffer[20000] = {0};
        int         release = 0;
        Ns_ObjvSpec flags[] = {
            {"-release", Ns_ObjvBool, &release,  INT2PTR(NS_TRUE)},
            {NULL,       NULL,        NULL,      NULL}
        };

        if (Ns_ParseObjv(flags, NULL, interp, 2, objc, objv) != NS_OK) {
            Tcl_DecrRefCount(resultObj);
            return TCL_ERROR;
        }

        if (preload_library_name != NULL && preload_library_handle != NULL) {
            if (MallocExtensionReleaseFreeMemory != NULL && release != 0) {
                Ns_Log(Notice, "MallocExtension_ReleaseFreeMemory");
                MallocExtensionReleaseFreeMemory();
            }
            if (MallocExtensionGetStats != NULL) {
                MallocExtensionGetStats(memStatsBuffer, sizeof(memStatsBuffer));
            }
        }
        Tcl_DictObjPut(NULL, resultObj,
                       Tcl_NewStringObj("preload", 7),
                       Tcl_NewStringObj(preload_library_name != NULL ? preload_library_name : "", TCL_INDEX_NONE));
        Tcl_DictObjPut(NULL, resultObj,
                       Tcl_NewStringObj("version", 7),
                       Tcl_NewStringObj(mallocLibraryVersionString, TCL_INDEX_NONE));
        Tcl_DictObjPut(NULL, resultObj,
                       Tcl_NewStringObj("stats", 5),
                       Tcl_NewStringObj(memStatsBuffer, TCL_INDEX_NONE));
#endif
        Tcl_SetObjResult(interp, resultObj);
        break;
    }

#ifdef NS_WITH_DEPRECATED
        /*
         * All following cases are deprecated.
         */
    case IPlatformIdx:
        Ns_LogDeprecated(objv, 2, "$::tcl_platform(platform)", NULL);
        Tcl_SetObjResult(interp, Tcl_NewStringObj(Ns_InfoPlatform(), TCL_INDEX_NONE));
        break;

    case IWinntIdx:
        Ns_LogDeprecated(objv, 2, "$::tcl_platform(platform)", NULL);
# ifdef _WIN32
        Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
# else
        Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
# endif
        break;
#endif

    default:
        /* cases handled below */
        done = NS_FALSE;
        break;
    }

    if (!done) {
        /*
         * The following subcommands require a virtual server.
         */

        if (unlikely(itPtr->servPtr == NULL)) {
            Tcl_SetObjResult(interp, Tcl_NewStringObj("no server", TCL_INDEX_NONE));
            result = TCL_ERROR;

        } else {
            const char *server;

            server = itPtr->servPtr->server;

            switch (opt) {
            case IServerIdx:
                Tcl_SetObjResult(interp,  Tcl_NewStringObj(server, TCL_INDEX_NONE));
                break;

#ifdef NS_WITH_DEPRECATED
                /*
                 * All following cases are deprecated.
                 */
            case IPageDirIdx: NS_FALL_THROUGH; /* fall through */
            case IPageRootIdx:
                Ns_LogDeprecated(objv, 2, "ns_server ?-server s? pagedir", NULL);
                (void)NsPageRoot(&ds, itPtr->servPtr, NULL);
                Tcl_DStringResult(interp, &ds);
                break;

            case ITclLibIdx:
                Ns_LogDeprecated(objv, 2, "ns_server ?-server s? tcllib", NULL);
                Tcl_SetObjResult(interp, Tcl_NewStringObj(itPtr->servPtr->tcl.library, TCL_INDEX_NONE));
                break;

            case IFiltersIdx:
                Ns_LogDeprecated(objv, 2, "ns_server ?-server s? filters", NULL);
                NsGetFilters(&ds, server);
                Tcl_DStringResult(interp, &ds);
                break;

            case ITracesIdx:
                Ns_LogDeprecated(objv, 2, "ns_server ?-server s? traces", NULL);
                NsGetTraces(&ds, server);
                Tcl_DStringResult(interp, &ds);
                break;

            case IRequestProcsIdx:
                Ns_LogDeprecated(objv, 2, "ns_server ?-server s? requestprocs", NULL);
                NsGetRequestProcs(&ds, server);
                Tcl_DStringResult(interp, &ds);
                break;

            case IUrl2FileIdx:
                Ns_LogDeprecated(objv, 2, "ns_server ?-server s? url2file", NULL);
                NsGetUrl2FileProcs(&ds, server);
                Tcl_DStringResult(interp, &ds);
                break;

#endif
            default:
                Tcl_SetObjResult(interp, Tcl_NewStringObj("unrecognized option", TCL_INDEX_NONE));
                result = TCL_ERROR;
                break;
            }
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclLibraryObjCmd --
 *
 *  Implements "ns_library".
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
NsTclLibraryObjCmd(ClientData clientData, Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int             result = TCL_OK, kind;
    char           *moduleString = NULL;
    const char     *lib = NS_EMPTY_STRING;
    const NsInterp *itPtr = clientData;
    static Ns_ObjvTable kindTable[] = {
        {"private",  1u},
        {"shared",   2u},
        {NULL,       0u}
    };
    Ns_ObjvSpec  args[] = {
        {"kind",    Ns_ObjvIndex,  &kind,         &kindTable},
        {"?module", Ns_ObjvString, &moduleString, NULL},
        {NULL, NULL, NULL, NULL}
    };

    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (kind == 1u) {
        lib = itPtr->servPtr->tcl.library;
    } else /* if (kind == 2u)*/ {
        lib = nsconf.tcl.sharedlibrary;
    }

    if (result == TCL_OK) {
        Ns_DString ds;

        Ns_DStringInit(&ds);
        if (moduleString != NULL) {
            (void)Ns_MakePath(&ds, lib, moduleString, (char *)0L);
        } else {
            (void)Ns_MakePath(&ds, lib, (char *)0L);
        }
        Tcl_DStringResult(interp, &ds);
    }
    return result;
}


static void
ThreadArgProc(Tcl_DString *dsPtr, Ns_ThreadProc proc, const void *arg)
{
    Ns_GetProcInfo(dsPtr, (ns_funcptr_t)proc, arg);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 70
 * indent-tabs-mode: nil
 * End:
 */
