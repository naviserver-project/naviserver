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

static const char *RCSID = "@(#) $Header$, compiled: " __DATE__ " " __TIME__;

#include "nsd.h"
#ifdef _WIN32
#define DEVNULL "nul:"
#else
#define DEVNULL "/dev/null"
#endif

/*
 * Local functions defined in this file.
 */

static int  StartWatchedServer(void);
static void SysLog(int priority, char *fmt, ...);
static void WatchdogSigtermHandler(int sig);
static int  WaitForServer();

static void UsageError(char *msg);
static void StatusMsg(int state);
static char *FindConfig(char *config);

#if (STATIC_BUILD == 1)
extern void NsthreadsInit();
extern void NsdInit();
#endif

/*
 * Setup timer/counter values for graceously waiting before trying 
 * to restart crippled server. This should be configurable from the
 * server config file (ns/watchdog section or alike).
 */

#define MAX_RESTART_SECONDS 64 /* Max time in sec to wait between restarts */
#define MIN_WORK_SECONDS   128 /* After being up for # secs, reset timers */
#define MAX_NUM_RESTARTS   256 /* Quit after somany unsuccessful restarts */

#ifndef _WIN32
# ifdef LOG_DEBUG
#  undef LOG_DEBUG /* Because this is used by the syslog facility as well */
# endif
# include <syslog.h>
# include <signal.h>
# include <stdarg.h>
# include <unistd.h>
static int watchdogExit = 0; /* Watchdog loop toggle */
#endif /* _WIN32 */


/*
 *----------------------------------------------------------------------
 *
 * Ns_Main --
 *
 *  The NaviServer startup routine called from main().  Startup is
 *  somewhat complicated to ensure certain things happen in the
 *  correct order.
 *
 * Results:
 *  Returns 0 to main() on final exit.
 *
 * Side effects:
 *  Many - read comments below.
 *
 *----------------------------------------------------------------------
 */

