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
 * nsmain.c --
 *
 *  NaviServer Ns_Main() startup routine.
 */

#include "nsd.h"

/*
 * The following structure is used to pass command line args to
 * the command interpreter thread.
 */

typedef struct Args {
    char **argv;
    int    argc;
} Args;

/*
 * This one is used to better track run state
 * when looking at the code
 */

typedef enum _runState {
    starting,  /* == 0 */
    running,   /* == 1 */
    stopping,  /* == 2 */
    exiting    /* == 3 */
} runState;

/*
 * Local functions defined in this file.
 */

static Ns_ThreadProc CmdThread;

static void UsageError(char *msg, ...);
static void StatusMsg(runState state);
static void LogTclVersion(void);
static char *MakePath(char *file);
static char *SetCwd(char *homedir);

#if (STATIC_BUILD == 1)
extern void NsthreadsInit();
extern void NsdInit();
#endif



/*
 *----------------------------------------------------------------------
 *
 * Ns_Main --
 *
 *      The NaviServer startup routine called from main(). Startup is
 *      somewhat complicated to ensure certain things happen in the
 *      correct order.
 *
 * Results:
 *      Returns 0 to main() on final exit.
 *
 * Side effects:
 *      Many - read comments below.
 *
 *----------------------------------------------------------------------
 */

