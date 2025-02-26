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
 * thread.h --
 *
 *      Private nsthread library include.
 *
 */

#ifndef THREAD_H
#define THREAD_H

#include "nsthread.h"

extern void   NsthreadsInit(void);
extern void   NsInitThreads(void);
extern void   NsInitMaster(void);
extern void   NsInitReentrant(void);
extern void   NsMutexInitNext(Ns_Mutex *mutex, const char *prefix, uintptr_t *nextPtr)
  NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
extern void  *NsGetLock(Ns_Mutex *mutex)   NS_GNUC_NONNULL(1);
extern void  *NsLockAlloc(void)            NS_GNUC_RETURNS_NONNULL;
extern void   NsLockFree(void *lock)       NS_GNUC_NONNULL(1);
extern void   NsLockSet(void *lock)        NS_GNUC_NONNULL(1);
extern bool   NsLockTry(void *lock)        NS_GNUC_NONNULL(1);
extern void   NsLockUnset(void *lock)      NS_GNUC_NONNULL(1);
extern void   NsCleanupTls(void **slots)   NS_GNUC_NONNULL(1);
extern void **NsGetTls(void)               NS_GNUC_RETURNS_NONNULL;
extern void   NsThreadMain(void *arg)      NS_GNUC_NORETURN;
extern void   NsCreateThread(void *arg, ssize_t stacksize, Ns_Thread *threadPtr);
extern void   NsThreadExit(void *arg)      NS_GNUC_NORETURN;
extern void  *NsThreadResult(void *arg)    NS_GNUC_CONST;
extern void   NsThreadFatal(const char *func, const char *osfunc, int err)
#ifndef NS_TCL_PRE86
  NS_GNUC_NORETURN
#endif
  ;
extern void   NsThreadShutdownStarted(void);
extern const char *NsThreadLibName(void)   NS_GNUC_CONST;
extern pid_t  Ns_Fork(void);



#endif /* THREAD_H */
