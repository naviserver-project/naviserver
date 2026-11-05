/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

#include "nsthread.h"

static Ns_ThreadProc Observe;
static Ns_ThreadProc Worker;
#ifndef _WIN32
static void *ForeignWorker(void *arg);
#endif

static Ns_Mutex lock;
static unsigned int observations;
static bool failed;

/* Reentrant identity/name lookups must reuse the registered TLS context. */
static void
Observe(void *arg)
{
    uint64_t tid = Ns_ThreadGetOsId();

    Ns_MutexLock(&lock);
    ++observations;
    if (arg != NULL || Ns_ThreadGetName() == NULL || Ns_ThreadGetOsId() != tid) {
        failed = NS_TRUE;
    }
    Ns_MutexUnlock(&lock);
}

static void
Worker(void *UNUSED(arg))
{
    uint64_t tid = Ns_ThreadGetOsId();

    Ns_ThreadSetName("identity-test");
    Ns_MutexLock(&lock);
    if (tid != Ns_ThreadGetOsId()) {
        failed = NS_TRUE;
    }
    Ns_MutexUnlock(&lock);
}

#ifndef _WIN32
static void *
ForeignWorker(void *arg)
{
    Worker(arg);
    return NULL;
}
#endif

int
main(int UNUSED(argc), char **argv)
{
    Ns_Thread workers[8];
    unsigned int i, expected = 8u;
#ifndef _WIN32
    pthread_t foreign;
#endif

    Tcl_FindExecutable(argv[0]);
    Nsthreads_LibInit();
    Ns_ThreadSetName("identity-main");
    Ns_MutexInit(&lock);
    Ns_ThreadSetInitProc(Observe);
    for (i = 0u; i < 8u; ++i) {
        Ns_ThreadCreate(Worker, NULL, 0, &workers[i]);
    }
    for (i = 0u; i < 8u; ++i) {
        Ns_ThreadJoin(&workers[i], NULL);
    }
#ifndef _WIN32
    if (pthread_create(&foreign, NULL, ForeignWorker, NULL) != 0
        || pthread_join(foreign, NULL) != 0) {
        return EXIT_FAILURE;
    }
    ++expected;
#endif
    Ns_ThreadSetInitProc(NULL);
    if (failed || observations != expected) {
        fprintf(stderr, "thread identity: expected %u callbacks, got %u, failed=%d\n",
                expected, observations, (int)failed);
        return EXIT_FAILURE;
    }
    printf("thread identity: %u registered threads, reentrant cached IDs passed\n", observations);
    return EXIT_SUCCESS;
}