int
Ns_Main(int argc, char **argv, Ns_ServerInitProc *initProc)
{
    Args      cmd;
    int       i, sig, optind;
    char     *config = NULL;
    Ns_Time   timeout;
    Ns_Set   *set;

#ifndef _WIN32
    int       debug = 0, mode = 0;
    char     *root = NULL, *garg = NULL, *uarg = NULL, *server = NULL;
    char     *bindargs = NULL, *bindfile = NULL;
    Ns_Set   *servers;
    struct rlimit  rl;
#else
    /*
     * The following variables are declared static so they
     * preserve their values when Ns_Main is re-entered by
     * the Win32 service control manager.
     */
    static int     mode = 0;
    static Ns_Set *servers;
    static char   *procname, *server;
#endif

    /*
     * Initialise the Nsd library.
     */

    Nsd_LibInit();

    /*
     * Mark the server stopped until initialization is complete.
     */

    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.started = 0;
    Ns_MutexUnlock(&nsconf.state.lock);

    /*
     * When run as a Win32 service, Ns_Main will be re-entered
     * in the service main thread. In this case, jump past the
     * point where the initial thread blocked when connected to
     * the service control manager.
     */

#ifdef _WIN32
    if (mode == 'S') {
        goto contservice;
    }
#endif

    nsconf.argv0 = argv[0];

    /*
     * Parse the command line arguments.
     */

    for (optind = 1; optind < argc; optind++) {
        if (argv[optind][0] != '-') {
            break;
        }
        switch (argv[optind][1]) {
        case 'h':
            UsageError(NULL);
            break;
        case 'c':
        case 'f':
        case 'V':
#ifdef _WIN32
        case 'I':
        case 'R':
        case 'S':
#else
        case 'i':
        case 'w':
#endif
            if (mode != 0) {
#ifdef _WIN32
                UsageError("only one of -c, -f, -I, -R, or -S"
                           " options may be specified");
#else
                UsageError("only one of -c, -f, -i, or -w"
                           " options may be specified");
#endif
            }
            mode = argv[optind][1];
            break;
        case 's':
            if (server != NULL) {
                UsageError("multiple -s <server> options");
            }
            if (optind + 1 < argc) {
                server = argv[++optind];
            } else {
                UsageError("no parameter for -s option");
            }
            break;
        case 't':
            if (nsconf.config != NULL) {
                UsageError("multiple -t <file> options");
            }
            if (optind + 1 < argc) {
                nsconf.config = argv[++optind];
            } else {
                UsageError("no parameter for -t option");
            }
            break;
        case 'p':
        case 'z':
            /* NB: Ignored. */
            break;
#ifndef _WIN32
        case 'b':
            if (optind + 1 < argc) {
            	bindargs = argv[++optind];
            } else {
                UsageError("no parameter for -b option");
            }
            break;
        case 'B':
            if (optind + 1 < argc) {
            	bindfile = argv[++optind];
            } else {
                UsageError("no parameter for -B option");
            }
            break;
        case 'r':
            if (optind + 1 < argc) {
            	root = argv[++optind];
            } else {
                UsageError("no parameter for -r option");
            }
            break;
        case 'd':
            debug = 1;
            break;
        case 'g':
            if (optind + 1 < argc) {
                garg = argv[++optind];
            } else {
                UsageError("no parameter for -g option");
            }
            break;
        case 'u':
            if (optind + 1 < argc) {
                uarg = argv[++optind];
            } else {
                UsageError("no parameter for -u option");
            }
            break;
#endif
        default:
            UsageError("invalid option: -%c", argv[optind][1]);
            break;
        }
    }
    if (mode == 'V') {
        printf("%s/%s\n", PACKAGE_NAME, PACKAGE_VERSION);
        printf("   Tag:             %s\n", Ns_InfoTag());
        printf("   Built:           %s\n", Ns_InfoBuildDate());
        printf("   Tcl version:     %s\n", nsconf.tcl.version);
        printf("   Platform:        %s\n", Ns_InfoPlatform());
        return 0;
    }

    if (mode == 'c') {
        cmd.argv = ns_calloc((size_t) argc - optind + 2, sizeof(char *));
        cmd.argc = 0;
        cmd.argv[cmd.argc++] = argv[0];
        for (i = optind; i < argc; i++) {
            cmd.argv[cmd.argc++] = argv[i];
        }
        Ns_ThreadCreate(CmdThread, &cmd, 0, NULL);
    }

#ifndef _WIN32

    /*
     * If running as privileged user (root) check given user/group
     * information and bail-out if any of them not really known.
     */

    if (getuid() == 0) {

        /*
         * OK, so the caller is running as root. In such cases
         * he/she should have used "-u" to give the actual user
         * to run as (may be root as well) and optionally "-g"
         * to set the process group.
         */

        if (uarg == NULL) {
            Ns_Fatal("nsmain: will not run without valid user; "
                     "must specify '-u username' parameter");
        }
    }

    /*
     * Fork into the background
     */

    if (mode == 0 || mode == 'w') {
        i = ns_fork();
        if (i == -1) {
            Ns_Fatal("nsmain: fork() failed: '%s'", strerror(errno));
        }
        if (i > 0) {
            return 0;
        }
        setsid(); /* Detach from the controlling terminal device */
    }

    /*
     * For watchdog mode, start the watchdog/server process pair.
     * The watchdog will monitor and restart the server unless the
     * server exits gracefully, either by calling exit(0) or get
     * signalled by the SIGTERM signal.
     * The watchdog itself will exit when the server exits gracefully,
     * or, when get signalled by the SIGTERM signal. In the latter
     * case, watchdog will pass the SIGTERM to the server, so both of
     * them will gracefully exit.
     */

    if (mode == 'w') {
        if (NsForkWatchedProcess() == 0) {
            /*
             * Watchdog exiting. We're done.
             */
            return 0;
        }

        /*
         * Continue as watched server process.
         */
    }

    nsconf.pid = getpid();

    /*
     * Block all signals for the duration of startup to ensure any new
     * threads inherit the blocked state.
     */

    NsBlockSignals(debug);

#endif /* ! _WIN32 */

    /*
     * The call to Tcl_FindExecutable() must be done before we ever
     * attempt any file-related operation, because it is initializing
     * the Tcl library and Tcl VFS (virtual filesystem interface)
     * which is used throughout the code.
     * Side-effect of this call is initialization of the notifier
     * subsystem. The notifier subsystem creates special private
     * notifier thread and we should better do this after all those
     * ns_fork's above...
     */

    Tcl_FindExecutable(argv[0]);
    nsconf.nsd = ns_strdup(Tcl_GetNameOfExecutable());

    /*
     * Find and read config file, if given at the command line, just use it,
     * if not specified, try to figure out by looking in the current dir for
     * nsd.tcl and for ../conf/nsd.tcl
     */

    if (nsconf.config == NULL) {
        nsconf.config = MakePath("nsd.tcl");
        if (nsconf.config == NULL) {
            nsconf.config = MakePath("conf/nsd.tcl");
        }
    }

    if (!(mode == 'c' && nsconf.config == NULL)) {
	config = NsConfigRead(nsconf.config);
    }

#ifndef _WIN32

    /*
     * Pre-bind any sockets now, before a possible setuid from root
     * or chroot which may hide /etc/resolv.conf required to resolve
     * name-based addresses.
     */

    NsPreBind(bindargs, bindfile);

    /*
     * Chroot() if requested before setuid from root.
     */

    if (root != NULL) {
        if (chroot(root) != 0) {
            Ns_Fatal("nsmain: chroot(%s) failed: '%s'", root, strerror(errno));
        }
        nsconf.home = SetCwd("/");
    }

    /*
     * If caller is running as the privileged user, change
     * to the run time (given) user and/or group now.
     */

    if (getuid() == 0) {

        /*
         * Set or clear supplementary groups.
         */

        if (Ns_SetGroup(garg) == NS_ERROR) {
            Ns_Fatal("nsmain: failed to switch to group %s", garg);
        }

        /*
         * Before setuid, fork the background binder process to
         * listen on ports which were not pre-bound above.
         */

        NsForkBinder();

        if (Ns_SetUser(uarg) == NS_ERROR) {
            Ns_Fatal("nsmain: failed to switch to user %s", uarg);
        }
    }

#ifdef __linux

    /*
     * On Linux, once a process changes uid/gid, the dumpable flag
     * is cleared, preventing a core file from being written.  On
     * Linux 2.4+, it can be set again using prctl() so that we can
     * get core files.
     */

    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) < 0) {
        Ns_Fatal("nsmain: prctl(PR_SET_DUMPABLE) failed: '%s'",
                 strerror(errno));
    }

