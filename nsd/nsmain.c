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
    char     **argv;
    TCL_SIZE_T argc;
} Args;

/*
 * This one is used to better track run state
 * when looking at the code
 */

typedef enum {
    starting_state,  /* == 0 */
    running_state,   /* == 1 */
    stopping_state,  /* == 2 */
    exiting_state    /* == 3 */
} runState;

/*
 * Local functions defined in this file.
 */

static Ns_ThreadProc CmdThread;

static void UsageError(const char *msg, ...) NS_GNUC_NONNULL(1) NS_GNUC_PRINTF(1, 2) NS_GNUC_NORETURN;
static void UsageMsg(int exitCode)           NS_GNUC_NORETURN;
static void StatusMsg(runState state);
static void LogTclVersion(void);
static const char *MakePath(const char *file) NS_GNUC_NONNULL(1);
static const char *SetCwd(const char *path) NS_GNUC_NONNULL(1);

#if defined(STATIC_BUILD) && (STATIC_BUILD == 1)
extern void NsthreadsInit();
extern void NsdInit();
#endif

static const char *configParametersReverseproxySection = "ns/parameters/reverseproxymode";
static const char *configServersSection = "ns/servers";

/*
 * Used by other files as well.
 */
const char *NS_EMPTY_STRING = "";


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
Ns_Main(int argc, char *const* argv, Ns_ServerInitProc *initProc)
{
    Args           cmd;
    int            sig, optionIndex;
    Ns_Time        timeout;
    Ns_Set        *set;
    bool           testMode = NS_FALSE;
    const char    *configFileContent = NULL;
#ifndef _WIN32
    bool           debug = NS_FALSE;
    bool           forked = NS_FALSE;
    char           mode = '\0';
    const char    *root = NULL, *garg = NULL, *uarg = NULL, *server = NULL;
    const char    *bindargs = NULL, *bindfile = NULL;
    Ns_Set        *servers;
    Ns_ReturnCode  status;
#else
    /*
     * The following variables are declared static so they
     * preserve their values when Ns_Main is re-entered by
     * the Win32 service control manager.
     */
    static char    mode = '\0', *procname, *server = NULL;
    static Ns_Set *servers;
#endif

    /*
     * Before doing anything else, initialize the Tcl API
     * as we rely heavily on it, even for the most basic
     * functions like memory allocation.
     */

    Tcl_FindExecutable(argv[0]);

    /*
     * When run as a Win32 service, Ns_Main will be re-entered
     * in the service main thread. In this case, jump past the
     * point where the initial thread blocked when connected to
     * the service control manager.
     */

#ifdef _WIN32
    if (mode == 'S') {
        Ns_ThreadSetName("-service-");
        goto contservice;
    }
#endif

    nsconf.argv0 = argv[0];
    nsconf.argvObj = Tcl_NewListObj(0, NULL);
    Tcl_IncrRefCount(nsconf.argvObj);
    for (optionIndex = 1; optionIndex < argc; optionIndex++) {
        Tcl_ListObjAppendElement(NULL, nsconf.argvObj,
                                 Tcl_NewStringObj( argv[optionIndex], TCL_INDEX_NONE));
    }


    /*
     * Parse the command line arguments.
     */

    for (optionIndex = 1; optionIndex < argc; optionIndex++) {
        if (argv[optionIndex][0] != '-') {
            break;
        }
        switch (argv[optionIndex][1]) {
        case 'h':
            UsageMsg(0);

        case 'c': NS_FALL_THROUGH; /* fall through */
        case 'f': NS_FALL_THROUGH; /* fall through */
#ifdef _WIN32
        case 'I': NS_FALL_THROUGH; /* fall through */
        case 'R': NS_FALL_THROUGH; /* fall through */
        case 'S': NS_FALL_THROUGH; /* fall through */
#else
        case 'i': NS_FALL_THROUGH; /* fall through */
        case 'w': NS_FALL_THROUGH; /* fall through */
#endif
        case 'V':
            if (mode != '\0') {
#ifdef _WIN32
                UsageError("only one of the options -c, -f, -I, -R, -S or -V"
                           " may be specified");
#else
                UsageError("only one of the options -c, -f, -i, -w or -V"
                           " may be specified");
#endif
            }
            mode = argv[optionIndex][1];
            break;
        case 's':
            if (server != NULL) {
                UsageError("multiple -s <server> options");
            }
            if (optionIndex + 1 < argc) {
                server = argv[++optionIndex];
            } else {
                UsageError("no parameter for -s option");
            }
            break;
        case 't':
            if (nsconf.configFile != NULL) {
                UsageError("multiple -t <file> options");
            }
            if (optionIndex + 1 < argc) {
                nsconf.configFile = argv[++optionIndex];
            } else {
                UsageError("no parameter for -t option");
            }
            break;
        case 'T':
            testMode = NS_TRUE;
            break;

        case 'p':
        case 'z':
            /* NB: Ignored. */
            break;
#ifndef _WIN32
        case 'b':
            if (optionIndex + 1 < argc) {
                bindargs = argv[++optionIndex];
            } else {
                UsageError("no parameter for -b option");
            }
            break;
        case 'B':
            if (optionIndex + 1 < argc) {
                bindfile = argv[++optionIndex];
            } else {
                UsageError("no parameter for -B option");
            }
            break;
        case 'r':
            if (optionIndex + 1 < argc) {
                root = argv[++optionIndex];
            } else {
                UsageError("no parameter for -r option");
            }
            break;
        case 'd':
            debug = NS_TRUE;
            break;
        case 'g':
            if (optionIndex + 1 < argc) {
                garg = argv[++optionIndex];
            } else {
                UsageError("no parameter for -g option");
            }
            break;
        case 'u':
            if (optionIndex + 1 < argc) {
                uarg = argv[++optionIndex];
            } else {
                UsageError("no parameter for -u option");
            }
            break;
#endif
        default:
            UsageError("invalid option: -%c", argv[optionIndex][1]);
        }
    }
    if (mode == 'V') {
        printf("%s/%s\n", PACKAGE_NAME, PACKAGE_VERSION);
        printf("   Tag:             %s\n", Ns_InfoTag());
        printf("   Version:         %d\n", NS_VERSION_NUM);
        printf("   Built:           %s\n", nsBuildDate);
        printf("   Tcl version:     %s\n", TCL_PATCH_LEVEL);
        printf("   Platform:        %s\n", Ns_InfoPlatform());
        return 0;
    }

    /*
     * Initialize the Nsd library.
     */

    Nsd_LibInit();

    /*
     * Mark the server stopped until initialization is complete.
     */

    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.started = NS_FALSE;
    Ns_MutexUnlock(&nsconf.state.lock);

    /*
     * We need the name of the executable already in the test mode.
     */
    nsconf.nsd = ns_strdup(Tcl_GetNameOfExecutable());

    if (testMode) {
        const char *fileContent;

        if (nsconf.configFile == NULL) {
            UsageError("option -t <file> must be provided, when -T is used");
        }
        fileContent = NsConfigRead(nsconf.configFile);
        if (fileContent != NULL) {

            /*
             * Evaluate the configuration file.
             */
            NsConfigEval(fileContent, nsconf.configFile, argc, argv, optionIndex);

            printf("%s/%s: configuration file %s looks OK\n",
                   PACKAGE_NAME, PACKAGE_VERSION, nsconf.configFile);

            ns_free((char *)fileContent);
        }
        return 0;
    }

    if (mode == 'c') {
        int i;

        cmd.argv = ns_calloc(((size_t)argc - (size_t)optionIndex) + 2u, sizeof(char *));
        cmd.argc = 0;
        cmd.argv[cmd.argc++] = argv[0];
        for (i = optionIndex; i < argc; i++) {
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
#ifdef HAVE_COREFOUNDATION
        Ns_Fatal("nsmain: Tcl compiled with Core Foundation support does not support forking modes; "
                 "use e.g. the '-i' mode parameter in the command line.\n");
#else
        int i;

        if (mode != 'w') {
            /*
             * Unless we are in watchdog mode, setup pipe for realizing
             * nonzero return codes in case setup fails.
             *
             * Background: The pipe is used for communicating problems during
             * startup from the child process to return nonzero return codes
             * in case the server does not start up. However, the watchdog
             * mode restarts the child if necessary, so the pipe to the child
             * can't be used.
             */
            ns_pipe(nsconf.state.pipefd);
        }

        i = ns_fork();
        if (i == -1) {
            Ns_Fatal("nsmain: fork() failed: '%s'", strerror(errno));
        }
        if (i > 0) {
            /*
             * We are in the parent process.
             */
            int exit_code;

            /*
             * Pipe communication between parent and child communication is
             * NOT established in watchdog mode.
             */
            if (mode != 'w') {
                char    buf = '\0';
                ssize_t nread;

                /*
                 * Close the write-end of the pipe, we do not use it
                 */
                ns_close(nsconf.state.pipefd[1]);
                nsconf.state.pipefd[1] = 0;

                Ns_Log(Debug, "nsmain: wait for feedback from forked child, read from fd %d",
                       nsconf.state.pipefd[0]);

                /*
                 * Read the status from the child process. We expect as result
                 * either 'O' (when initialization went OK) or 'F' (for Fatal).
                 */
                nread = ns_read(nsconf.state.pipefd[0], &buf, 1);
                if (nread < 0) {
                    /*
                     * Do nothing, even when the read fails
                     */
                    ;
                    Ns_Log(Warning, "nsmain: received no feedback from child process, error: %s", strerror(errno));
                }
                Ns_Log(Debug, "nsmain: received from child %" PRIdz " bytes", nread);
                ns_close(nsconf.state.pipefd[0]);
                nsconf.state.pipefd[0] = 0;
                exit_code = (buf == 'O') ? 0 : 1;

            } else {
                /*
                 * If we are not in watchdog mode, the parent will return
                 * after a successful for always with 0.
                 */
                exit_code = 0;
            }

            return exit_code;
        }
        /*
         * We are in the child process.
         */
        if (mode != 'w') {
            /*
             * Unless we are in the watchdog mode, close the read-end of the
             * pipe, we do not use it.
             */
            ns_close(nsconf.state.pipefd[0]);
            nsconf.state.pipefd[0] = 0;
        }

        forked = NS_TRUE;
        setsid(); /* Detach from the controlling terminal device */
#endif
    }

    /*
     * For watchdog mode, start the watchdog/server process pair.
     * The watchdog will monitor and restart the server unless the
     * server exits gracefully, either by calling exit(0) or get
     * signaled by the SIGTERM signal.
     * The watchdog itself will exit when the server exits gracefully,
     * or, when get signaled by the SIGTERM signal. In the latter
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
        forked = NS_TRUE;
    }

   /*
    * Keep up the C-compatibility with Tcl 8.4.
    */

    if (forked) {
        int major, minor;

        Tcl_GetVersion(&major, &minor, NULL, NULL);
        if (major == 8 && minor <= 4) {

           /*
            * For Tcl versions up to (and including the) Tcl 8.4
            * we need to re-init the notifier after the fork.
            * Failing to do so will make Tcl_ThreadAlert (et.al.)
            * unusable since the notifier subsystem may not be
            * initialized. The problematic behavior may be exhibited
            * for any loadable module that creates threads using the
            * Tcl API but never calls directly into Tcl_CreateInterp
            * that handles the notifier initialization indirectly.
            */

            Tcl_InitNotifier();
        }
    }

    nsconf.pid = getpid();

    /*
     * Block all signals for the duration of startup to ensure any new
     * threads inherit the blocked state.
     */

    NsBlockSignals(debug);

#endif /* ! _WIN32 */

    /*
     * Find and read configuration file, if given at the command line, just use it,
     * if not specified, try to figure out by looking in the current dir for
     * nsd.tcl and for ../conf/nsd.tcl
     */

    if (nsconf.configFile == NULL) {
        nsconf.configFile = MakePath("nsd.tcl");
        if (nsconf.configFile == NULL) {
            nsconf.configFile = MakePath("conf/nsd.tcl");
        }
    }

    /*
     * In case chroot has to be performed, we might not be able anymore to
     * read the configuration file. So, we have to read it before issuing the
     * chroot() command.
     */
    if (nsconf.configFile != NULL) {
        configFileContent = NsConfigRead(nsconf.configFile);
    }

#ifndef _WIN32

    if (bindargs != NULL || bindfile != NULL) {
        /*
         * Pre-bind any sockets now, before a possible setuid from root
         * or chroot which may hide /etc/resolv.conf required to resolve
         * name-based addresses.
         */
        status = NsPreBind(bindargs, bindfile);
        if (status != NS_OK) {
            Ns_Fatal("nsmain: prebind failed");
        }
    }

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
        if (bindargs != NULL || bindfile != NULL) {
            NsForkBinder();
        }

        if (Ns_SetUser(uarg) == NS_ERROR) {
            Ns_Fatal("nsmain: failed to switch to user %s", uarg);
        }
    } else {
        if (uarg != NULL) {
            Ns_Log(Warning, "nsmain: command line argument '-u %s' is ignored"
                   " since not running as a privileged user", uarg);
        }
        if (garg != NULL) {
            Ns_Log(Warning, "nsmain: command line argument '-g %s' is ignored"
                   " since not running as a privileged user", garg);
        }
    }
#ifndef _WIN32
    {
        Tcl_DString dsName, dsGroup;

        Tcl_DStringInit(&dsName);
        Tcl_DStringInit(&dsGroup);
        Ns_GetNameForUid(&dsName, getuid());
        Ns_GetNameForGid(&dsGroup, getgid());
        Ns_Log(Notice, "Running nsd with user '%s' and group '%s'", dsName.string, dsGroup.string);
        Tcl_DStringFree(&dsName);
        Tcl_DStringFree(&dsGroup);
    }
#endif

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

    if (configFileContent != NULL) {
        /*
         * Evaluate the configuration file.
         */
        NsConfigEval(configFileContent, nsconf.configFile, argc, argv, optionIndex);
        ns_free((char *)configFileContent);
    }

    /*
     * This is the first place, where we can use values from the configuration
     * file.
     *
     * In order to debug configuration values, the following severity setting
     * might be useful.
     */
    /*Ns_LogSeveritySetEnabled(Ns_LogNsSetDebug, NS_TRUE);*/

    /*
     * Internationalized programs must call setlocale() to initiate specific
     * language operations.
     */
    {
        char *localeString =  getenv("LC_ALL");
        const char *source = "environment variable LC_ALL", *response;

        if (localeString == NULL) {
            source = "environment variable LC_COLLATE";
            localeString =  getenv("LC_COLLATE");
        }
        if (localeString == NULL) {
            source = "environment variable LANG";
            localeString =  getenv("LANG");
        }
        if (localeString == NULL) {
            source = "system-wide default locale";
            localeString =  setlocale(LC_COLLATE, NULL);
        }

#ifdef _WIN32
        if (localeString != NULL) {
            /*
             * Under windows, later calls to setlocale() overwrite the
             * returned string and the pointer will be invalid.
             */
            localeString = ns_strdup(localeString);
        }
#endif

        response = setlocale(LC_COLLATE, localeString);
        if (response != NULL) {
#ifdef _WIN32
            nsconf.locale = _create_locale(LC_COLLATE, localeString);
#else
            nsconf.locale = newlocale(LC_COLLATE_MASK, localeString, (locale_t)0);
#endif
        }
        if (nsconf.locale == 0) {
            Ns_Fatal("nsmain: system configuration mismatch.\n"
                     "The provided locale '%s' based on the %s is not installed on this system.\n"
                     "On unix-like systems you can check the available locales with the command 'locale -a' .\n",
                               localeString, source);
        }
        Ns_Log(Notice, "initialized locale %s from %s", localeString, source);
    }

    /*
     * Turn on logging of long mutex calls if desired. For whatever reason, we
     * can't access NS_mutexlocktrace from here (unknown external symbol),
     * although it is defined exactly like NS_finalshutdown;
     */
#ifndef _WIN32
    NS_mutexlocktrace = Ns_ConfigBool(NS_GLOBAL_CONFIG_PARAMETERS, "mutexlocktrace", NS_FALSE);
#endif

    nsconf.formFallbackCharset =
        ns_strcopy(Ns_ConfigString(NS_GLOBAL_CONFIG_PARAMETERS, "formfallbackcharset", NULL));
    if (nsconf.formFallbackCharset != NULL
        && *nsconf.formFallbackCharset == '\0') {
        nsconf.formFallbackCharset  = NULL;
    }

    /*
     * If no servers were defined, autocreate server "default"
     * so all default config values will be used for that server
     */

    servers = Ns_ConfigGetSection(configServersSection);
    if (servers == NULL) {
        servers = Ns_ConfigCreateSection(configServersSection);
    }
    if (Ns_SetSize(servers) == 0u) {
        (void)Ns_SetPutSz(servers, "default", 7, "Default NaviServer", 18);
    }

    /*
     * If a single server was specified, ensure it exists
     * and update the pointer to the config string (the
     * config server strings are considered the unique
     * server "handles").
     */

    if (server != NULL) {
        int idx = Ns_SetFind(servers, server);
        if (idx < 0) {
            Ns_Log(Error, "nsmain: no such server '%s' in configuration file '%s'",
                   server, nsconf.configFile);
            Ns_Log(Warning, "nsmain: Writing known server names stderr:");
            Ns_SetPrint(NULL, servers);
            Ns_Fatal("nsmain: no such server '%s'", server);
        }
        server = Ns_SetKey(servers, idx);
    }

    /*
     * Verify and change to the home directory.
     */
    nsconf.home = Ns_ConfigGetValue(NS_GLOBAL_CONFIG_PARAMETERS, "home");
    if (nsconf.home == NULL && mode != 'c') {

        /*
         *  We will try to figure out our installation directory from
         *  executable binary.  Check if nsd is in bin/ subdirectory according
         *  to our make install, if true make our home one level up, otherwise
         *  make home directory where executable binary resides.  All custom
         *  installation will require "home" config parameter to be specified
         *  in the nsd.tcl
         */

        nsconf.home = MakePath("");
        if (nsconf.home == NULL) {
            Ns_Fatal("nsmain: missing: [%s]home", NS_GLOBAL_CONFIG_PARAMETERS);
        }
    } else if (nsconf.home == NULL /* && mode == 'c' */) {
        /*
         * Try to get HOME from environment variable NAVISERVER. If
         * this is not defined, take the value from the path. Using
         * NAVISERVER makes especially sense when testing or running
         * nsd from the source directory.
         */
        nsconf.home = getenv("NAVISERVER");
        if (nsconf.home == NULL) {
            /*
             * There is no such environment variable. Try, if we can get the
             * home from the binary. In such cases, we expect to find
             * "bin/init.tcl" under home.
             */
            const char *path = MakePath("bin/init.tcl");
            if (path != NULL) {
                /*
                 * Yep, we found it, use its parent directory.
                 */
                nsconf.home =  MakePath("");
            } else {
                /*
                 * Desperate fallback. Use the name of the configured install
                 * directory.
                 */
                nsconf.home = NS_NAVISERVER;
            }
        }
        assert(nsconf.home != NULL);
    }
    nsconf.home = SetCwd(nsconf.home);

    /*
     * The value of nsconf.home is set. We can use it now as the base
     * directory for completion for "logdir" and "bindir" in case they are
     * relative. The logdir is required early in the startup to be usable as a
     * base directory for e.g. the pid file.
     */
    nsconf.logDir = Ns_ConfigFilename(NS_GLOBAL_CONFIG_PARAMETERS,
                                      "logdir", 6,
                                      nsconf.home, "logs", NS_FALSE, NS_FALSE);

    nsconf.binDir = Ns_ConfigFilename(NS_GLOBAL_CONFIG_PARAMETERS,
                                      "bindir", 6,
                                      nsconf.home, "bin", NS_TRUE, NS_TRUE);
    /*
     * Assure log directory is available since it is expected
     * from some subsystems (tclhttp, log, ...).
     */
    if (Ns_RequireDirectory(nsconf.logDir) != NS_OK) {
        Ns_Fatal("nsmain: log directory '%s' could not be created", nsconf.logDir);
    }

    nsconf.reject_already_closed_or_detached_connection =
        Ns_ConfigBool(NS_GLOBAL_CONFIG_PARAMETERS, "rejectalreadyclosedconn", NS_TRUE);
    nsconf.sanitize_logfiles =
        Ns_ConfigIntRange(NS_GLOBAL_CONFIG_PARAMETERS, "sanitizelogfiles", 2, 0, 3);
    /*
     * Old-style, backward compatible. Can be overridden by "ns/params/reverseproxy"
     */
    nsconf.reverseproxymode.enabled =
        Ns_ConfigBool(NS_GLOBAL_CONFIG_PARAMETERS, "reverseproxymode", NS_FALSE);

    /*
     * New-style reverse proxy server configuration. Use old-style value as default.
     */
    nsconf.reverseproxymode.enabled =
        Ns_ConfigBool(configParametersReverseproxySection, "enabled", nsconf.reverseproxymode.enabled);
    nsconf.reverseproxymode.skipnonpublic =
        Ns_ConfigBool(configParametersReverseproxySection, "skipnonpublic", NS_FALSE);
    nsconf.reverseproxymode.trustedservers =
        Ns_ConfigGetValue(configParametersReverseproxySection, "trustedservers");

    if (nsconf.reverseproxymode.trustedservers != NULL) {
        if (*nsconf.reverseproxymode.trustedservers == '\0') {
            nsconf.reverseproxymode.trustedservers = NULL;
        } else {
            nsconf.reverseproxymode.trustedservers = ns_strdup(nsconf.reverseproxymode.trustedservers);
        }
    }

    {
        /*
         * Allow values like "none", or abbreviated to "no"), but be open for
         * future enhancements like e.g. "cluster".
         */
        const char *valueString = Ns_ConfigString(NS_GLOBAL_CONFIG_PARAMETERS, "cachingmode", "full");
        if (strcmp(valueString, "full") == 0) {
            nsconf.nocache = NS_FALSE;
        } else {
            nsconf.nocache = (strncmp(valueString, "no", 2) == 0);
        }
    }

    /*
     * Make the result queryable.
     */
    set = Ns_ConfigCreateSection(NS_GLOBAL_CONFIG_PARAMETERS);
    Ns_SetIUpdateSz(set, "home", 4, nsconf.home, TCL_INDEX_NONE);

    /*
     * Update core config values.
     */

    NsConfUpdate();

    nsconf.tmpDir = ns_strcopy(Ns_ConfigGetValue(NS_GLOBAL_CONFIG_PARAMETERS, "tmpdir"));
    if (nsconf.tmpDir == NULL) {
        size_t dirNameLength;

        nsconf.tmpDir = getenv("TMPDIR");
        if (nsconf.tmpDir == NULL) {
            nsconf.tmpDir = P_tmpdir;
        }
        dirNameLength = strlen(nsconf.tmpDir);
        if (nsconf.tmpDir[dirNameLength-1] == '/') {
            char *tmpDirName = ns_strdup(nsconf.tmpDir);

            tmpDirName[dirNameLength-1] = '\0';
            nsconf.tmpDir = tmpDirName;
        }

        Ns_SetIUpdateSz(set, "tmpdir", 6, nsconf.tmpDir, TCL_INDEX_NONE);
    }

#ifdef _WIN32

    /*
     * Set the procname used for the pid file.
     */

    procname = ((server != NULL) ? server : Ns_SetKey(servers, 0));

    /*
     * Connect to the service control manager if running
     * as a service (see service comment above).
     */

    if (mode == 'I' || mode == 'R' || mode == 'S') {
        Ns_ReturnCode status = NS_OK;

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
        default:
            /* cannot happen */
            assert(0);
            break;
        }
        return (status == NS_OK ? 0 : 1);
    }

 contservice:

