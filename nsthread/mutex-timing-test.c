/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (C) 2026 Gustaf Neumann
 */

/* Portable condition-wait/hold-time regressions, linked only to libnsthread. */
#include "nsthread.h"
#ifdef _WIN32
# include <io.h>
# define DUP _dup
# define DUP2 _dup2
# define CLOSE _close
# define FILENO _fileno
#else
# include <unistd.h>
# define DUP dup
# define DUP2 dup2
# define CLOSE close
# define FILENO fileno
#endif

#ifndef NS_NO_MUTEX_TIMING
#define CHECK(expr) Check((expr), #expr, __LINE__)
static unsigned int checks;

typedef struct Test {
    Ns_Mutex mutex;
    Ns_Cond cond;
    bool ready;
    int holdMs;
} Test;

static void
Check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        printf("FAIL mutex timing line %d: %s\n", line, expression);
        exit(EXIT_FAILURE);
    }
}

static double
Seconds(const Ns_Time *timePtr)
{
    return (double)timePtr->sec + (double)timePtr->usec / 1000000.0;
}

static double
TotalHeld(const char *name)
{
    Tcl_DString ds;
    TCL_SIZE_T count, i;
    const char **rows;
    double result = -1.0;

    Tcl_DStringInit(&ds);
    Ns_MutexList(&ds);
    CHECK(Tcl_SplitList(NULL, ds.string, &count, &rows) == TCL_OK);
    for (i = 0; i < count; ++i) {
        TCL_SIZE_T fields;
        const char **values;

        CHECK(Tcl_SplitList(NULL, rows[i], &fields, &values) == TCL_OK);
        if (fields == 8 && strcmp(values[0], name) == 0) {
            result = strtod(values[7], NULL);
        }
        Tcl_Free((char *)values);
    }
    Tcl_Free((char *)rows);
    Tcl_DStringFree(&ds);
    CHECK(result >= 0.0);
    return result;
}

static unsigned int
Occurrences(const char *text, const char *needle)
{
    unsigned int count = 0u;

    while ((text = strstr(text, needle)) != NULL) {
        ++count;
        text += strlen(needle);
    }
    return count;
}

static void
Signaler(void *arg)
{
    Test *testPtr = arg;

    Ns_ThreadSetName("-mutex-signaler-");
    Tcl_Sleep(300);
    Ns_MutexLock(&testPtr->mutex);
    testPtr->ready = NS_TRUE;
    Ns_CondSignal(&testPtr->cond);
    /* Force the woken waiter to reacquire a mutex still owned by this thread. */
    Tcl_Sleep(testPtr->holdMs);
    Ns_MutexUnlock(&testPtr->mutex);
}

/* Sleep time and other owners' held intervals must not be charged to waiter. */
static void
WaitCase(const char *name, bool signal, bool timed, int holdMs)
{
    Test test = {0};
    Ns_Thread thread;
    Ns_Time start, end, deadline;
    double held, elapsed;

    test.holdMs = holdMs;
    Ns_MutexInit(&test.mutex);
    Ns_MutexSetName(&test.mutex, name);
    Ns_CondInit(&test.cond);
    Ns_GetTime(&start);
    Ns_MutexLock(&test.mutex);
    Tcl_Sleep(holdMs);
    if (signal) {
        Ns_ThreadCreate(Signaler, &test, 0, &thread);
    }
    Ns_GetTime(&deadline);
    Ns_IncrTime(&deadline, signal ? 5 : 0, signal ? 0 : 300000);
    while (!test.ready) {
        if (timed) {
            Ns_ReturnCode status = Ns_CondTimedWait(&test.cond, &test.mutex, &deadline);
            if (status == NS_TIMEOUT) {
                CHECK(!signal);
                break;
            }
            CHECK(status == NS_OK);
        } else {
            Ns_CondWait(&test.cond, &test.mutex);
        }
    }
    CHECK(test.ready == signal);
    Tcl_Sleep(holdMs);
    Ns_MutexUnlock(&test.mutex);
    Ns_GetTime(&end);
    if (signal) {
        Ns_ThreadJoin(&thread, NULL);
    }
    held = TotalHeld(name);
    elapsed = Seconds(&end) - Seconds(&start);
    CHECK(held >= (double)(holdMs * (signal ? 3 : 2)) / 1000.0 - 0.015);
    CHECK(elapsed - held >= 0.20);
    Ns_CondDestroy(&test.cond);
    Ns_MutexDestroy(&test.mutex);
}

