/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

/* Public-header and libnsthread-only atomic API regression tests. */
#include "nsthread.h"

#define WORKERS 4
#define ITERATIONS 20000u
#define PUBLICATIONS 5000u
#define CHECK(expression) Check((expression), #expression, __LINE__)

static unsigned int checks;

static void Check(bool condition, const char *expression, int line) NS_GNUC_NONNULL(2);
static void CountWorker(void *arg) NS_GNUC_NONNULL(1);
static void PublishWorker(void *arg) NS_GNUC_NONNULL(1);
static void ConsumeWorker(void *arg) NS_GNUC_NONNULL(1);

typedef struct Counters {
    Ns_AtomicUint32 count32;
    Ns_AtomicUint32 remaining;
    Ns_AtomicUint64 count64;
} Counters;

typedef struct CountResult {
    Counters *counters;
    uint64_t sum32;
    uint64_t sum64;
    uint64_t sumRemaining;
} CountResult;

typedef struct Publication {
    Ns_AtomicUint32 ready;
    uint32_t value;
    uint32_t inverse;
    time_t deadline;
    bool producerFailed;
    bool consumerFailed;
} Publication;

static void
Check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        ns_fprintf(stderr, "atomic test failed at line %d: %s\n", line, expression);
        exit(EXIT_FAILURE);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * CountWorker --
 *
 *      Exercise competing read-modify-write operations and retain each
 *      worker's returned-value sums for verification after joining.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Updates shared atomic counters and this worker's private result.
 *
 *----------------------------------------------------------------------
 */
