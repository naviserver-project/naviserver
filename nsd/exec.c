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
 * exec.c --
 *
 *      Routines for creating and waiting for child processes.
 */

#include "nsd.h"

#ifdef _WIN32

# include <process.h>
# include <versionhelpers.h>
static void Set2Argv(Ns_DString *dsPtr, const Ns_Set *env);

#else

# define ERR_DUP        (-1)
# define ERR_CHDIR      (-2)
# define ERR_EXEC       (-3)
static int ExecProc(const char *exec, const char *dir, int fdin, int fdout,
                    char **argv, char **envp)
    NS_GNUC_NONNULL(1);
#endif /* _WIN32 */


/*
 *----------------------------------------------------------------------
 * Ns_ExecProcess --
 *
 *      Execute a command in a child process.
 *
 * Results:
 *      Return pid of child process exec'ing the command or
 *      NS_INVALID_PID on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

pid_t
Ns_ExecProcess(const char *exec, const char *dir, int fdin, int fdout, char *args,
               const Ns_Set *env)
{
    NS_NONNULL_ASSERT(exec != NULL);

    return Ns_ExecArgblk(exec, dir, fdin, fdout, args, env);
}


/*
 *----------------------------------------------------------------------
 * Ns_ExecProc --
 *
 *      Execute a command in a child process.
 *
 * Results:
 *      Return pid of child process exec'ing the command or
 *      NS_INVALID_PID on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

pid_t
Ns_ExecProc(const char *exec, char **argv)
{
    NS_NONNULL_ASSERT(exec != NULL);

    return Ns_ExecArgv(exec, NULL, 0, 1, argv, NULL);
}


/*
 *----------------------------------------------------------------------
 * Ns_WaitProcess --
 *
 *      Wait for child process
 *
 * Results:
 *      Ruturn NS_OK for success and NS_ERROR for failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

Ns_ReturnCode
Ns_WaitProcess(pid_t pid)
{
    return Ns_WaitForProcessStatus(pid, NULL, NULL);
}


/*
 *----------------------------------------------------------------------
 * Ns_WaitForProcess --
 *
 *      Wait for child process.
 *
 * Results:
 *      Return NS_OK for success and NS_ERROR for failure.
 *
 * Side effects:
 *      Sets exit code in exitcodePtr if given.
 *
 *----------------------------------------------------------------------
 */
Ns_ReturnCode
Ns_WaitForProcess(pid_t pid, int *exitcodePtr)
{
    return Ns_WaitForProcessStatus(pid, exitcodePtr, NULL);
}


Ns_ReturnCode
Ns_WaitForProcessStatus(pid_t pid, int *exitcodePtr, int *waitstatusPtr)
{
    Ns_ReturnCode status = NS_OK;
#ifdef _WIN32
    HANDLE        process = (HANDLE)pid;
    DWORD         exitcode = 0u;

    if ((WaitForSingleObject(process, INFINITE) == WAIT_FAILED) ||
        (GetExitCodeProcess(process, &exitcode) != TRUE)) {
        Ns_Log(Error, "exec: failed to get process exit code: %s",
               NsWin32ErrMsg(GetLastError()));
        status = NS_ERROR;
    }
    if (CloseHandle(process) != TRUE) {
        Ns_Log(Warning, "exec: failed to close handle for process %d: %s",
               pid, NsWin32ErrMsg(GetLastError()));
        status = NS_ERROR;
    }
    if (status == NS_OK) {
        if (exitcodePtr != NULL) {
            *exitcodePtr = exitcode;
        }
        if (nsconf.exec.checkexit == TRUE && exitcode != 0u) {
            Ns_Log(Error, "exec: process %d exited with nonzero status: %d",
                   pid, exitcode);
            status = NS_ERROR;
        }
    }

#else
    int   waitstatus = 0;
    pid_t p;

    do {
        p = waitpid(pid, &waitstatus, 0);
    } while (p != pid && errno == NS_EINTR);

    if (p != pid) {
        Ns_Log(Error, "waitpid(%d) failed: %s", pid, strerror(errno));
        status = NS_ERROR;

    } else if (WIFSIGNALED(waitstatus)) {
        const char *coredump = NS_EMPTY_STRING;
#ifdef WCOREDUMP
        if (WCOREDUMP(waitstatus)) {
            coredump = " - core dumped";
        }
#endif
        if (*coredump != '\0' || waitstatusPtr == NULL) {
            Ns_Log(Error, "process %d killed with signal %d (%s)%s", pid,
                   WTERMSIG(waitstatus), strsignal(WTERMSIG(waitstatus)), coredump);
        }
    } else if (!WIFEXITED(waitstatus)) {
        Ns_Log(Error, "waitpid(%d): invalid status: %d", pid, waitstatus);
    } else {
        int exitcode = WEXITSTATUS(waitstatus);

        if (exitcode != 0) {
            Ns_Log(Warning, "process %d exited with nonzero exit code: %d",
                   pid, (int8_t)exitcode);
        }
        if (exitcodePtr != NULL) {
            *exitcodePtr = exitcode;
        }
    }

    if (waitstatusPtr != NULL) {
        *waitstatusPtr = waitstatus;
    }

#endif /* _WIN32 */

    return status;
}