static void
ExpiredAndTryLock(void)
{
    Ns_Mutex mutex = NULL;
    Ns_Cond cond = NULL;
    Ns_Time deadline;
    double before, after;

    Ns_MutexInit(&mutex);
    Ns_MutexSetName(&mutex, "timing-expired");
    Ns_CondInit(&cond);
    Ns_MutexLock(&mutex);
    Tcl_Sleep(130);
    Ns_GetTime(&deadline);
    --deadline.sec;
    CHECK(Ns_CondTimedWait(&cond, &mutex, &deadline) == NS_TIMEOUT);
    Tcl_Sleep(130);
    Ns_MutexUnlock(&mutex);
    CHECK(TotalHeld("timing-expired") >= 0.25);
    Ns_CondDestroy(&cond);
    Ns_MutexDestroy(&mutex);

    Ns_MutexInit(&mutex);
    Ns_MutexSetName(&mutex, "timing-trylock");
    Ns_MutexLock(&mutex);
    Ns_MutexUnlock(&mutex);
    before = TotalHeld("timing-trylock");
    Tcl_Sleep(300);
    CHECK(Ns_MutexTryLock(&mutex) == NS_OK);
    Tcl_Sleep(130);
    Ns_MutexUnlock(&mutex);
    after = TotalHeld("timing-trylock");
    CHECK(after - before >= 0.12);
    CHECK(after - before < 0.30);
    Ns_MutexDestroy(&mutex);
}
#endif

int
main(int argc, char **argv)
{
    (void)argc;
    Tcl_FindExecutable(argv[0]);
    Nsthreads_LibInit();
#ifndef NS_NO_MUTEX_TIMING
    {
        FILE *trace = tmpfile();
        int saved;
        char buffer[16384];
        size_t length;

        CHECK(trace != NULL);
        saved = DUP(FILENO(stderr));
        CHECK(saved >= 0);
        CHECK(DUP2(FILENO(trace), FILENO(stderr)) >= 0);
        NS_mutexlocktrace = NS_TRUE;
        Ns_ThreadSetName("-mutex-timing-test-");
        WaitCase("timing-idle-timeout", NS_FALSE, NS_TRUE, 0);
        WaitCase("timing-idle-signal", NS_TRUE, NS_FALSE, 0);
        WaitCase("timing-idle-timed-signal", NS_TRUE, NS_TRUE, 0);
        WaitCase("timing-long-signal", NS_TRUE, NS_FALSE, 130);
        WaitCase("timing-long-timeout", NS_FALSE, NS_TRUE, 130);
        WaitCase("timing-long-timed-signal", NS_TRUE, NS_TRUE, 130);
        ExpiredAndTryLock();
        NS_mutexlocktrace = NS_FALSE;
        fflush(stderr);
        CHECK(DUP2(saved, FILENO(stderr)) >= 0);
        CLOSE(saved);
        rewind(trace);
        length = fread(buffer, 1, sizeof(buffer) - 1u, trace);
        buffer[length] = '\0';
        CHECK(!ferror(trace));
        CHECK(strstr(buffer, "Mutex unlock timing-idle-") == NULL);
        CHECK(Occurrences(buffer, "Mutex unlock timing-long-signal: lock duration") == 3u);
        CHECK(Occurrences(buffer, "Mutex unlock timing-long-timeout: lock duration") == 2u);
        CHECK(Occurrences(buffer, "Mutex unlock timing-long-timed-signal: lock duration") == 3u);
        CHECK(strstr(buffer, "Mutex unlock timing-expired: lock duration") != NULL);
        CHECK(strstr(buffer, "Mutex unlock timing-trylock: lock duration") != NULL);
        fclose(trace);
        printf("PASS: %u mutex timing checks\n", checks);
    }
#else
    puts("SKIP: mutex timing disabled (NS_NO_MUTEX_TIMING)");
#endif
    return EXIT_SUCCESS;
}