static void
CountWorker(void *arg)
{
    CountResult *result = arg;
    unsigned int i;

    for (i = 0; i < ITERATIONS; ++i) {
        result->sum32 += Ns_AtomicUint32FetchAddRelaxed(&result->counters->count32, 1u);
        result->sum64 += Ns_AtomicUint64FetchAddRelaxed(&result->counters->count64, 1u)
            - (UINT64_C(1) << 40);
        result->sumRemaining += Ns_AtomicUint32FetchSubAcqRel(&result->counters->remaining, 1u);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * WaitFor --
 *
 *      Wait for a publication flag, with a shared abort value and a bounded
 *      timeout so a broken synchronization implementation cannot hang tests.
 *
 * Results:
 *      True on observing the requested value, false on abort or timeout.
 *
 * Side effects:
 *      Yields periodically; publishes an abort flag on timeout.
 *
 *----------------------------------------------------------------------
 */
static bool
WaitFor(Publication *data, uint32_t expected)
{
    unsigned int spins = 0u;

    for (;;) {
        uint32_t value = Ns_AtomicUint32LoadAcquire(&data->ready);

        if (value == expected) {return true;}
        if (value == 2u) {return false;}
        if ((++spins % 256u) == 0u) {
            if (time(NULL) > data->deadline) {
                Ns_AtomicUint32StoreRelease(&data->ready, 2u);
                return false;
            }
            Ns_ThreadYield();
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * PublishWorker, ConsumeWorker --
 *
 *      Hand off ordinary, non-atomic payload fields using acquire/release
 *      operations. Alternate stores and bitwise release operations to cover
 *      each publication path. Only the consumer checks the payload.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Exchanges payloads and records failures in per-worker fields.
 *
 *----------------------------------------------------------------------
 */
static void
PublishWorker(void *arg)
{
    Publication *data = arg;
    uint32_t i;

    for (i = 1u; i <= PUBLICATIONS; ++i) {
        if (!WaitFor(data, 0u)) {data->producerFailed = true; return;}
        data->value = i;
        data->inverse = ~i;
        if ((i & 1u) == 0u) {
            Ns_AtomicUint32StoreRelease(&data->ready, 1u);
        } else {
            (void)Ns_AtomicUint32FetchOrRelease(&data->ready, 1u);
        }
    }
}

static void
ConsumeWorker(void *arg)
{
    Publication *data = arg;
    uint32_t i;

    for (i = 1u; i <= PUBLICATIONS; ++i) {
        if (!WaitFor(data, 1u)) {data->consumerFailed = true; return;}
        if (data->value != i || data->inverse != ~i) {data->consumerFailed = true;}
        if ((i & 1u) == 0u) {
            Ns_AtomicUint32StoreRelease(&data->ready, 0u);
        } else {
            (void)Ns_AtomicUint32FetchAndRelease(&data->ready, ~UINT32_C(1));
        }
    }
}

int
main(int argc, char **argv)
{
    Ns_AtomicUint32 value32;
    Ns_AtomicUint64 value64;
    Counters counters;
    CountResult results[WORKERS] = {{0}};
    Publication publication = {0};
    Ns_Thread threads[WORKERS], producer, consumer;
    uint64_t n = WORKERS * ITERATIONS;
    uint64_t sum32 = 0u, sum64 = 0u, sumRemaining = 0u;
    unsigned int i;

    (void)argc;
    Tcl_FindExecutable(argv[0]);
    Nsthreads_LibInit();
    Ns_ThreadSetName("-atomic-test-");

    Ns_AtomicUint32Init(&value32, UINT32_MAX);
    CHECK(Ns_AtomicUint32LoadRelaxed(&value32) == UINT32_MAX);
    CHECK(Ns_AtomicUint32FetchAddRelaxed(&value32, 2u) == UINT32_MAX);
    CHECK(Ns_AtomicUint32LoadRelaxed(&value32) == 1u);
    CHECK(Ns_AtomicUint32ExchangeRelaxed(&value32, UINT32_C(0x80000000)) == 1u);
    CHECK(Ns_AtomicUint32FetchSubAcqRel(&value32, UINT32_C(0x80000000)) == UINT32_C(0x80000000));
    CHECK(Ns_AtomicUint32LoadAcquire(&value32) == 0u);
    Ns_AtomicUint32StoreRelaxed(&value32, UINT32_C(0x80000000));
    CHECK(Ns_AtomicUint32FetchOrRelaxed(&value32, 3u) == UINT32_C(0x80000000));
    CHECK(Ns_AtomicUint32FetchAndRelaxed(&value32, 3u) == UINT32_C(0x80000003));
    CHECK(Ns_AtomicUint32FetchOrRelease(&value32, 4u) == 3u);
    CHECK(Ns_AtomicUint32FetchAndRelease(&value32, 6u) == 7u);
    CHECK(Ns_AtomicUint32LoadAcquire(&value32) == 6u);
    Ns_AtomicUint32StoreRelease(&value32, UINT32_MAX);
    CHECK(Ns_AtomicUint32LoadAcquire(&value32) == UINT32_MAX);
#ifdef NS_ATOMIC_MUTEX_FALLBACK
    CHECK(value32.lock != NULL);
    CHECK(strcmp(Ns_MutexGetName(&value32.lock), "atomic") == 0);
#endif
    Ns_AtomicUint32Destroy(&value32);
    Ns_AtomicUint32Init(&value32, 42u);
    CHECK(Ns_AtomicUint32LoadRelaxed(&value32) == 42u);
    Ns_AtomicUint32Destroy(&value32);

    Ns_AtomicUint64Init(&value64, UINT64_MAX);
    CHECK(Ns_AtomicUint64LoadRelaxed(&value64) == UINT64_MAX);
    CHECK(Ns_AtomicUint64FetchAddRelaxed(&value64, 2u) == UINT64_MAX);
    CHECK(Ns_AtomicUint64LoadRelaxed(&value64) == 1u);
#ifdef NS_ATOMIC_MUTEX_FALLBACK
    CHECK(value64.lock != NULL);
    CHECK(strcmp(Ns_MutexGetName(&value64.lock), "atomic64") == 0);
#endif
    Ns_AtomicUint64Destroy(&value64);
    Ns_AtomicUint64Init(&value64, UINT64_C(1) << 40);
    CHECK(Ns_AtomicUint64LoadRelaxed(&value64) == (UINT64_C(1) << 40));
    Ns_AtomicUint64Destroy(&value64);

    Ns_AtomicUint32Init(&counters.count32, 0u);
    Ns_AtomicUint32Init(&counters.remaining, (uint32_t)n);
    Ns_AtomicUint64Init(&counters.count64, UINT64_C(1) << 40);
    for (i = 0; i < WORKERS; ++i) {
        results[i].counters = &counters;
        Ns_ThreadCreate(CountWorker, &results[i], 0, &threads[i]);
    }
    for (i = 0; i < WORKERS; ++i) {
        Ns_ThreadJoin(&threads[i], NULL);
        sum32 += results[i].sum32;
        sum64 += results[i].sum64;
        sumRemaining += results[i].sumRemaining;
    }
    CHECK(Ns_AtomicUint32LoadRelaxed(&counters.count32) == n);
    CHECK(Ns_AtomicUint64LoadRelaxed(&counters.count64) == (UINT64_C(1) << 40) + n);
    CHECK(Ns_AtomicUint32LoadAcquire(&counters.remaining) == 0u);
    CHECK(sum32 == n * (n - 1u) / 2u);
    CHECK(sum64 == sum32);
    CHECK(sumRemaining == n * (n + 1u) / 2u);
    Ns_AtomicUint32Destroy(&counters.count32);
    Ns_AtomicUint32Destroy(&counters.remaining);
    Ns_AtomicUint64Destroy(&counters.count64);

    Ns_AtomicUint32Init(&publication.ready, 0u);
    publication.deadline = time(NULL) + 20;
    Ns_ThreadCreate(PublishWorker, &publication, 0, &producer);
    Ns_ThreadCreate(ConsumeWorker, &publication, 0, &consumer);
    Ns_ThreadJoin(&producer, NULL);
    Ns_ThreadJoin(&consumer, NULL);
    CHECK(!publication.producerFailed && !publication.consumerFailed);
    CHECK(Ns_AtomicUint32LoadAcquire(&publication.ready) == 0u);
    CHECK(publication.value == PUBLICATIONS);
    Ns_AtomicUint32Destroy(&publication.ready);

    printf("PASS: %u atomic checks (%s)\n", checks,
#ifdef NS_ATOMIC_MUTEX_FALLBACK
           "forced mutex fallback"
#elif defined(_MSC_VER)
           "MSVC interlocked"
#elif defined(HAVE_GNU_ATOMIC_UINT32_BUILTINS) && defined(HAVE_GNU_ATOMIC_UINT64_BUILTINS)
           "native GNU atomics, 32 and 64 bits"
#else
           "configured native/mutex backends"
#endif
    );
    return EXIT_SUCCESS;
}