#endif /* __linux */

#endif /* ! _WIN32 */

    if (config) {
	/*
	 * Evaluate the config file.
	 */
	
	NsConfigEval(config, argc, argv, optind);
	ns_free(config);
    }

    /*
     * If no servers were defained, autocreate server "default"
     * so all default config values will be used for that server
     */

    servers = Ns_ConfigCreateSection("ns/servers");
    if (Ns_SetSize(servers) == 0) {
        Ns_SetPut(servers, "default", "Default NaviServer");
    }

    /*
     * If a single server was specified, ensure it exists
     * and update the pointer to the config string (the
     * config server strings are considered the unique
     * server "handles").
     */

    if (server != NULL) {
        i = Ns_SetFind(servers, server);
        if (i < 0) {
            Ns_Fatal("nsmain: no such server '%s'", server);
        }
        server = Ns_SetKey(servers, i);
    }


    /*
     * Verify and change to the home directory.
     */

    nsconf.home = Ns_ConfigGetValue(NS_CONFIG_PARAMETERS, "home");
    if (mode != 'c' && nsconf.home == NULL) {

        /*
         *  We will try to figure out our installation directory from
         *  executable binary.
         *  Check if nsd is in bin/ subdirectory according to our make install,
         *  if true make our home one level up, otherwise make home directory
         *  where executable binary resides.
         *  All custom installation will require "home" config parameter to be
         *  specified in the nsd.tcl
         */

        nsconf.home = MakePath("");
        if (nsconf.home == NULL) {
            Ns_Fatal("nsmain: missing: [%s]home", NS_CONFIG_PARAMETERS);
        }
    } else if (mode == 'c' && nsconf.config == NULL) {
	/*
	 * Try to get HOME from environment variable NAVISERVER. If
	 * this is not defined, take the value from the path. Using
	 * NAVISERVER makes especially sense when testing or running
	 * nsd from the source directory.
	 */
	nsconf.home = getenv("NAVISERVER");
	if (nsconf.home == NULL) {
	    nsconf.home = MakePath("");
	}
    }
    nsconf.home = SetCwd(nsconf.home);

    /* 
     * Make the result queryable.
     */
    set = Ns_ConfigCreateSection(NS_CONFIG_PARAMETERS);
    Ns_SetUpdate(set, "home", nsconf.home);

    /*
     * Update core config values.
     */

    NsConfUpdate();

    nsconf.tmpDir = Ns_ConfigGetValue(NS_CONFIG_PARAMETERS, "tmpdir");
    if (nsconf.tmpDir == NULL) {
	nsconf.tmpDir = getenv("TMPDIR");
	if (nsconf.tmpDir == NULL) {
	    nsconf.tmpDir = P_tmpdir;
	}
	Ns_SetUpdate(set, "tmpdir", nsconf.tmpDir);
    }