/*
 *----------------------------------------------------------------------
 * Ns_ExecArgblk --
 *
 *      Execute a command in a child process using a null
 *      byte separated list of args.
 *
 * Results:
 *      Return pid of child process exec'ing the command or
 *      NS_INVALID_PID on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

pid_t
Ns_ExecArgblk(const char *exec, const char *dir, int fdin, int fdout,
              char *args, const Ns_Set *env)
{
#ifndef _WIN32
    pid_t  pid;
    char **argv, *argList[256]; /* maximum 256 arguments */

    NS_NONNULL_ASSERT(exec != NULL);

    if (args == NULL) {
        argv = NULL;
    } else {
        int i;
        /*
         * Produce a NULL terminated argv from a string containing '\0'
         * characters as separators. We could make this dynamic, but the only
         * usage within the NaviServer source tree is nscgi, which uses always
         * exactly 2 or 0 arguments.
         */
        argv = argList;
        for (i = 0; i < 255 && *args != '\0'; i++) {
            argv[i] = args;
            args += strlen(args) + 1;
        }
        argv[i] = NULL;
        if (i == 255) {
            Ns_Log(Warning, "as set up, exec accepts only 255 arguments");
        }
    }
    pid = Ns_ExecArgv(exec, dir, fdin, fdout, argv, env);
    return pid;
#else
    STARTUPINFO     si;
    PROCESS_INFORMATION pi;
    HANDLE          hCurrentProcess;
    pid_t           pid;
    Ns_DString      cds, xds, eds;
    char           *envp;
    OSVERSIONINFO   oinfo;
    const char     *cmd;

    if (exec == NULL) {
        return NS_INVALID_PID;
    }
    oinfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    if (IsWindowsXPOrGreater()) {
        cmd = "cmd.exe";
    } else {
        cmd = "command.com";
    }

    /*
     * Setup STARTUPINFO with stdin, stdout, and stderr.
     */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = (HANDLE) _get_osfhandle(_fileno(stderr));
    hCurrentProcess = GetCurrentProcess();
    if (fdout < 0) {
        fdout = 1;
    }
    if (DuplicateHandle(hCurrentProcess, (HANDLE) _get_osfhandle(fdout), hCurrentProcess,
            &si.hStdOutput, 0u, TRUE, DUPLICATE_SAME_ACCESS) != TRUE) {
        Ns_Log(Error, "exec: failed to duplicate handle: %s",
        NsWin32ErrMsg(GetLastError()));
        return NS_INVALID_PID;
    }
    if (fdin < 0) {
        fdin = 0;
    }
    if (DuplicateHandle(hCurrentProcess, (HANDLE) _get_osfhandle(fdin), hCurrentProcess,
            &si.hStdInput, 0u, TRUE, DUPLICATE_SAME_ACCESS) != TRUE) {
        Ns_Log(Error, "exec: failed to duplicate handle: %s",
        NsWin32ErrMsg(GetLastError()));
        (void) CloseHandle(si.hStdOutput);
        return NS_INVALID_PID;
    }

    /*
     * Setup the command line and environment block and create the new
     * subprocess.
     */

    Ns_DStringInit(&cds);
    Ns_DStringInit(&xds);
    Ns_DStringInit(&eds);
    if (args == NULL) {
        /* NB: exec specifies a complete cmd.exe command string. */
        Ns_DStringVarAppend(&cds, cmd, " /c ", exec, (char *)0L);
        exec = NULL;
    } else {
        char *s;

        s = args;
        while (*s != '\0') {
            size_t len;

            len = strlen(s);
            Ns_DStringNAppend(&cds, s, (int)len);
            s += len + 1u;
            if (*s != '\0') {
                Ns_DStringNAppend(&cds, " ", 1);
            }
        }
        s = Ns_NormalizePath(&xds, exec);
        while (*s != '\0') {
            if (*s == '/') {
                *s = '\\';
            }
            ++s;
        }
        exec = xds.string;
    }
    if (env == NULL) {
        envp = NULL;
    } else {
        Set2Argv(&eds, env);
        envp = eds.string;
    }
    if (CreateProcess(exec, cds.string, NULL, NULL, TRUE, 0, envp, dir, &si, &pi) != TRUE) {
        Ns_Log(Error, "exec: failed to create process: %s: %s",
        exec != NULL ? exec : cds.string, NsWin32ErrMsg(GetLastError()));
        pid = NS_INVALID_PID;
    } else {
        (void)CloseHandle(pi.hThread);
        pid = (pid_t)pi.hProcess;
    }
    Ns_DStringFree(&cds);
    Ns_DStringFree(&xds);
    Ns_DStringFree(&eds);
    (void)CloseHandle(si.hStdInput);
    (void)CloseHandle(si.hStdOutput);
    return pid;
