/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

/* Deterministic tests of the production mutex implementation. Real primitive
 * locks are used, but time/thread names and diagnostic output are controlled.
 * The private copy avoids changing the production library's clock or logger.
 */
#if !defined(NS_NO_MUTEX_TIMING) && !defined(_WIN32)
#define NS_mutexlocktrace TestMutexTraceEnabled
#define Ns_MutexInit TestMutexInit
#define Ns_MutexSetName TestMutexSetName
#define Ns_MutexSetName2 TestMutexSetName2
#define Ns_MutexGetName TestMutexGetName
#define Ns_MutexDestroy TestMutexDestroy
#define Ns_MutexLock TestMutexLock
#define Ns_MutexTryLock TestMutexTryLock
#define Ns_MutexUnlock TestMutexUnlock
#define Ns_MutexList TestMutexList
#define NsMutexInitNext TestMutexInitNext
#define NsGetLock TestGetLock
#define NsMutexCondWaitBegin TestCondWaitBegin
#define NsMutexCondWaitEnd TestCondWaitEnd
#define Ns_GetTime TestGetTime
#define Ns_ThreadGetName TestThreadName
#define NsLockTry TestLockTry
#define NsLockSet TestLockSet
#define NsLockUnset TestLockUnset
#include "thread.h"
#include <stdarg.h>

static int CaptureTrace(FILE *stream, const char *format, ...) NS_GNUC_NONNULL(1,2) NS_GNUC_PRINTF(2,3);
#define fprintf CaptureTrace
#include "mutex.c"
#undef fprintf
#undef NsLockTry
#undef NsLockSet
#undef NsLockUnset

/* Native primitive functions used by the private mutex implementation. */
extern bool NsLockTry(void *lock) NS_GNUC_NONNULL(1);
extern void NsLockSet(void *lock) NS_GNUC_NONNULL(1);
extern void NsLockUnset(void *lock) NS_GNUC_NONNULL(1);

static Ns_Time now = {1000, 0};
static char owner[256] = "initial-owner";
static bool held;
static void (*afterRelease)(void);
static Ns_Mutex raceMutex;
static unsigned int checks, writes, lockedWrites;
static char output[8192];

#define CHECK(expr) Check((expr), #expr, __LINE__)
static void
Check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        printf("FAIL deferred mutex diagnostics line %d: %s\n%s", line, expression, output);
        exit(EXIT_FAILURE);
    }
}

void
TestGetTime(Ns_Time *timePtr)
{
    *timePtr = now;
}

const char *
TestThreadName(void)
{
    return owner;
}

bool
TestLockTry(void *lock)
{
    bool acquired = NsLockTry(lock);

    if (acquired) {held = NS_TRUE;}
    return acquired;
}

void
TestLockSet(void *lock)
{
    NsLockSet(lock);
    held = NS_TRUE;
}

void
TestLockUnset(void *lock)
{
    NsLockUnset(lock);
    held = NS_FALSE;
    if (afterRelease != NULL) {
        void (*callback)(void) = afterRelease;

        afterRelease = NULL;
        callback();
    }
}

static int
CaptureTrace(FILE *UNUSED(stream), const char *format, ...)
{
    va_list ap;
    size_t length = strlen(output);
    int result;

    ++writes;
    if (held) {++lockedWrites;}
    va_start(ap, format);
    result = vsnprintf(output + length, sizeof(output) - length, format, ap);
    va_end(ap);
    CHECK(result >= 0 && (size_t)result < sizeof(output) - length);
    return result;
}

static void
ResetOutput(void)
{
    CHECK(!held);
    output[0] = '\0';
    writes = lockedWrites = 0u;
}

static void
Advance(long usec)
{
    Ns_IncrTime(&now, 0, usec);
}

/* Simulate the primitive release/reacquisition inside pthread_cond_wait,
 * without an ordinary unlock that would flush the pending summary.
 */
static void
ConditionRelease(Ns_Mutex *mutex)
{
    NsMutexCondWaitBegin(mutex);
    TestLockUnset(NsGetLock(mutex));
}