#endif

    /*
     * Open the log file now that the home directory and run time
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

    StatusMsg(starting_state);
    LogTclVersion();

#ifndef _WIN32
    {
        struct rlimit rl;

        /*
         * Log the current open file limit.
         */
        memset(&rl, 0, sizeof(rl));

        if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
            Ns_Log(Warning, "nsmain: "
                   "getrlimit(RLIMIT_NOFILE) failed: '%s'", strerror(errno));
        } else {
            char curBuffer[TCL_INTEGER_SPACE], maxBuffer[TCL_INTEGER_SPACE];

            snprintf(curBuffer, sizeof(curBuffer), "%" PRIuMAX, (uintmax_t)rl.rlim_cur);
            snprintf(maxBuffer, sizeof(maxBuffer), "%" PRIuMAX, (uintmax_t)rl.rlim_max);
            Ns_Log(Notice, "nsmain: "
                   "max files: soft limit %s, hard limit %s",
                   (rl.rlim_cur == RLIM_INFINITY ? "infinity" : curBuffer),
                   (rl.rlim_max == RLIM_INFINITY ? "infinity" : maxBuffer)
                   );
            if (rl.rlim_cur == RLIM_INFINITY
                || rl.rlim_cur > FD_SETSIZE) {
                Ns_Log(Warning, "nsmain: current limit "
                       "of maximum number of files > FD_SETSIZE (%d), "
                       "select() calls should not be used",
                       FD_SETSIZE);
            }
        }
    }