#ifdef _WIN32

    /*
     * Set the procname used for the pid file.
     */
    procname = (server ? server : Ns_SetKey(servers, 0));

    /*
     * Connect to the service control manager if running
     * as a service (see service comment above).
     */

    if (mode == 'I' || mode == 'R' || mode == 'S') {
	int status = TCL_OK;

        Ns_ThreadSetName("-service-");
        switch (mode) {
        case 'I':
            status = NsInstallService(procname);
            break;
        case 'R':
            status = NsRemoveService(procname);
            break;
        case 'S':
            status = NsConnectService();
            break;
        }
        return (status == NS_OK ? 0 : 1);
    }

 contservice:

#endif

    /*
     * Open the log file now that the home directory and runtime
     * user id have been set.
     */

    if (mode != 'c' && mode != 'f') {
        NsLogOpen();
    }

    /*
     * Log the first startup message which should be the first
     * output to the open log file unless the config script
     * generated some messages.
     */

    StatusMsg(starting);
    LogTclVersion();

#ifndef _WIN32

    /*
     * Log the current open file limit.
     */

    if (getrlimit(RLIMIT_NOFILE, &rl)) {
        Ns_Log(Warning, "nsmain: "
               "getrlimit(RLIMIT_NOFILE) failed: '%s'", strerror(errno));
    } else {
        if (rl.rlim_max == RLIM_INFINITY) {
            Ns_Log(Notice, "nsmain: "
                   "max files: FD_SETSIZE = %u, rl_cur = %u, rl_max = %s",
                   FD_SETSIZE, (unsigned int)rl.rlim_cur, "infinity");
        } else {
            Ns_Log(Notice, "nsmain: "
                   "max files: FD_SETSIZE = %u, rl_cur = %u, rl_max = %u",
                   FD_SETSIZE, (unsigned int)rl.rlim_cur, (unsigned int)rl.rlim_max);
        }
        if (rl.rlim_cur > FD_SETSIZE) {
            Ns_Log(Warning, "nsmain: rl_cur > FD_SETSIZE");
        }
    }

#endif

    /*
     * Create the pid file.
     */

    NsCreatePidFile();

    /*
     * Initialize the virtual servers.
     */

    if (server != NULL) {
        NsInitServer(server, initProc);
    } else {
        for (i = 0; i < Ns_SetSize(servers); ++i) {
            server = Ns_SetKey(servers, i);
            NsInitServer(server, initProc);
        }
    }
    nsconf.defaultServer = server;

    /*
     * Initialize non-server static modules.
     */

    NsInitStaticModules(NULL);

    /*
     * Run pre-startups and start the servers.
     */

    NsRunPreStartupProcs();
    NsStartServers();
    NsStartDrivers();

    /*
     * Signal startup is complete.
     */

    StatusMsg(running);

    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.started = 1;
    Ns_CondBroadcast(&nsconf.state.cond);
    Ns_MutexUnlock(&nsconf.state.lock);

    /*
     * Run any post-startup procs.
     */

    NsRunStartupProcs();

    /*
     * Start the drivers now that the server appears ready
     * and then close any remaining pre-bound sockets.
     */

#ifndef _WIN32
    NsClosePreBound();
    NsStopBinder();