static void
ConditionReturn(Ns_Mutex *mutex)
{
    TestLockSet(NsGetLock(mutex));
    NsMutexCondWaitEnd(mutex);
}

static void
CountTotalMaximumAndAttribution(void)
{
    Ns_Mutex mutex = NULL;

    ResetOutput();
    Ns_MutexInit(&mutex);
    Ns_MutexSetName(&mutex, "deferred-summary");
    strcpy(owner, "first-owner");
    Ns_MutexLock(&mutex);
    Advance(200000);
    ConditionRelease(&mutex);
    CHECK(writes == 0u);
    Advance(9000000);
    strcpy(owner, "maximum-owner");
    Ns_MutexLock(&mutex);
    Advance(500000);
    ConditionRelease(&mutex);
    CHECK(writes == 0u);
    Advance(9000000);
    /* Renaming after release must not alter the queued attribution. */
    strcpy(owner, "renamed-flusher");
    ConditionReturn(&mutex);
    Advance(15000);
    Ns_MutexUnlock(&mutex);
    CHECK(lockedWrites == 0u && writes == 1u);
    CHECK(strstr(output, "[maximum-owner] Mutex unlock deferred-summary: lock duration 0.500000") != NULL);
    CHECK(strstr(output, "deferred condition-wait holds 2, total 0.700000") != NULL);
    CHECK(strstr(output, "renamed-flusher") == NULL);
    /* A later ordinary unlock must not emit the same summary again. */
    Ns_MutexLock(&mutex);
    Advance(1);
    Ns_MutexUnlock(&mutex);
    CHECK(writes == 1u);
    Ns_MutexDestroy(&mutex);
}

static void
FlushByAnotherOwner(void)
{
    Ns_Mutex mutex = NULL;

    ResetOutput();
    Ns_MutexInit(&mutex);
    Ns_MutexSetName(&mutex, "deferred-other-owner");
    strcpy(owner, "waiting-owner");
    Ns_MutexLock(&mutex);
    Advance(300000);
    ConditionRelease(&mutex);
    Advance(9000000);
    strcpy(owner, "other-owner");
    Ns_MutexLock(&mutex);
    Advance(1);
    Ns_MutexUnlock(&mutex);
    CHECK(lockedWrites == 0u && writes == 1u);
    CHECK(strstr(output, "[waiting-owner] Mutex unlock deferred-other-owner: lock duration 0.300000") != NULL);
    CHECK(strstr(output, "deferred condition-wait holds 1, total 0.300000") != NULL);
    Ns_MutexDestroy(&mutex);
}

static void
BoundedAggregationAndThreshold(void)
{
    Ns_Mutex mutex = NULL;
    unsigned int i;
    char prefix[NS_THREAD_NAMESIZE+4];

    ResetOutput();
    Ns_MutexInit(&mutex);
    Ns_MutexSetName(&mutex, "deferred-many");
    memset(owner, 'a', sizeof(owner)-1u);
    owner[sizeof(owner)-1u] = '\0';
    prefix[0] = '[';
    memset(prefix+1, 'a', NS_THREAD_NAMESIZE);
    memcpy(prefix+1+NS_THREAD_NAMESIZE, "] ", 3u);
    Ns_MutexLock(&mutex);
    /* Exactly 100 ms is below the existing strictly-greater-than threshold. */
    Advance(100000);
    ConditionRelease(&mutex);
    ConditionReturn(&mutex);
    for (i = 0u; i < 10000u; ++i) {
        Advance(200001);
        ConditionRelease(&mutex);
        Advance(1000000);
        ConditionReturn(&mutex);
    }
    CHECK(writes == 0u);
    Advance(1);
    Ns_MutexUnlock(&mutex);
    CHECK(lockedWrites == 0u && writes == 1u);
    CHECK(strstr(output, prefix) == output);
    CHECK(strstr(output, "lock duration 0.200001") != NULL);
    CHECK(strstr(output, "deferred condition-wait holds 10000, total 2000.010000") != NULL);
    Ns_MutexDestroy(&mutex);
}