#endif
}


/*
 *----------------------------------------------------------------------
 * Ns_ExecArgv --
 *
 *      Execute a program in a new child process.
 *
 * Results:
 *      Return pid of child process exec'ing the command or
 *      NS_INVALID_PID on failure.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

pid_t
Ns_ExecArgv(const char *exec, const char *dir, int fdin, int fdout,
            char **argv, const Ns_Set *env)
{
#ifdef _WIN32
    /*
     * Win32 ExecArgv simply calls ExecArgblk.
     */
    pid_t           pid;
    Ns_DString      ads;
    char           *args;

    Ns_DStringInit(&ads);
    if (argv == NULL) {
        args = NULL;
    } else {
        int  i;
        for (i = 0; argv[i] != NULL; ++i) {
          Ns_DStringNAppend(&ads, argv[i], (int)strlen(argv[i]) + 1);
        }
        args = ads.string;
    }
    pid = Ns_ExecArgblk(exec, dir, fdin, fdout, args, env);
    Ns_DStringFree(&ads);

    return pid;
#else
    Ns_DString eds;
    char *argvSh[4], **envp;
    pid_t pid;

    NS_NONNULL_ASSERT(exec != NULL);

    if (argv == NULL) {
        argv = argvSh;
        argv[0] = (char *)"/bin/sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)exec;
        argv[3] = NULL;
        exec = argv[0];
    }
    Ns_DStringInit(&eds);
    if (env == NULL) {
        envp = Ns_CopyEnviron(&eds);
    } else {
        size_t i;

        for (i = 0u; i < Ns_SetSize(env); ++i) {
            Ns_DStringVarAppend(&eds,
                                Ns_SetKey(env, i), "=", Ns_SetValue(env, i), (char *)0L);
            Ns_DStringNAppend(&eds, NS_EMPTY_STRING, 1);
        }
        Ns_DStringNAppend(&eds, NS_EMPTY_STRING, 1);
        envp = Ns_DStringAppendArgv(&eds);
    }
    if (fdin < 0) {
        fdin = 0;
    }
    if (fdout < 0) {
        fdout = 1;
    }
    pid = ExecProc(exec, dir, fdin, fdout, argv, envp);
    Ns_DStringFree(&eds);

    return pid;
#endif
}

#ifndef _WIN32