#endif

    //(void)Ns_LogSeveritySetEnabled(Debug, NS_TRUE);

    /*
     * Create the pid file.
     */

    NsCreatePidFile();

    /*
     * Initialize the virtual servers.
     *
     * Set the default server before the init scripts to make it accessible
     * from there.
     */

    if (server != NULL) {
        nsconf.defaultServer = server;
        NsInitServer(server, initProc);

    } else {
        size_t i;

        /*
         * Make the first server the default server.
         */
        nsconf.defaultServer = Ns_SetKey(servers, 0);

        for (i = 0u; i < Ns_SetSize(servers); ++i) {
            server = Ns_SetKey(servers, i);
            NsInitServer(server, initProc);

        }
    }

    /*
     * Initialize non-server static modules.
     */

    NsInitStaticModules(NULL);

    /*
     * Run pre-startup procs
     */

    NsRunPreStartupProcs();

    /*
     * Map virtual servers. This requires that all servers and all drivers are
     * initialized (can be found via global data structures). Drivers are
     * created via NsRunPreStartupProcs().
     */

    NsDriverMapVirtualServers();

    /*
     * Start the servers and drivers.
     */

    NsStartServers();
    NsStartDrivers();

    /*
     * Signal startup is complete.
     */

    StatusMsg(running_state);

    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.started = NS_TRUE;
    Ns_CondBroadcast(&nsconf.state.cond);
    Ns_MutexUnlock(&nsconf.state.lock);

    if (mode != 'w' && nsconf.state.pipefd[1] != 0) {
        ssize_t nwrite;

        /*
         * Tell the parent process, that initialization went OK.
         */
        nwrite = ns_write(nsconf.state.pipefd[1], "O", 1);
        if (nwrite < 1) {
            Ns_Fatal("nsmain: can't communicate with parent process, nwrite %" PRIdz
                     ", error: %s (parent process was probably killed)",
                     nwrite, strerror(errno));
        }
        (void)ns_close(nsconf.state.pipefd[1]);
        nsconf.state.pipefd[1] = 0;
    }

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
     * timeout for all systems to complete shutdown.
     * If SIGQUIT signal was sent, make immediate shutdown
     * without waiting for all subsystems to exit gracefully
     */

    StatusMsg(stopping_state);

    Ns_MutexLock(&nsconf.state.lock);
    nsconf.state.stopping = NS_TRUE;
    if (sig == NS_SIGQUIT) {
        nsconf.shutdowntimeout.sec = 0;
        nsconf.shutdowntimeout.usec = 0;
    }
    Ns_GetTime(&timeout);
    Ns_IncrTime(&timeout, nsconf.shutdowntimeout.sec, nsconf.shutdowntimeout.usec);
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
     * In case, the callbacks above require spool operations, they might need
     * still working spoolers. But they are finished now and we can stop the
     * spoolers.
     */
    NsStopSpoolers();

    /*
     * Remove the pid maker file, print a final "server exiting"
     * status message and return to main.
     */

    NsRemovePidFile();
    StatusMsg(exiting_state);

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

