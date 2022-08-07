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
 * fork.c --
 *
 *      Implements "ns_fork".
 */

#include "thread.h"


/*
 *----------------------------------------------------------------------
 *
 * ns_fork --
 *
 *      POSIX style fork(), using fork1() on Solaris if needed.
 *
 * Results:
 *      See fork(2) man page.
 *
 * Side effects:
 *      See fork(2) man page.
 *
 *----------------------------------------------------------------------
 */

pid_t
ns_fork(void)
{
#ifdef HAVE_FORK1
    return fork1();
#else
    return fork();
#endif
}

#ifdef Ns_Fork
#undef Ns_Fork
#endif

pid_t
Ns_Fork(void)
{
    return ns_fork();
}

#endif