/*
 *----------------------------------------------------------------------
 * ExecProc --
 *
 *      Execute a new process.  This code is careful to capture the
 *      full error status from the child on failure.
 *
 * Results:
 *      Valid new child pid or NS_INVALID_PID on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static pid_t
ExecProc(const char *exec, const char *dir, int fdin, int fdout, char **argv,
         char **envp)
{
    struct iovec iov[2];
    int    errpipe[2], errnum = 0, result = 0;
    pid_t  pid;

    /*
     * Create a pipe for child error message.
     */

    if (ns_pipe(errpipe) < 0) {
        Ns_Log(Error, "exec: ns_pipe() failed: %s", strerror(errno));
        return NS_INVALID_PID;
    }

    /*
     * Fork child and read error message (if any).
     */

    pid = ns_fork();
    if (pid < 0) {
        ns_close(errpipe[0]);
        ns_close(errpipe[1]);
        Ns_Log(Error, "exec: ns_fork() failed: %s", strerror(errno));
        return NS_INVALID_PID;
    }
    iov[0].iov_base = (void*) &result;
    iov[1].iov_base = (void*) &errnum;
    iov[0].iov_len = iov[1].iov_len = sizeof(int);
    if (pid == 0) {

        /*
         * Setup child and exec the program, writing any error back
         * to the parent if necessary.
         */

        ns_close(errpipe[0]);
        if (dir != NULL && chdir(dir) != 0) {
            //result = ERR_CHDIR;
        } else if ((fdin == 1 && (fdin = ns_dup(1)) < 0) ||
                    (fdout == 0 && (fdout = ns_dup(0)) < 0) ||
                    (fdin != 0 && ns_dup2(fdin, 0) < 0) ||
                    (fdout != 1 && ns_dup2(fdout, 1) < 0)) {
            //result = ERR_DUP;
        } else {
            if (fdin > 2) {
                ns_close(fdin);
            }
            if (fdout > 2) {
                ns_close(fdout);
            }
            NsRestoreSignals();
            (void)Ns_NoCloseOnExec(0);
            (void)Ns_NoCloseOnExec(1);
            (void)Ns_NoCloseOnExec(2);
            execve(exec, argv, envp);
            /* NB: Not reached on successful execve(). */
            //result = ERR_EXEC;
        }
        //errnum = errno;
        {
            ssize_t written = writev(errpipe[1], iov, 2);
            if (written != 2) {
                /* just ignore the attempt to write */
                ;
            }
        }

        _exit(1);

    } else {
        ssize_t nread;
        /*
         * Read result and errno from the child if any.
         */

        ns_close(errpipe[1]);
        do {
            nread = readv(errpipe[0], iov, 2);
        } while (nread < 0 && errno == NS_EINTR);
        ns_close(errpipe[0]);
        if (nread == 0) {
            //errnum = 0;
            result = pid;
        } else {
            if (nread != (sizeof(int) * 2)) {
                Ns_Log(Error, "exec: %s: error reading status from child: %s",
                           exec, strerror(errno));
            } else {
                switch (result) {
                    case ERR_CHDIR:
                        Ns_Log(Error, "exec %s: chdir(%s) failed: %s",
                                exec, dir, strerror(errnum));
                        break;
                    case ERR_DUP:
                        Ns_Log(Error, "exec %s: ns_dup() failed: %s",
                                exec, strerror(errnum));
                        break;
                    case ERR_EXEC:
                        Ns_Log(Error, "exec %s: execve() failed: %s",
                                exec, strerror(errnum));
                        break;
                    default:
                        Ns_Log(Error, "exec %s: unknown result from child: %d",
                                exec, result);
                        break;
                }
            }
            (void) waitpid(pid, NULL, 0);
        }
    }
    return result;
}

#else /* _WIN32 */



/*
 *----------------------------------------------------------------------
 * Set2Argv --
 *
 *      Convert an Ns_Set containing key-value pairs into a character
 *      array containing a sequence of name-value pairs with their
 *      terminating null bytes.
 *
 * Results:
 *      Returns pointer to a character array containing a sequence of
 *      name-value pairs with their terminating null bytes.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
Set2Argv(Ns_DString *dsPtr, const Ns_Set *env)
{
    size_t i;

    for (i = 0u; i < Ns_SetSize(env); ++i) {
        Ns_DStringVarAppend(dsPtr,
                            Ns_SetKey(env, i), "=", Ns_SetValue(env, i), (char *)0L);
        Ns_DStringNAppend(dsPtr, NS_EMPTY_STRING, 1);
    }
    Ns_DStringNAppend(dsPtr, NS_EMPTY_STRING, 1);
    (void )Ns_DStringAppendArgv(dsPtr);
}

#endif /* _WIN32 */

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