Ns_ReturnCode
Ns_WaitForStartup(void)
{
    /*
     * This dirty-read is worth the effort.
     */

    if (unlikely(!nsconf.state.started)) {
        Ns_MutexLock(&nsconf.state.lock);
        while (nsconf.state.started == NS_FALSE) {
            Ns_CondWait(&nsconf.state.cond, &nsconf.state.lock);
        }
        Ns_MutexUnlock(&nsconf.state.lock);
    }
    return NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_StopServer --
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
 *      Implements "ns_shutdown". Shutdown the server, waiting at most timeout
 *      seconds for threads to exit cleanly before giving up.
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
NsTclShutdownObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, TCL_SIZE_T objc, Tcl_Obj *const* objv)
{
    int         sig = NS_SIGTERM, result = TCL_OK;
    Ns_Time    *timeoutPtr = NULL;
    Ns_ObjvSpec opts[] = {
        {"-restart", Ns_ObjvBool,  &sig, INT2PTR(NS_SIGINT)},
        {"--",       Ns_ObjvBreak, NULL, NULL},
        {NULL,       NULL,         NULL, NULL}
    };
    Ns_ObjvSpec args[] = {
        {"?timeout", Ns_ObjvTime, &timeoutPtr, NULL},
        {NULL,       NULL,        NULL,        NULL}
    };

    if (Ns_ParseObjv(opts, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;

    } else if (timeoutPtr != NULL && timeoutPtr->sec < 0) {
        Ns_TclPrintfResult(interp, "timeout must be >= 0");
        result = TCL_ERROR;

    } else {

        Ns_MutexLock(&nsconf.state.lock);
        if (timeoutPtr != NULL) {
            nsconf.shutdowntimeout.sec = timeoutPtr->sec;
            nsconf.shutdowntimeout.usec = timeoutPtr->usec;
        } else {
            timeoutPtr = &nsconf.shutdowntimeout;
        }
        Ns_MutexUnlock(&nsconf.state.lock);

        NsSendSignal(sig);
        Tcl_SetObjResult(interp, Ns_TclNewTimeObj(timeoutPtr));
    }
    return result;
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
    const char *what = ""; /* Just to make compiler silent, we have a complete enumeration of switch values */

    switch (state) {

    case starting_state:
        what = "starting";
        break;

    case running_state:
        what = "running";
        break;

    case stopping_state:
        what = "stopping";
        break;

    case exiting_state:
        what = "exiting";
        break;
    }
    Ns_Log(Notice, "nsmain: %s/%s (%s) %s",
           Ns_InfoServerName(), Ns_InfoServerVersion(), Ns_InfoTag(), what);
#ifndef _WIN32
    if (state == starting_state || state == running_state) {
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
UsageError(const char *msg, ...)
{
    va_list ap;

    va_start(ap, msg);
    fprintf(stderr, "\nError: ");
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    UsageMsg(1);
}

static void
UsageMsg(int exitCode)
{
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
        "  -b  bind <address:port>  (Example: 192.168.0.1:80,[::1]:80)\n"
        "  -B  bind address:port list from <file>\n"
#endif
        "  -s  use server named <server> in configuration file\n"
        "  -t  read configuration file from <file>\n"
        "  -T  just check configuration file (without starting server)\n"
        "\n", nsconf.argv0);
    exit(exitCode);
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

static const char *
MakePath(const char *file)
{
    const char *result = NULL;

    NS_NONNULL_ASSERT(file != NULL);

    if (Ns_PathIsAbsolute(nsconf.nsd) == NS_TRUE) {
        const char *str = strstr(nsconf.nsd, "/bin/");

        if (str == NULL) {
            str = strrchr(nsconf.nsd, INTCHAR('/'));
        }
        if (str != NULL) {
            const char *path = NULL;
            Tcl_Obj    *obj;

            /*
             * Make sure we have valid path on all platforms
             */
            obj = Tcl_NewStringObj(nsconf.nsd, (TCL_SIZE_T)(str - nsconf.nsd));
            Tcl_AppendStringsToObj(obj, "/", file, NS_SENTINEL);

            Tcl_IncrRefCount(obj);
            if (Tcl_FSGetNormalizedPath(NULL, obj) != NULL) {
                path = Tcl_FSGetTranslatedStringPath(NULL, obj);
            }
            Tcl_DecrRefCount(obj);

            /*
             * If filename was given, check if the file exists
             */
            if (path != NULL && *file != '\0' && access(path, F_OK) != 0) {
                ckfree((void *)path);
                path = NULL;
            }
            result = path;
        }
    }
    return result;
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

static const char *
SetCwd(const char *path)
{
    Tcl_Obj *pathObj;

    NS_NONNULL_ASSERT(path != NULL);

    pathObj = Tcl_NewStringObj(path, TCL_INDEX_NONE);
    Tcl_IncrRefCount(pathObj);
    if (Tcl_FSChdir(pathObj) == -1) {
        Ns_Fatal("nsmain: chdir(%s) failed: '%s'", path, strerror(Tcl_GetErrno()));
    }
    Tcl_DecrRefCount(pathObj);
    pathObj = Tcl_FSGetCwd(NULL);
    if (pathObj == NULL) {
        Ns_Fatal("nsmain: can't resolve home directory path");
    }

    return Tcl_FSGetTranslatedStringPath(NULL, pathObj);
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
    const Args *cmd = arg;

    Ns_ThreadSetName("-command-");

    (void)Ns_WaitForStartup();

    NsRestoreSignals();
    NsBlockSignal(NS_SIGPIPE);

#if defined(__APPLE__) && defined(__MACH__)
    signal(SIGPIPE, SIG_IGN);
#endif

    Tcl_Main((TCL_SIZE_T)cmd->argc, cmd->argv, NsTclAppInit);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
