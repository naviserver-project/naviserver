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

#ifndef _WIN32

/*
 * watchdog.c --
 *
 *      Fork a new process and watch it's exit status, restarting it
 *      unless it exits deliberately and cleanly.
 */

#include "nsd.h"

#ifdef LOG_DEBUG
# undef LOG_DEBUG /* Because this is used by the syslog facility as well */
#endif

#include <syslog.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>


/*
 * The following values define the restart behaviour.
 */

#define MAX_RESTART_SECONDS  64 /* Max time in sec to wait between restarts */
#define MIN_WORK_SECONDS    128 /* After being up for # secs, reset timers */
#define MAX_NUM_RESTARTS    256 /* Quit after somany unsuccessful restarts */
#define WAKEUP_IN_SECONDS   600 /* Wakeup watchdog after somany seconds */


/*
 * Local functions defined in this file.
 */

static void WatchdogSIGTERMHandler(int sig);
static void WatchdogSIGALRMHandler(int sig);
static int  WaitForServer();
static void SysLog(int priority, char *fmt, ...);


/*
 * Local variables defined in this file.
 */

static pid_t watchedPid   = 0; /* PID of the server to watch. */

static int   watchdogExit = 0; /* Watchdog process should exit. */
static int   processDied  = 0; /* 1 if watched process died unexpectedly. */



/*
 *----------------------------------------------------------------------
 *
 * NsForkWatchedProcess --
 *
 *      Fork a new process and watch for it to exit. Restart it unless
 *      it exits 0 (cleanly) or we exceed the maximum number of
 *      restart attempts.
 *
 * Results:
 *      Returns 1 if caller is now the watched process and should
 *      continue about it's business.
 *
 *      Returns 0 if caller is watchdog process and should
 *      exit(0) (success).
 *
 * Side effects:
 *      Install SIGTERM handler for watchdog process.
 *
 *----------------------------------------------------------------------
 */

int
NsForkWatchedProcess()
{
    struct itimerval timer;
    unsigned int numRestarts = 0, restartWait = 0;

    SysLog(LOG_NOTICE, "watchdog: started.");

    while (watchdogExit == 0) {
        unsigned int startTime;

        if (restartWait != 0) {
            SysLog(LOG_WARNING,
                   "watchdog: waiting %d seconds before restart %d.",
                   restartWait, numRestarts);
            sleep(restartWait);
        }

        /*
         * Reset the interval timer  (see below)
         */

        if (WAKEUP_IN_SECONDS != 0) {
            memset(&timer, 0, sizeof(struct itimerval));
            setitimer(ITIMER_REAL, &timer, NULL);
            ns_signal(SIGALRM, SIG_DFL);
        }
        ns_signal(SIGTERM, SIG_DFL);

        /*
         * fork() a new process:
         */

        watchedPid = ns_fork();
        if (watchedPid == NS_INVALID_PID) {
            SysLog(LOG_ERR, "watchdog: fork() failed: '%s'.", strerror(errno));
            Ns_Fatal("watchdog: fork() failed: '%s'.", strerror(errno));
        }
        if (watchedPid == 0) {
            /* Server process. */
            SysLog(LOG_NOTICE, "server: started.");
            return getpid();
        }

        /* Watchdog process */

        /*
         * Register SIGTERM handler so we can gracefully stop the server.
         * The watchdog passes the signal to the server, if possible.
         *
         * Register SIGALRM handler to wake up the watchdog to check if
         * the server is still present. This tries to solve issues with
         * signal delivery on some systems where waitpid() fails to report
         * process exitus (i.e. just stuck, although the process is gone).
         */

        if (WAKEUP_IN_SECONDS != 0) {
            timer.it_interval.tv_sec = WAKEUP_IN_SECONDS;
            timer.it_value.tv_sec  = timer.it_interval.tv_sec;
            setitimer(ITIMER_REAL, &timer, NULL);
            ns_signal(SIGALRM, WatchdogSIGALRMHandler);
        }
        ns_signal(SIGTERM, WatchdogSIGTERMHandler);
        startTime = time(NULL);

        if (WaitForServer() == NS_OK) {
            /*
             * The server exited cleanly. We're done.
             */
            break;
        }

        /*
         * The server died. Restart it unless we've already started
         * it too many times, too frequently.
         */

        if ((time(NULL) - startTime) > MIN_WORK_SECONDS) {
            restartWait = numRestarts = 0;
        }
        if (++numRestarts > MAX_NUM_RESTARTS) {
            SysLog(LOG_WARNING, "watchdog: exceeded restart limit of %d",
                   MAX_NUM_RESTARTS);
            break;
        }

        /*
         * Wait a little longer each time we restart the server.
         */

        restartWait *= 2;
        if (restartWait > MAX_RESTART_SECONDS) {
            restartWait = MAX_RESTART_SECONDS;
        } else if (restartWait == 0) {
            restartWait = 1;
        }

    }

    SysLog(LOG_NOTICE, "watchdog: exited.");

    return 0;
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
    int    ret, status;
    pid_t  pid;
    char  *msg;

    do {
        pid = waitpid(watchedPid, &status, 0);
    } while (pid == NS_INVALID_PID && errno == EINTR && watchedPid);

    if (processDied != 0) {
        msg = "terminated";
        ret = -1; /* Alarm handler found no server present? */
    } else if (WIFEXITED(status)) {
        ret = WEXITSTATUS(status);
        msg = "exited";
    } else if (WIFSIGNALED(status)) {
        ret = WTERMSIG(status);
        msg = "terminated";
    } else {
        msg = "killed";
        ret = -1; /* Some waitpid (or other unknown) failure? */
    }

    SysLog(LOG_NOTICE, "watchdog: server %d %s (%d).", watchedPid, msg, ret);

    return (ret != 0) ? NS_ERROR : NS_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * WatchdogSIGTERMHandler --
 *
 *      Handle SIGTERM in the watchdog process and send same signal to
 *      watched process.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Watchdog will no longer try to restart the watched process.
 *
 *----------------------------------------------------------------------
 */

static void
WatchdogSIGTERMHandler(int sig)
{
    if (watchedPid != 0) {
        kill((pid_t) watchedPid, sig);
    }
    watchdogExit = 1;
}


/*
 *----------------------------------------------------------------------
 *
 * WatchdogSIGALRMHandler --
 *
 *      Handle periodic SIGALRM to check for existence of the
 *      watched process.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Sets processDied to 1 if process with watchedPid doesn't exist.
 *
 *----------------------------------------------------------------------
 */

static void
WatchdogSIGALRMHandler(int sig)
{
    if (watchedPid && kill((pid_t) watchedPid, 0) && errno == ESRCH) {
        SysLog(LOG_WARNING, "watchdog: server %d terminated?", watchedPid);
        processDied = 1;
    }
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

#endif