int
Ns_Main(int argc, char **argv, Ns_ServerInitProc *initProc)
{
    int  i, fd, sig;
    char *config;
    Ns_Time timeout;
    char buf[PATH_MAX];

#ifndef _WIN32
    int  uid = -1;
    int  gid = -1;
    int  debug = 0;
    int  mode = 0;
    char *root = NULL;
    char *garg = NULL;
    char *uarg = NULL;
    char *bindargs = NULL;
    char *bindfile = NULL;
    char *procname = NULL;
    char *server = NULL;
    Ns_Set *servers;
    struct rlimit  rl;
#else
    /*
     * The following variables are declared static so they
     * preserve their values when Ns_Main is re-entered by
     * the Win32 service control manager.
     */
    static int mode = 0;
    static Ns_Set *servers;
    static char *procname;
    static char *server;
#endif

    /*
     * For static builds only, we have to initialize
     * otherwise dynamically loaded shared libraries.
     */

#if (STATIC_BUILD == 1)
    NsthreadsInit();
    NsdInit();
#endif

    /*
     * Mark the server stopped until initialization is complete.
     */

    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.started = 0;
    Ns_MutexUnlock(&nsconf.state.lock);
    /*
     * When run as a Win32 service, Ns_Main will be re-entered
     * in the service main thread.  In this case, jump past
     * the point where the initial thread blocked when
     * connected to the service control manager.
     */
     
#ifdef _WIN32
    if (mode == 'S') {
        goto contservice;
    }
#endif

    /*
     * Set up configuration defaults and initial values.
     */

    nsconf.argv0 = argv[0];

    /*
     * NaviServer requires file descriptor 0 be open on /dev/null to
     * ensure the server never blocks reading stdin.
     */
     
    fd = open(DEVNULL, O_RDONLY);
    if (fd > 0) {
        dup2(fd, 0);
        close(fd);
    }

    /*     
     * Also, file descriptors 1 and 2 may not be open if the server
     * is starting from /etc/init.  If so, open them on /dev/null
     * as well because the server will assume they're open during
     * initialization.  In particular, the log file will be duped
     * to fd's 1 and 2.
     */

    fd = open(DEVNULL, O_WRONLY);
    if (fd > 0 && fd != 1) {
        close(fd);
    }
    fd = open(DEVNULL, O_WRONLY);
    if (fd > 0 && fd != 2) {
        close(fd);
    }

    /*
     * Parse the command line arguments.
     */

    opterr = 0;
    while ((i = getopt(argc, argv, "hpzifwVs:t:IRSkKdr:u:g:b:B:")) != -1) {
        switch (i) {
        case 'h':
            UsageError(NULL);
            break;
        case 'f':
        case 'i':
        case 'w':
        case 'V':
#ifdef _WIN32
        case 'I':
        case 'R':
        case 'S':
#endif
            if (mode != 0) {
#ifdef _WIN32
                UsageError("only one of -i, -f, -V, -I, -R, or -S may be specified");
#else
                UsageError("only one of -i, -f, -w, or -V may be specified");
#endif
            }
            mode = i;
            break;
        case 's':
            if (server != NULL) {
                UsageError("multiple -s <server> options");
            }
            server = optarg;
            break;
        case 't':
            if (nsconf.config != NULL) {
                UsageError("multiple -t <file> options");
            }
            nsconf.config = optarg;
            break;
        case 'p':
        case 'z':
            /* NB: Ignored. */
            break;
#ifndef _WIN32
        case 'b':
            bindargs = optarg;
            break;
        case 'B':
            bindfile = optarg;
            break;
        case 'r':
            root = optarg;
            break;
            break;
        case 'd':
            debug = 1;
            break;
        case 'g':
            garg = optarg;
            break;
        case 'u':
            uarg = optarg;
            break;
#endif
        case ':':
            sprintf(buf, "option -%c requires a parameter", optopt);
            UsageError(buf);
            break;
        default:
            sprintf(buf, "invalid option: -%c", optopt);
            UsageError(buf);
            break;
        }
    }
    if (mode == 'V') {
        printf("NaviServer/%s (%s)\n", NSD_VERSION, Ns_InfoLabel()); 
        printf("   CVS Tag:         %s\n", Ns_InfoTag());
        printf("   Built:           %s\n", Ns_InfoBuildDate());
        printf("   Tcl version:     %s\n", nsconf.tcl.version);
        printf("   Platform:        %s\n", Ns_InfoPlatform());
        return 0;
    } else if (nsconf.config == NULL) {
        UsageError("required -t <config> option not specified");
    }
    
    /*
     * Find the absolute config pathname and read the config data
     * before a possible chroot().
     */
    
    nsconf.config = FindConfig(nsconf.config);
    config = NsConfigRead(nsconf.config);
    
#ifndef _WIN32

    /*
     * Verify the uid/gid args.
     */
    
    if (uarg != NULL) {
        uid = Ns_GetUid(uarg);
        if (uid == -1) {
            int nc;
            /*
             * Hm, try see if given as numeric uid...
             */
            if (sscanf(uarg, "%d%n", &uid, &nc) != 1
                || nc != strlen(uarg)
                || Ns_GetNameForUid(NULL, uid) == NS_FALSE) {
                Ns_Fatal("nsmain: unknown user '%s'", uarg);
            }
            /*
             * Set user-passed value to NULL, causing supplementary 
             * groups to be ignored later.
             */
            uarg = NULL;
        }
        if (uarg != NULL) {
             gid = Ns_GetUserGid(uarg);
        } else {
            Ns_DString ds;
            Ns_DStringInit(&ds);
            if (Ns_GetNameForUid(&ds, uid) == NS_TRUE) {
                gid = Ns_GetUserGid(Ns_DStringValue(&ds));
            }
            Ns_DStringFree(&ds);
        }
    }
    if (garg != NULL) {
        gid = Ns_GetGid(garg);
        if (gid == -1) {
            int nc;
            if (sscanf(garg, "%d%n", (int*)&gid, &nc) != 1
                || nc != strlen(garg)
                || Ns_GetNameForGid(NULL, gid) == NS_FALSE) {
                Ns_Fatal("nsmain: unknown group '%s'", garg);
            }
        }
    }

    /*
     * If running as privileged user (root) check given user/group 
     * information and bail-out if any of them not really known.
     */

    if (getuid() == 0) {

        /*
         * OK, so the caller is running as root. In such cases
         * he/she should have used "-u" to give the actual user
         * to run as (may be root as well) and optionally "-g"
         * to set the process group. We're picky about the group
         * though. If we were not able to figure out to which
         * group the user belongs to, we will abort, no mercy.
         */

        if (uid == -1) {
            Ns_Fatal("nsmain: will not run without valid user; "
                     "must specify '-u username' parameter");
        }
        if (gid == -1) {
            Ns_Fatal("nsmain: will not run for unknown group; "
                     "must specify '-g group' parameter");
        }
    }
    
    /*
     * The server now uses poll() but Tcl and other components may
     * still use select() which will likely break when fd's exceed
     * FD_SETSIZE.  We now allow setting the fd limit above FD_SETSIZE,
     * but do so at your own risk.
     *
     * Note this limit must be set now to ensure it's inherited by
     * all future threads on certain platforms such as Linux.
     */

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        Ns_Log(Warning, "nsmain: getrlimit(RLIMIT_NOFILE) failed: '%s'",
               strerror(errno));
    } else {
        if (rl.rlim_cur != rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
                Ns_Log(Warning, "nsmain: "
                       "setrlimit(RLIMIT_NOFILE, %d) failed: '%s'",
                       rl.rlim_max, strerror(errno));
            } 
        }
    }
    
    /*
     * Fork into the background and create a new session.
     */

    if (mode == 0 || mode == 'w') {
        i = ns_fork();
        if (i == -1) {
            Ns_Fatal("nsmain: fork() failed: '%s'", strerror(errno));
        }
        if (i > 0) {
            return 0;
        }
        setsid(); /* Detaches from the controlling terminal device */
    }

    /*
     * Optionally, start the watchdog/server process pair.
     * The watchdog process will monitor and restart the server unless 
     * the server exits gracefully, either by calling exit(0) or get 
     * signalled by the SIGTERM signal.
     * The watchdog process itself will exit when the server process 
     * exits gracefully, or, when get signalled by the SIGTERM signal. 
     * In the latter case, watchdog will pass the SIGTERM to the server 
     * process, so both of them will gracefully terminate.
     */

    if (mode == 'w') {
        if (StartWatchedServer() == 0) {
            return 0;
        }
    } else {
        nsconf.pid = getpid();
    }
    
    /*
     * Pre-bind any sockets now, before a possible setuid from root
     * or chroot which may hide /etc/resolv.conf required to
     * resolve name-based addresses.
     */

    NsPreBind(bindargs, bindfile);

    /*
     * Chroot() if requested before setuid from root.
     */

    if (root != NULL) {
        if (chroot(root) != 0) {
            Ns_Fatal("nsmain: chroot(%s) failed: '%s'", root, strerror(errno));
        }
        if (chdir("/") != 0) {
            Ns_Fatal("nsmain: chdir(/) failed: '%s'", strerror(errno));
        }
        nsconf.home = "/";
    }

    /*
     * If caller is running as the privileged user, change 
     * to the run time (given) user and/or group now.
     */

    if (getuid() == 0) {
        
        /*
         * Set or clear supplementary groups.
         */
        
        if (uarg != NULL) {
            if (initgroups(uarg, (gid_t)gid) != 0) {
                Ns_Fatal("nsmain: initgroups(%s, %d) failed: '%s'",
                         uarg, gid, strerror(errno));
            }
        } else {
            if (setgroups(0, NULL) != 0) {
                Ns_Fatal("nsmain: setgroups(0, NULL) failed: '%s'",
                         strerror(errno));
            }
        }
        
        if (gid != getgid() && setgid((gid_t)gid) != 0) {
            Ns_Fatal("nsmain: setgid(%d) failed: '%s'", gid, strerror(errno));
        }
        if (setuid((uid_t)uid) != 0) {
            Ns_Fatal("nsmain: setuid(%d) failed: '%s'", uid, strerror(errno));
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
#endif
        
    /*
     * Finally, block all signals for the duration of startup to ensure any
     * new threads inherit the blocked state.
     */

    NsBlockSignals(debug);

#endif

    /*
     * Initialize Tcl and eval the config file.
     */

    Tcl_FindExecutable(argv[0]);
    nsconf.nsd = (char *) Tcl_GetNameOfExecutable();
    NsConfigEval(config, argc, argv, optind);
    ns_free(config);
    
    /*
     * Ensure servers where defined.
     */

    servers = Ns_ConfigGetSection("ns/servers");
    if (servers == NULL || Ns_SetSize(servers) == 0) {
        Ns_Fatal("nsmain: no servers defined");
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
     * Set the procname used for the pid file.
     */
    
    procname = (server ? server : Ns_SetKey(servers, 0));
    
    /*
     * Verify and change to the home directory.
     */
    
    nsconf.home = Ns_ConfigGetValue(NS_CONFIG_PARAMETERS, "home");
    if (nsconf.home == NULL) {
        Ns_Fatal("nsmain: missing: [%s]home", NS_CONFIG_PARAMETERS);
    }
    if (chdir(nsconf.home) != 0) {
        Ns_Fatal("nsmain: chdir(%s) failed: '%s'", nsconf.home, strerror(errno));
    }
    
#ifdef _WIN32

    /*
     * On Win32, first perform some additional cleanup of
     * home, ensuring forward slashes and lowercase.
     */
    
    nsconf.home = getcwd(buf, sizeof(buf));
    if (nsconf.home == NULL) {
        Ns_Fatal("nsmain: getcwd failed: '%s'", strerror(errno));
    }
    while (*nsconf.home != '\0') {
        if (*nsconf.home == '\\') {
            *nsconf.home = '/';
        } else if (isupper(*nsconf.home)) {
            *nsconf.home = tolower(*nsconf.home);
        }
        ++nsconf.home;
    }
    nsconf.home = buf;
    
    /*
     * Then, connect to the service control manager if running
     * as a service (see service comment above).
     */
    
    if (mode == 'I' || mode == 'R' || mode == 'S') {
        int status;
        
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
     * Update core config values.
     */
    
    NsConfUpdate();

    /*
     * Open the log file now that the home directory and runtime
     * user id have been set.
     */
     
    if (mode != 'f') {
        NsLogOpen();
    }

    /*
     * Log the first startup message which should be the first
     * output to the open log file unless the config script 
     * generated some messages.
     */
     
    StatusMsg(0);

#ifndef _WIN32

    /*
     * Log the current open file limit.
     */
     
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        Ns_Log(Warning, "nsmain: "
               "getrlimit(RLIMIT_NOFILE) failed: '%s'", strerror(errno));
    } else {
        Ns_Log(Notice, "nsmain: "
               "max files: FD_SETSIZE = %d, rl_cur = %d, rl_max = %d",
               FD_SETSIZE, rl.rlim_cur, rl.rlim_max);
        if (rl.rlim_max > FD_SETSIZE) {
            Ns_Log(Warning, "nsmain: rl_max > FD_SETSIZE");
        }
    }

#endif

    /*
     * Create the pid file used.
     */

    NsCreatePidFile(procname);

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
    
    /*
     * Load non-server modules.
     */

    NsLoadModules(NULL);

    /*
     * Run pre-startups and start the servers.
     */

    NsRunPreStartupProcs();
    NsStartServers();

    /*
     * Signal startup is complete.
     */

    StatusMsg(1);
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

    NsStartDrivers();
#ifndef _WIN32
    NsClosePreBound();
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
     */

    StatusMsg(2);
    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.stopping = 1;
    if (nsconf.shutdowntimeout < 0) {
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

    /*
     * Next, start simultaneous shutdown in other systems and wait
     * for them to complete.
     */

    NsWaitDriversShutdown(&timeout);
    NsStartSchedShutdown(); 
    NsStartSockShutdown();
    NsStartJobsShutdown();
    NsStartShutdownProcs();
    NsWaitSchedShutdown(&timeout);
    NsWaitSockShutdown(&timeout);
    NsWaitJobsShutdown(&timeout);
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

    NsRemovePidFile(procname);
    StatusMsg(3);

    /*
     * The server exits gracefully on NS_SIGTERM.
     * All other signals are propagated to the caller.
     */

    return (sig == NS_SIGTERM) ? 0 : sig;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_WaitForStartup --
 *
 *  Blocks thread until the server has completed loading modules, 
 *  sourcing Tcl, and is ready to begin normal operation. 
 *
 * Results:
 *  NS_OK/NS_ERROR 
 *
 * Side effects:
 *  None. 
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
 *  Shutdown a server.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Server will begin shutdown process. 
 *
 *----------------------------------------------------------------------
 */

void
Ns_StopServer(char *server)
{
    Ns_Log(Warning, "nsmain: immediate server shutdown requested");
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
NsTclShutdownObjCmd(ClientData dummy, Tcl_Interp *interp, int objc, Tcl_Obj **objv)
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
    Tcl_SetIntObj(Tcl_GetObjResult(interp), timeout);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * StatusMsg --
 *
 *  Print a status message to the log file.  Initial messages log
 *  security status to ensure setuid()/setgid() works as expected.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static void
StatusMsg(int state)
{
    char *what;

    switch (state) {
    case 0:
        what = "starting";
        break;
    case 1:
        what = "running";
        break;
    case 2:
        what = "stopping";
        break;
    case 3:
        what = "exiting";
        break;
    default:
        what = "unknown";
        break;
    }
    Ns_Log(Notice, "nsmain: %s/%s %s",
           Ns_InfoServerName(), Ns_InfoServerVersion(), what);
#ifndef _WIN32
    if (state < 2) {
        Ns_Log(Notice, "nsmain: security info: uid=%d, euid=%d, gid=%d, egid=%d",
               getuid(), geteuid(), getgid(), getegid());
    }
#endif
}


/*
 *----------------------------------------------------------------------
 *
 * UsageError --
 *
 *  Print a command line usage error message and exit.
 *
 * Results:
 *  None.
 *
 * Side effects:
 *  Server exits.
 *
 *----------------------------------------------------------------------
 */

static void
UsageError(char *msg)
{
    if (msg != NULL) {
    fprintf(stderr, "\nError: %s\n", msg);
    }
    fprintf(stderr, "\n"
        "Usage: %s [-h|V] [-i|f] "
#ifdef _WIN32
        "[-I|R] "
#else
        "[-u <user>] [-g <group>] [-r <path>] [-b <address:port>|-B <file>] "
#endif
        "[-s <server>] -t <file>\n"
        "\n"
        "  -h  help (this message)\n"
        "  -V  version and release information\n"
        "  -i  inittab mode\n"
        "  -f  foreground mode\n"
        "  -w  watchdog mode: restart a failed server\n"
#ifdef _WIN32
        "  -I  Install win32 service\n"
        "  -R  Remove win32 service\n"
#else
        "  -d  debugger-friendly mode (ignore SIGINT)\n"
        "  -u  run as <user>\n"
        "  -g  run as <group>\n"
        "  -r  chroot to <path>\n"
        "  -b  bind <address:port>\n"
        "  -B  bind address:port list from <file>\n"
#endif
        "  -s  use server named <server> in config file\n"
        "  -t  read config from <file> (REQUIRED)\n"
        "\n", nsconf.argv0);
    exit(msg ? 1 : 0);
}
    

/*
 *----------------------------------------------------------------------
 *
 * FindConfig --
 *
 *  Find the absolute pathname to the config and clean it up.
 *
 * Results:
 *  ns_malloc'ed string with clean path.
 *
 * Side effects:
 *  Config path is "normalized".
 *
 *----------------------------------------------------------------------
 */

char *
FindConfig(char *config)
{
    Ns_DString ds1, ds2;
    char cwd[PATH_MAX];

    Ns_DStringInit(&ds1);
    Ns_DStringInit(&ds2);
    if (!Ns_PathIsAbsolute(config) && getcwd(cwd, sizeof(cwd)) != NULL) {
        Ns_MakePath(&ds2, cwd, config, NULL);
        config = ds2.string;
    }
    Ns_NormalizePath(&ds1, config);
    config = Ns_DStringExport(&ds1);
    Ns_DStringFree(&ds2);
    return config;
}

/*
 *----------------------------------------------------------------------
 *
 * SysLog --
 *
 *      Logs a message to the system log facility
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
SysLog(int priority, char *fmt, ...)
{
    va_list ap;

    openlog("nsd", LOG_CONS | LOG_NDELAY | LOG_PID, LOG_DAEMON);
    va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);
    closelog();    
}

/*
 *----------------------------------------------------------------------
 *
 * WatchdogSigtermHandler --
 *
 *      Handle SIGTERM and pass to server process.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Watchdog will not restart the server.
 *
 *----------------------------------------------------------------------
 */

static void 
WatchdogSigtermHandler(int sig)
{
    kill((pid_t) nsconf.pid, sig);
    watchdogExit = 1;
}

/*
 *----------------------------------------------------------------------
 *
 * WaitForServer --
 *
 *      Waits for the server process to exit or die due to an uncaught
 *      signal.
 *
 * Results:
 *      NS_OK if the server exited cleanly, NS_ERROR otherwise.
 *
 * Side effects:
 *      May wait forever...
 *
 *----------------------------------------------------------------------
 */

static int 
WaitForServer()
{
    int ret, status;
    pid_t pid;
    char *msg;

    do {
        pid = waitpid(nsconf.pid, &status, 0);
    } while (pid == -1 && errno == EINTR);

    if (WIFEXITED(status)) {
        ret = WEXITSTATUS(status);
        msg = "exited";
    } else if (WIFSIGNALED(status)) {
        ret = WTERMSIG(status);
        msg = "terminated";
    } else {
        msg = "killed";
        ret = -1; /* Some waitpid (or other unknown) failure? */
    }

    SysLog(LOG_NOTICE, "watchdog: server %d %s (%d).", nsconf.pid, msg, ret);

    return ret ? NS_ERROR : NS_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * StartWatchedServer --
 *
 *      Restart the server process until it exits 0 or we exceed the
 *      maximum number of restart attempts.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Install SIGTERM handler for watchdog process.
 *      Sets the global nsconf.pid with the process ID of the server.
 *
 *----------------------------------------------------------------------
 */

static int
StartWatchedServer(void)
{
    unsigned int setSigterm = 0, startTime, numRestarts = 0, restartWait = 0;

    SysLog(LOG_NOTICE, "watchdog: started.");

    do {
        if (restartWait) {
            SysLog(LOG_WARNING, 
                   "watchdog: waiting %d seconds before restart %d.",
                   restartWait, numRestarts);
            sleep(restartWait);
        }
        nsconf.pid = ns_fork();
        if (nsconf.pid == -1) {
            SysLog(LOG_ERR, "watchdog: fork() failed: '%s'.", strerror(errno));
            Ns_Fatal("watchdog: fork() failed: '%s'.", strerror(errno));
        }
        if (nsconf.pid == 0) {
            /* Server process. */
            nsconf.pid = getpid();
            SysLog(LOG_NOTICE, "server: started.");
            return nsconf.pid;
        }

        /* Watchdog process */

        /*
         * Register SIGTERM handler so we can gracefully stop the server. 
         * The watchdog will exit w/o stopping the server if got signalled 
         * with any other signal, though.
         */

        if (setSigterm == 0) {
            setSigterm = 1;
            ns_signal(SIGTERM, WatchdogSigtermHandler);
        }

        startTime = time(NULL);

        if (WaitForServer() == NS_OK) {
            break;
        }

        if ((time(NULL) - startTime) > MIN_WORK_SECONDS) {
            restartWait = numRestarts = 0;
        }

        if (++numRestarts > MAX_NUM_RESTARTS) {
            SysLog(LOG_WARNING, "watchdog: exceeded restart limit of %d",
                   MAX_NUM_RESTARTS);
            break;
        }

        restartWait *= 2;
        if (restartWait > MAX_RESTART_SECONDS) {
            restartWait = MAX_RESTART_SECONDS;
        } else if (restartWait == 0) {
            restartWait = 1;
        }

    } while (!watchdogExit);

    SysLog(LOG_NOTICE, "watchdog: exited.");

    return 0;
}