#endif

    /*
     * Once the drivers listen thread is started, this thread will just
     * endlessly wait for Unix signals, calling NsRunSignalProcs()
     * whenever SIGHUP arrives.
     */

    sig = NsHandleSignals();

    /*
     * Print a "server shutting down" status message, set
     * the nsconf.stopping flag for any threads calling
     * Ns_InfoShutdownPending(), and set the absolute
     * timeout for all systems to complete shutown.
     * If SIGQUIT signal was sent, make immediate shutdown
     * without waiting for all subsystems to exit gracefully
     */

    StatusMsg(stopping);

    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.stopping = 1;
    if (sig == NS_SIGQUIT || nsconf.shutdowntimeout < 0) {
        nsconf.shutdowntimeout = 0;
    }
    Ns_GetTime(&timeout);
    Ns_IncrTime(&timeout, nsconf.shutdowntimeout, 0);
    Ns_MutexUnlock(&nsconf.state.lock);

    /*
     * First, stop the drivers and servers threads.
     */

    NsStopDrivers();
    NsStopServers(&timeout);
    NsStopSpoolers();

    /*
     * Next, start simultaneous shutdown in other systems and wait
     * for them to complete.
     */

    NsStartSchedShutdown();
    NsStartSockShutdown();
    NsStartTaskQueueShutdown();
    NsStartJobsShutdown();
    NsStartShutdownProcs();

    NsWaitSchedShutdown(&timeout);
    NsWaitSockShutdown(&timeout);
    NsWaitTaskQueueShutdown(&timeout);
    NsWaitJobsShutdown(&timeout);
    NsWaitDriversShutdown(&timeout);
    NsWaitShutdownProcs(&timeout);

    /*
     * Finally, execute the exit procs directly.  Note that
     * there is not timeout check for the exit procs so they
     * should be well behaved.
     */

    NsRunAtExitProcs();

    /*
     * Remove the pid maker file, print a final "server exiting"
     * status message and return to main.
     */

    NsRemovePidFile();
    StatusMsg(exiting);

    /*
     * The main thread exits gracefully on NS_SIGTERM.
     * All other signals are propagated to the caller.
     */

    return (sig == NS_SIGTERM) ? 0 : sig;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_WaitForStartup --
 *
 *      Blocks thread until the server has completed loading modules,
 *      sourcing Tcl, and is ready to begin normal operation.
 *
 * Results:
 *      NS_OK
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_WaitForStartup(void)
{

    /*
     * This dirty-read is worth the effort.
     */
    if (nsconf.state.started) {
        return NS_OK;
    }

    Ns_MutexLock(&nsconf.state.lock);
    while (!nsconf.state.started) {
        Ns_CondWait(&nsconf.state.cond, &nsconf.state.lock);
    }
    Ns_MutexUnlock(&nsconf.state.lock);
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StopSerrver --
 *
 *      Shutdown a server.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Server will begin shutdown process.
 *
 *----------------------------------------------------------------------
 */

void
Ns_StopServer(char *server)
{
    Ns_Log(Warning, "nsmain: immediate shutdown of server %s requested", server);
    NsSendSignal(NS_SIGTERM);
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclShutdownObjCmd --
 *
 *      Shutdown the server, waiting at most timeout seconds for threads
 *      to exit cleanly before giving up.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      If -restart was specified and watchdog is active, server
 *      will be restarted.
 *
 *----------------------------------------------------------------------
 */

int
NsTclShutdownObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int timeout = 0, signal = NS_SIGTERM;

    Ns_ObjvSpec opts[] = {
        {"-restart", Ns_ObjvBool,  &signal, (void *) NS_SIGINT},
        {"--",       Ns_ObjvBreak, NULL,    NULL},
        {NULL,       NULL,         NULL,    NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?timeout", Ns_ObjvInt, &timeout, NULL},
        {NULL,       NULL,       NULL,     NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        return TCL_ERROR;
    }

    Ns_MutexLock(&nsconf.state.lock);
    if (timeout > 0) {
        nsconf.shutdowntimeout = timeout;
    } else {
        timeout = nsconf.shutdowntimeout;
    }
    Ns_MutexUnlock(&nsconf.state.lock);

    NsSendSignal(signal);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(timeout));

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * StatusMsg --
 *
 *      Print a status message to the log file.  Initial messages log
 *      security status to ensure setuid()/setgid() works as expected.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
StatusMsg(runState state)
{
    char *what;

    switch (state) {
    case starting:
        what = "starting";
        break;
    case running:
        what = "running";
        break;
    case stopping:
        what = "stopping";
        break;
    case exiting:
        what = "exiting";
        break;
    default:
        what = "unknown";
        break;
    }
    Ns_Log(Notice, "nsmain: %s/%s %s",
           Ns_InfoServerName(), Ns_InfoServerVersion(), what);
#ifndef _WIN32
    if (state == starting || state == running) {
        Ns_Log(Notice, "nsmain: security info: uid=%d, euid=%d, gid=%d, egid=%d",
               (int)getuid(), (int)geteuid(), (int)getgid(), (int)getegid());
    }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * LogTclVersion --
 *
 *      Emit Tcl library version to server log.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
LogTclVersion(void)
{
    int major, minor, patch;

    Tcl_GetVersion(&major, &minor, &patch, NULL);
    Ns_Log(Notice, "nsmain: Tcl version: %d.%d.%d", major, minor, patch);

    return;
}


/*
 *----------------------------------------------------------------------
 *
 * UsageError --
 *
 *      Print a command line usage error message and exit.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Server exits.
 *
 *----------------------------------------------------------------------
 */

static void
UsageError(char *msg, ...)
{
    if (msg != NULL) {
    	va_list ap;
    	va_start(ap, msg);
    	fprintf(stderr, "\nError: ");
        vfprintf(stderr, msg, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
    fprintf(stderr, "\n"
#ifdef _WIN32
        "Usage: %s [-h|V] [-c|f|I|R|S] "
#else
        "Usage: %s [-h|V] [-c|f|i|w] "
        "[-u <user>] [-g <group>] [-r <path>] [-b <address:port>|-B <file>] "
#endif
        "[-s <server>] [-t <file>]\n"
        "\n"
        "  -h  help (this message)\n"
        "  -V  version and release information\n"
        "  -c  command (interactive) mode\n"
        "  -f  foreground mode\n"
#ifdef _WIN32
        "  -I  install Win32 service\n"
        "  -R  remove Win32 service\n"
        "  -S  start Win32 service\n"
#else
        "  -i  inittab mode\n"
        "  -w  watchdog mode (restart a failed server)\n"
        "  -d  debugger-friendly mode (ignore SIGINT)\n"
        "  -u  run as <user>\n"
        "  -g  run as <group>\n"
        "  -r  chroot to <path>\n"
        "  -b  bind <address:port>\n"
        "  -B  bind address:port list from <file>\n"
#endif
        "  -s  use server named <server> in config file\n"
        "  -t  read config from <file>\n"
        "\n", nsconf.argv0);
    exit(msg ? 1 : 0);
}

/*
 *----------------------------------------------------------------------
 *
 * MakePath --
 *
 *      Returns full path to the file relative to the base dir
 *
 * Results:
 *      Allocated full path or NULL
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static char *
MakePath(char *file)
{
    if (Ns_PathIsAbsolute(nsconf.nsd)) {
        char *str, *path = NULL;
        Tcl_Obj *obj;

        str = strstr(nsconf.nsd, "/bin/");
        if (str == NULL) {
            str = strrchr(nsconf.nsd, '/');
        }
        if (str == NULL) {
            return NULL;
        }

        /*
         * Make sure we have valid path on all platforms
         */

        obj = Tcl_NewStringObj(nsconf.nsd, (int)(str - nsconf.nsd));
        Tcl_AppendStringsToObj(obj, "/", file, NULL);

        Tcl_IncrRefCount(obj);
        if (Tcl_FSGetNormalizedPath(NULL, obj)) {
            path = (char *)Tcl_FSGetTranslatedStringPath(NULL, obj);
        }
        Tcl_DecrRefCount(obj);

        /*
         * If file name was given, check if the file exists
         */

        if (path != NULL && *file != 0 && access(path, F_OK) != 0) {
            ns_free(path);
            return NULL;
        }
        return path;
    }
    return NULL;
}


/*
 *----------------------------------------------------------------------
 *
 * SetCwd --
 *
 *      Changes the current working directory to the passed path.
 *
 * Results:
 *      Tcl_Alloc'ated string with the normalized path of the
 *      current working directory.
 *
 * Side effects:
 *      Kills server if unable to change to given directory
 *      or if the absolute normalized path of the directory
 *      could not be resolved.
 *
 *----------------------------------------------------------------------
 */

char *
SetCwd(char *path)
{
    Tcl_Obj *pathObj;

    pathObj = Tcl_NewStringObj(path, -1);
    Tcl_IncrRefCount(pathObj);
    if (Tcl_FSChdir(pathObj) == -1) {
        Ns_Fatal("nsmain: chdir(%s) failed: '%s'", path, strerror(Tcl_GetErrno()));
    }
    Tcl_DecrRefCount(pathObj);
    pathObj = Tcl_FSGetCwd(NULL);
    if (pathObj == NULL) {
        Ns_Fatal("nsmain: can't resolve home directory path");
    }

    return (char *)Tcl_FSGetTranslatedStringPath(NULL, pathObj);
}


/*
 *----------------------------------------------------------------------
 *
 * CmdThread --
 *
 *      Run a command shell accepting commands on standard input.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
CmdThread(void *arg)
{
    Args *cmd = arg;

    Ns_ThreadSetName("-command-");

    Ns_WaitForStartup();

    NsRestoreSignals();
    NsBlockSignal(NS_SIGPIPE);

    Tcl_Main(cmd->argc, cmd->argv, NsTclAppInit);
}
