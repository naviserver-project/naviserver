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
 * fork.c --
 *
 *	Implement ns_fork.
 */

#include "thread.h"


/*
 *----------------------------------------------------------------------
 *
 * ns_fork --
 *
 *	Posix style fork(), using fork1() on Solaris if needed.
 *
 * Results:
 *	See fork(2) man page.
 *
 * Side effects:
 *	See fork(2) man page.
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