static void
TraceSwitchAndOrdinaryHold(void)
{
    Ns_Mutex mutex = NULL;

    ResetOutput();
    Ns_MutexInit(&mutex);
    Ns_MutexSetName(&mutex, "deferred-switch");
    strcpy(owner, "observed-owner");
    Ns_MutexLock(&mutex);
    Advance(200000);
    ConditionRelease(&mutex);
    NS_mutexlocktrace = NS_FALSE;
    ConditionReturn(&mutex);
    Advance(300000);
    ConditionRelease(&mutex);
    ConditionReturn(&mutex);
    Advance(1);
    Ns_MutexUnlock(&mutex);
    CHECK(lockedWrites == 0u && writes == 1u);
    CHECK(strstr(output, "deferred condition-wait holds 1, total 0.200000") != NULL);
    ResetOutput();
    NS_mutexlocktrace = NS_TRUE;
    Ns_MutexLock(&mutex);
    Advance(400000);
    Ns_MutexUnlock(&mutex);
    CHECK(lockedWrites == 0u && writes == 1u);
    CHECK(strstr(output, "lock duration 0.400000") != NULL);
    CHECK(strstr(output, "[observed-owner] Mutex unlock deferred-switch:") != NULL);
    CHECK(strstr(output, "condition-wait holds") == NULL);
    Ns_MutexDestroy(&mutex);
}

/* Interleave a new owner after the primitive unlock but before the old owner
 * emits its snapshot. This must neither change the old record nor erase the
 * new pending summary.
 */
static void
QueueAfterRelease(void)
{
    strcpy(owner, "late-owner");
    Ns_MutexLock(&raceMutex);
    Advance(300000);
    ConditionRelease(&raceMutex);
    strcpy(owner, "first-flusher");
}

static void
SnapshotSurvivesNewOwner(void)
{
    ResetOutput();
    raceMutex = NULL;
    Ns_MutexInit(&raceMutex);
    Ns_MutexSetName(&raceMutex, "deferred-interleaving");
    strcpy(owner, "early-owner");
    Ns_MutexLock(&raceMutex);
    Advance(200000);
    ConditionRelease(&raceMutex);
    strcpy(owner, "first-flusher");
    Ns_MutexLock(&raceMutex);
    Advance(1);
    afterRelease = QueueAfterRelease;
    Ns_MutexUnlock(&raceMutex);
    CHECK(lockedWrites == 0u && writes == 1u);
    CHECK(strstr(output, "[early-owner] Mutex unlock deferred-interleaving: lock duration 0.200000") != NULL);
    CHECK(strstr(output, "deferred condition-wait holds 1, total 0.200000") != NULL);
    CHECK(strstr(output, "late-owner") == NULL);
    ResetOutput();
    Ns_MutexLock(&raceMutex);
    Advance(1);
    Ns_MutexUnlock(&raceMutex);
    CHECK(lockedWrites == 0u && writes == 1u);
    CHECK(strstr(output, "[late-owner] Mutex unlock deferred-interleaving: lock duration 0.300000") != NULL);
    CHECK(strstr(output, "deferred condition-wait holds 1, total 0.300000") != NULL);
    CHECK(strstr(output, "early-owner") == NULL);
    Ns_MutexDestroy(&raceMutex);
}

int
main(int argc, char **argv)
{
    (void)argc;
    Tcl_FindExecutable(argv[0]);
    Nsthreads_LibInit();
    NS_mutexlocktrace = NS_TRUE;
    CountTotalMaximumAndAttribution();
    FlushByAnotherOwner();
    BoundedAggregationAndThreshold();
    TraceSwitchAndOrdinaryHold();
    SnapshotSurvivesNewOwner();
    printf("PASS: %u deferred mutex diagnostic checks\n", checks);
    return EXIT_SUCCESS;
}
#else
#include "nsthread.h"
int main(void)
{
    puts("SKIP: deferred pthread diagnostics unavailable or timing disabled");
    return EXIT_SUCCESS;
}
#endif
