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


#ifndef _WIN32

/*
 * signal.c --
 *
 *      Routines for signal handling.
 */

#include "thread.h"
#include <pthread.h>

int NS_finalshutdown = 0;


/*
 *----------------------------------------------------------------------
 *
 * ns_sigmask --
 *
 *      Set the thread's signal mask.
 *
 * Results:
 *      0 on success, otherwise an error code.
 *
 * Side effects:
 *      See pthread_sigmask.
 *
 *----------------------------------------------------------------------
 */

int
ns_sigmask(int how, sigset_t *set, sigset_t *oset)
{
    NS_NONNULL_ASSERT(set != NULL);

    return pthread_sigmask(how, set, oset);
}


/*
 *----------------------------------------------------------------------
 *
 * ns_signal --
 *
 *      Install a process-wide signal handler.  Note that the handler
 *      is shared among all threads (although the signal mask is
 *      per-thread).
 *
 * Results:
 *      0 on success, -1 on error with specific error code set in errno.
 *
 * Side effects:
 *      Handler will be called when signal is received in this thread.
 *
 *----------------------------------------------------------------------
 */

int
ns_signal(int sig, void (*proc) (int))
{
    struct sigaction sa;

    sa.sa_flags = 0;
    sa.sa_handler = (void (*)(int)) proc;
    sigemptyset(&sa.sa_mask);

    return sigaction(sig, &sa, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * ns_sigwait --
 *
 *      POSIX style sigwait().
 *
 * Results:
 *      0 on success, otherwise an error code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
ns_sigwait(sigset_t *set, int *sig)
{
    NS_NONNULL_ASSERT(set != NULL);
    NS_NONNULL_ASSERT(sig != NULL);

    return sigwait(set, sig);
}
#else
/*
 * _WIN32
 *
 * We need just the definition of NS_EXTERN
 */

#include <nsthread.h>

NS_EXTERN int NS_finalshutdown;
int NS_finalshutdown = 0;
#endif
